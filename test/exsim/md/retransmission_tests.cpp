// RetransmissionRing and RetransmissionServer: exact resent bytes, request
// validation, ring edges, count and payload caps.

#include <md/retransmission_server.hpp>
#include <test/exsim/md/md_test_support.hpp>
#include <test/framework/test.hpp>

#include <bit>
#include <stdexcept>
#include <string>

using namespace exsim;
using namespace exsim::mdtest;

namespace {

// A recognisable message of `length` bytes for `sequence`.
Bytes fake_message(std::uint64_t sequence, std::size_t length = 12) {
    Bytes message(length, static_cast<char>('a' + sequence % 26));
    message[0] = 'S';
    return message;
}

void fill_ring(md::RetransmissionRing& ring, std::uint64_t first, std::uint64_t last, std::size_t length = 12) {
    for (std::uint64_t sequence = first; sequence <= last; ++sequence)
        REQUIRE(ring.append(sequence, fake_message(sequence, length)));
}

Bytes request_bytes(std::string_view session, std::uint64_t sequence, std::uint16_t count) {
    protocol::moldudp64::Request request;
    md::MoldSessionName name{};
    REQUIRE(md::make_mold_session_name(session, name));
    request.session = name;
    request.sequence = sequence;
    request.count = count;
    Bytes bytes(md::kMoldRequestSize);
    REQUIRE_EQ(protocol::moldudp64::encode(request, bytes.data(), bytes.size()), md::kMoldRequestSize);
    return bytes;
}

struct Responses {
    std::deque<Bytes> packets;
    md::RetransmitResult ask(md::RetransmissionServer& server, const Bytes& request) {
        return server.answer(request, PacketQueueSink{&packets});
    }
};

}  // namespace

TEST_CASE(md_retransmission, ring_append_rules) {
    bool threw = false;
    try {
        md::RetransmissionRing empty(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    md::RetransmissionRing ring(4);
    CHECK_EQ(ring.oldest_sequence(), 0u);
    CHECK_EQ(ring.newest_sequence(), 0u);
    CHECK(ring.find(1).empty());
    CHECK(!ring.append(0, fake_message(0)));
    CHECK(!ring.append(1, Bytes{}));
    CHECK(!ring.append(1, Bytes(md::kMaxItchMessageSize + 1, 'x')));
    CHECK(ring.append(1, Bytes(md::kMaxItchMessageSize, 'x')));
    CHECK(!ring.append(3, fake_message(3)));  // not consecutive
    CHECK(!ring.append(1, fake_message(1)));
    CHECK(ring.append(2, fake_message(2)));
    CHECK_EQ(ring.oldest_sequence(), 1u);
    CHECK_EQ(ring.newest_sequence(), 2u);

    // A ring may start at any sequence (the first append fixes it). The
    // capacity is rounded up to a power of two: 3 holds 4 messages.
    md::RetransmissionRing late(3);
    CHECK_EQ(late.capacity(), 4u);
    fill_ring(late, 100, 105);
    CHECK_EQ(late.oldest_sequence(), 102u);
    CHECK_EQ(late.newest_sequence(), 105u);
    CHECK(late.find(101).empty());
    CHECK_EQ(hex_of(late.find(102)), hex_of(fake_message(102)));
    CHECK_EQ(hex_of(late.find(105)), hex_of(fake_message(105)));
    CHECK(late.find(106).empty());
}

TEST_CASE(md_retransmission, ring_capacity_is_a_power_of_two) {
    for (const std::size_t requested : {1u, 2u, 5u, 64u, 65u, 100'000u}) {
        md::RetransmissionRing ring(requested);
        CHECK(std::has_single_bit(ring.capacity()));
        CHECK_GE(ring.capacity(), requested);
        CHECK_LT(ring.capacity(), 2 * requested);
    }
    // Wrapping several times keeps exactly the newest capacity() messages.
    md::RetransmissionRing ring(5);
    fill_ring(ring, 1, 8 * 3 + 5);
    CHECK_EQ(ring.oldest_sequence(), 22u);
    CHECK(ring.find(21).empty());
    for (std::uint64_t sequence = 22; sequence <= 29; ++sequence) {
        CHECK_EQ(hex_of(ring.find(sequence)), hex_of(fake_message(sequence)));
    }
}

TEST_CASE(md_retransmission, server_config_rules) {
    md::RetransmissionRing ring(4);
    md::RetransmissionConfig config;
    config.max_payload = md::kMinimumMaxPayload - 1;
    bool threw = false;
    try {
        md::RetransmissionServer server(ring, config);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    config = {};
    config.session = "ABCDEFGHIJK";
    threw = false;
    try {
        md::RetransmissionServer server(ring, config);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE(md_retransmission, answers_with_original_bytes_in_one_packet) {
    md::RetransmissionRing ring(16);
    fill_ring(ring, 1, 10);
    md::RetransmissionServer server(ring, {});
    Responses responses;
    const md::RetransmitResult result = responses.ask(server, request_bytes("EXSIM", 3, 4));
    CHECK(result.status == md::RetransmitStatus::Answered);
    CHECK_EQ(result.first_sequence, 3u);
    CHECK_EQ(result.message_count, 4u);
    REQUIRE_EQ(responses.packets.size(), 1u);
    Bytes expected = hex_bytes("45 58 53 49 4D 20 20 20 20 20 0000000000000003 0004");
    for (std::uint64_t sequence = 3; sequence <= 6; ++sequence) {
        expected.push_back(0);
        expected.push_back(12);
        const Bytes message = fake_message(sequence);
        expected.insert(expected.end(), message.begin(), message.end());
    }
    CHECK_EQ(hex_of(responses.packets[0]), hex_of(expected));
    CHECK_EQ(server.stats().requests, 1u);
    CHECK_EQ(server.stats().responses, 1u);
    CHECK_EQ(server.stats().messages_resent, 4u);
}

TEST_CASE(md_retransmission, ignores_malformed_and_foreign_requests) {
    md::RetransmissionRing ring(16);
    fill_ring(ring, 1, 10);
    md::RetransmissionServer server(ring, {});
    Responses responses;
    Bytes short_request = request_bytes("EXSIM", 1, 1);
    short_request.pop_back();
    Bytes long_request = request_bytes("EXSIM", 1, 1);
    long_request.push_back(0);
    CHECK(responses.ask(server, short_request).status == md::RetransmitStatus::MalformedRequest);
    CHECK(responses.ask(server, long_request).status == md::RetransmitStatus::MalformedRequest);
    CHECK(responses.ask(server, Bytes{}).status == md::RetransmitStatus::MalformedRequest);
    CHECK(responses.ask(server, request_bytes("EXSIM", 0, 3)).status == md::RetransmitStatus::MalformedRequest);
    CHECK(responses.ask(server, request_bytes("EXSIM", 2, 0)).status == md::RetransmitStatus::MalformedRequest);
    CHECK(responses.ask(server, request_bytes("OTHER", 2, 3)).status == md::RetransmitStatus::WrongSession);
    CHECK(responses.ask(server, request_bytes("exsim", 2, 3)).status == md::RetransmitStatus::WrongSession);
    CHECK(responses.packets.empty());
    CHECK_EQ(server.stats().ignored, 7u);
    CHECK_EQ(server.stats().responses, 0u);
}

TEST_CASE(md_retransmission, ring_edges_clip_or_refuse) {
    md::RetransmissionRing ring(4);
    md::RetransmissionServer server(ring, {});
    Responses responses;
    CHECK(responses.ask(server, request_bytes("EXSIM", 1, 1)).status == md::RetransmitStatus::NotYetPublished);

    fill_ring(ring, 1, 10);  // holds 7..10
    md::RetransmitResult result = responses.ask(server, request_bytes("EXSIM", 7, 10));
    CHECK(result.status == md::RetransmitStatus::Answered);
    CHECK_EQ(result.message_count, 4u);  // clipped to the newest
    REQUIRE_EQ(responses.packets.size(), 1u);
    CHECK_EQ(decode_packet(responses.packets[0]).count, 4u);
    CHECK_EQ(decode_packet(responses.packets[0]).sequence, 7u);

    // First message already evicted: nothing, even though 7..8 are held.
    CHECK(responses.ask(server, request_bytes("EXSIM", 6, 3)).status == md::RetransmitStatus::OutsideRing);
    CHECK(responses.ask(server, request_bytes("EXSIM", 1, 65535)).status == md::RetransmitStatus::OutsideRing);
    CHECK(responses.ask(server, request_bytes("EXSIM", 11, 1)).status == md::RetransmitStatus::NotYetPublished);
    result = responses.ask(server, request_bytes("EXSIM", 10, 1));
    CHECK(result.status == md::RetransmitStatus::Answered);
    CHECK_EQ(result.message_count, 1u);
    CHECK_EQ(responses.packets.size(), 2u);
}

TEST_CASE(md_retransmission, caps_count_and_payload) {
    md::RetransmissionRing ring(64);
    fill_ring(ring, 1, 40, 30);
    md::RetransmissionConfig config;
    config.max_messages_per_response = 3;
    md::RetransmissionServer counted(ring, config);
    Responses responses;
    md::RetransmitResult result = responses.ask(counted, request_bytes("EXSIM", 5, 10));
    CHECK(result.status == md::RetransmitStatus::Answered);
    CHECK_EQ(result.message_count, 3u);

    // 20 + 2 * (2 + 30) = 84 <= 100 < 116: two messages per response.
    config = {};
    config.max_payload = 100;
    md::RetransmissionServer sized(ring, config);
    responses.packets.clear();
    result = responses.ask(sized, request_bytes("EXSIM", 5, 10));
    CHECK_EQ(result.message_count, 2u);
    REQUIRE_EQ(responses.packets.size(), 1u);
    CHECK_LE(responses.packets[0].size(), 100u);
    const DecodedPacket decoded = decode_packet(responses.packets[0]);
    REQUIRE_EQ(decoded.messages.size(), 2u);
    CHECK_EQ(decoded.messages[1].sequence, 6u);
    CHECK_EQ(hex_of(decoded.messages[1].bytes), hex_of(fake_message(6, 30)));
}
