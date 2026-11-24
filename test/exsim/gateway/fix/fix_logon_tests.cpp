// FIX Logon acceptance and refusal, Logout handshake, logon timeout.

#include "test/exsim/gateway/fix/fix_test_support.hpp"

#include <string>

using namespace fix_test;
using exsim::gateway::fix::FixConnectionPhase;

TEST_CASE(gateway_fix_logon, accepted_logon_golden_response) {
    FixTestExchange exchange;
    FixTestConnection connection(exchange, "CLIENT1");
    connection.send("A", "98=0|108=30|554=secret1|");
    const std::string output(connection.sink().bytes());
    CHECK_EQ(with_pipes(output), with_pipes(exchange_message("A", 1, "98=0|108=30|")));
    // Literal frame with BodyLength and CheckSum computed by hand (python).
    CHECK_EQ(with_pipes(output),
             std::string("8=FIX.4.4|9=67|35=A|49=EXSIM|56=CLIENT1|34=1|52=20261103-09:30:00.000|98=0|108=30|10=100|"));
    CHECK(connection.connection().phase() == FixConnectionPhase::Active);
    CHECK(!connection.connection().wants_disconnect());
    CHECK(exchange.is_bound(1));
    CHECK_EQ(exchange.user(1).session_day_state().expected_inbound_seq(), 2u);
    CHECK_EQ(exchange.user(1).session_day_state().next_outbound_seq(), 2u);
    CHECK_EQ(connection.connection().heartbeat_interval_ns(), 30 * fix::kNanosecondsPerSecond);
}

TEST_CASE(gateway_fix_logon, reset_flag_echoed_and_sequences_restart) {
    FixTestExchange exchange;
    FixTestConnection connection(exchange, "CLIENT2");
    connection.send("A", "98=0|108=10|");
    connection.drain();
    connection.send("0", "");
    connection.disconnect();
    CHECK_EQ(exchange.user(2).session_day_state().expected_inbound_seq(), 3u);

    connection.reconnect();
    const ParsedFixMessage response = connection.logon(10, true, {});
    CHECK_EQ(response.pipes(), with_pipes(exchange_message("A", 1, "98=0|108=10|141=Y|", "CLIENT2")));
    CHECK_EQ(exchange.user(2).session_day_state().expected_inbound_seq(), 2u);
    CHECK_EQ(exchange.user(2).session_day_state().next_outbound_seq(), 2u);
    // The reset Logon is a session message: it takes no store slot.
    CHECK_EQ(exchange.user(2).session_day_state().store().last_seq(), 0u);
}

namespace {

// Feeds one Logon that the connection must refuse: it closes without a reply.
void check_logon_refused(FixTestExchange& exchange, std::string_view sender, std::string_view body,
                         std::string_view target = "EXSIM") {
    FixTestConnection connection(exchange, std::string(sender));
    connection.feed(fix_frame("35=A|49=" + std::string(sender) + "|56=" + std::string(target) +
                              "|34=1|52=" + std::string(kStartTimestamp) + "|" + std::string(body)));
    CHECK(connection.connection().wants_disconnect());
    CHECK(connection.connection().user() == nullptr);
    CHECK_EQ(with_pipes(connection.sink().bytes()), std::string());
}

}  // namespace

// A refused Logon gets no Logout. The session has no MsgSeqNum to give it, and
// QuickFIX counts a Logout's MsgSeqNum, so a Logout with 34=1 made it reject the
// next Logon reply (also 34=1) as "MsgSeqNum too low".
TEST_CASE(gateway_fix_logon, refusals_close_without_reply) {
    FixTestExchange exchange;
    check_logon_refused(exchange, "CLIENT1", "98=0|108=30|554=wrong|");
    check_logon_refused(exchange, "CLIENT1", "98=0|108=30|");
    check_logon_refused(exchange, "NOBODY", "98=0|108=30|");
    check_logon_refused(exchange, "OUCH01", "98=0|108=30|");
    check_logon_refused(exchange, "CLIENT2", "98=0|108=30|", "OTHER");
    check_logon_refused(exchange, "CLIENT2", "98=1|108=30|");
    check_logon_refused(exchange, "CLIENT2", "98=0|108=301|");
    check_logon_refused(exchange, "CLIENT2", "98=0|");
    check_logon_refused(exchange, "CLIENT2", "98=0|108=x|");
    // Nothing was bound and no day state was touched.
    CHECK(!exchange.is_bound(1));
    CHECK(!exchange.is_bound(2));
    CHECK_EQ(exchange.user(1).session_day_state().next_outbound_seq(), 1u);
    CHECK_EQ(exchange.user(2).session_day_state().expected_inbound_seq(), 1u);
}

TEST_CASE(gateway_fix_logon, second_login_refused_while_first_is_bound) {
    FixTestExchange exchange;
    FixTestConnection first(exchange, "CLIENT2");
    first.logon(30, false, {});
    check_logon_refused(exchange, "CLIENT2", "98=0|108=30|");
    CHECK(exchange.is_bound(2));
    CHECK(!first.connection().wants_disconnect());
    // The refused attempt did not consume the first connection's sequence numbers.
    CHECK_EQ(exchange.user(2).session_day_state().expected_inbound_seq(), 2u);
    CHECK_EQ(exchange.user(2).session_day_state().next_outbound_seq(), 2u);
}

TEST_CASE(gateway_fix_logon, first_message_not_logon_disconnects_silently) {
    FixTestExchange exchange;
    FixTestConnection connection(exchange, "CLIENT1");
    connection.send("0", "");
    CHECK(connection.connection().wants_disconnect());
    CHECK(connection.sink().bytes().empty());
}

TEST_CASE(gateway_fix_logon, reset_flag_requires_sequence_one) {
    FixTestExchange exchange;
    FixTestConnection connection(exchange, "CLIENT2");
    connection.send_at("A", 5, "98=0|108=30|141=Y|");
    const auto messages = connection.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(),
             with_pipes(exchange_message("5", 1, "58=ResetSeqNumFlag(141)=Y requires MsgSeqNum(34)=1|", "CLIENT2")));
    CHECK(connection.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_logon, relogon_with_low_sequence_is_logged_out) {
    FixTestExchange exchange;
    FixTestConnection connection(exchange, "CLIENT2");
    connection.logon(30, false, {});
    connection.send("0", "");
    connection.reconnect();
    CHECK(!exchange.is_bound(2));
    connection.send_at("A", 2, "98=0|108=30|");
    const auto messages = connection.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(),
             with_pipes(exchange_message("5", 2, "58=MsgSeqNum too low, expecting 3 but received 2|", "CLIENT2")));
    CHECK(connection.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_logon, logon_with_high_sequence_requests_resend) {
    FixTestExchange exchange;
    FixTestConnection connection(exchange, "CLIENT2");
    connection.send_at("A", 4, "98=0|108=30|");
    auto messages = connection.drain();
    REQUIRE_EQ(messages.size(), std::size_t{2});
    CHECK_EQ(messages[0].type(), std::string("A"));
    CHECK_EQ(messages[1].pipes(), with_pipes(exchange_message("2", 2, "7=1|16=0|", "CLIENT2")));
    CHECK(connection.connection().resend_request_outstanding());
    // The client fills 1..3 (its Logon at 4 was already processed).
    connection.send_at("4", 1, "123=Y|36=4|", true);
    CHECK_EQ(exchange.user(2).session_day_state().expected_inbound_seq(), 5u);
    CHECK(!connection.connection().resend_request_outstanding());
    connection.send_at("0", 5, "");
    CHECK_EQ(exchange.user(2).session_day_state().expected_inbound_seq(), 6u);
    CHECK(connection.drain().empty());
}

TEST_CASE(gateway_fix_logon, logon_timeout_disconnects) {
    FixTestExchange exchange;
    FixTestConnection connection(exchange, "CLIENT1");
    connection.tick(9 * fix::kNanosecondsPerSecond);
    CHECK(!connection.connection().wants_disconnect());
    connection.tick(fix::kNanosecondsPerSecond);
    CHECK(connection.connection().wants_disconnect());
}

TEST_CASE(gateway_fix_logon, client_initiated_logout_handshake) {
    FixTestExchange exchange;
    FixTestConnection connection(exchange, "CLIENT2");
    connection.logon(30, false, {});
    connection.send("5", "58=bye|");
    const auto messages = connection.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(), with_pipes(exchange_message("5", 2, "", "CLIENT2")));
    CHECK(connection.connection().wants_disconnect());
    connection.disconnect();
    CHECK(!exchange.is_bound(2));
}

TEST_CASE(gateway_fix_logon, exchange_initiated_logout_handshake_and_timeout) {
    FixTestExchange exchange;
    FixTestConnection connection(exchange, "CLIENT2");
    connection.logon(30, false, {});
    CHECK(connection.connection().begin_logout("end of day", exchange));
    auto messages = connection.drain();
    REQUIRE_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].pipes(), with_pipes(exchange_message("5", 2, "58=end of day|", "CLIENT2")));
    CHECK(connection.connection().phase() == FixConnectionPhase::LogoutSent);
    connection.send("5", "");
    CHECK(connection.drain().empty());  // no second Logout
    CHECK(connection.connection().wants_disconnect());

    FixTestConnection second(exchange, "CLIENT1");
    connection.disconnect();
    second.logon();
    CHECK(second.connection().begin_logout({}, exchange));
    second.drain();
    second.tick(fix::kNanosecondsPerSecond);
    CHECK(!second.connection().wants_disconnect());
    second.tick(fix::kNanosecondsPerSecond);
    CHECK(second.connection().wants_disconnect());
}
