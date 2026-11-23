// The admin socket on a real ServerRuntime: replies, the reactor wakeup, one
// client at a time, and the socket file. Process-level checks (signals, the
// ready line, nc-style clients) are in test/integration/exsim_exchanged_test.py.

#include "server/admin/admin_socket.hpp"
#include "server/runtime/server_runtime.hpp"
#include "test/framework/test.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {
using namespace exsim;

server::ServerConfig loopback_config_with_ephemeral_ports() {
    server::ServerConfig config;
    std::string error;
    REQUIRE(server::load_config("test/fixtures/exchange.conf", config, error));
    config.exchange.ouch_port = 0;
    config.exchange.fix_port = 0;
    config.market_data.port = 0;
    config.market_data.retransmit_port = 0;
    return config;
}

server::ServerRuntimeOptions quiet(server::ServerRuntimeOptions options) {
    options.log = nullptr;
    return options;
}

struct TemporaryDirectory {
    std::filesystem::path path;
    TemporaryDirectory() {
        char text[] = "/tmp/exsim-admin-XXXXXX";
        REQUIRE(::mkdtemp(text) != nullptr);
        path = text;
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

// A started runtime with an admin socket at <directory>/admin.sock.
struct AdminExchange {
    TemporaryDirectory directory;
    std::string path = (directory.path / "admin.sock").string();
    server::ServerRuntime runtime;
    server::AdminSocket admin{runtime};
    explicit AdminExchange(server::ServerRuntimeOptions options)
        : runtime(loopback_config_with_ephemeral_ports(), quiet(options)) {
        REQUIRE_EQ(admin.open(path), "");
        REQUIRE(runtime.start().empty());
        REQUIRE_EQ(admin.start(), "");
    }
};

struct AdminClient {
    net::Socket socket;
    explicit AdminClient(const std::string& path) : socket(::socket(AF_UNIX, SOCK_STREAM, 0)) {
        REQUIRE(socket.is_open());
        REQUIRE_EQ(net::set_no_sigpipe(socket.fd()), 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        path.copy(address.sun_path, path.size());
        REQUIRE_EQ(::connect(socket.fd(), reinterpret_cast<const sockaddr*>(&address), sizeof address), 0);
    }
    void send(std::string_view text) {
        REQUIRE_EQ(net::send_some(socket.fd(), text.data(), text.size()), static_cast<ssize_t>(text.size()));
    }
    // Everything received so far without waiting; `closed` is set at end of input.
    std::string take(bool& closed) {
        std::string text;
        char buffer[4096];
        for (;;) {
            const ssize_t count = ::recv(socket.fd(), buffer, sizeof buffer, MSG_DONTWAIT);
            if (count <= 0) {
                closed = count == 0;
                return text;
            }
            text.append(buffer, static_cast<std::size_t>(count));
        }
    }
    // Polls the runtime until the exchange closes the connection; returns the whole reply.
    std::string reply(server::ServerRuntime& runtime) {
        std::string text;
        const auto deadline = common::monotonic_ns() + 2'000'000'000;
        while (common::monotonic_ns() < deadline) {
            bool closed = false;
            text += take(closed);
            if (closed) return text;
            runtime.poll_once();
        }
        FAIL("admin reply timeout");
        return text;
    }
};

bool exists(const std::string& path) {
    struct stat info {};
    return ::lstat(path.c_str(), &info) == 0;
}
}  // namespace

TEST_CASE(admin_socket, answers_one_command_per_connection_in_idle_and_busy_poll_modes) {
    for (const bool busy_poll : {false, true}) {
        server::ServerRuntimeOptions options;
        options.busy_poll = busy_poll;
        AdminExchange exchange(options);
        struct stat info {};
        REQUIRE_EQ(::lstat(exchange.path.c_str(), &info), 0);
        CHECK(S_ISSOCK(info.st_mode));
        CHECK_EQ(static_cast<unsigned>(info.st_mode & 0777), 0600u);
        for (int round = 0; round < 2; ++round) {  // the listener is watched again after each client
            AdminClient client(exchange.path);
            client.send("status\n");
            const std::string reply = client.reply(exchange.runtime);
            CHECK(reply.find("orders live=0") != std::string::npos);
            CHECK(reply.ends_with("OK\n"));
        }
        CHECK(!exchange.runtime.finished());
    }
}

TEST_CASE(admin_socket, a_command_wakes_a_long_idle_wait) {
    server::ServerRuntimeOptions options;
    options.idle_poll_timeout_ms = 10'000;
    AdminExchange exchange(options);
    AdminClient client(exchange.path);
    client.send("status\n");
    // With a 10 s idle wait, the reply arrives quickly only because the
    // listener and then the client end the wait.
    const auto started = common::monotonic_ns();
    CHECK(client.reply(exchange.runtime).ends_with("OK\n"));
    CHECK_LT(common::monotonic_ns() - started, 2'000'000'000u);
}

TEST_CASE(admin_socket, serves_one_client_at_a_time_and_survives_one_that_leaves) {
    AdminExchange exchange(server::ServerRuntimeOptions{});
    AdminClient first(exchange.path);
    first.send("sta");
    for (int iteration = 0; iteration < 20; ++iteration) exchange.runtime.poll_once();
    AdminClient second(exchange.path);
    second.send("help\n");
    for (int iteration = 0; iteration < 20; ++iteration) exchange.runtime.poll_once();
    bool closed = false;
    CHECK_EQ(second.take(closed), "");  // waits in the backlog behind the unfinished line
    CHECK(!closed);
    first.send("tus\n");
    CHECK(first.reply(exchange.runtime).find("orders live=0") != std::string::npos);
    CHECK(second.reply(exchange.runtime).find("phase") != std::string::npos);

    { AdminClient silent(exchange.path); }  // connects and leaves without a command
    AdminClient next(exchange.path);
    next.send("help\n");
    CHECK(next.reply(exchange.runtime).ends_with("OK\n"));
}

TEST_CASE(admin_socket, refuses_a_taken_path_and_removes_only_its_own_file) {
    TemporaryDirectory directory;
    const std::string regular = (directory.path / "regular").string();
    std::ofstream(regular) << "keep";
    server::ServerRuntime runtime(loopback_config_with_ephemeral_ports(), quiet({}));
    {
        server::AdminSocket admin(runtime);
        CHECK(admin.open(regular).find("Address already in use") != std::string::npos);
        CHECK(admin.open(std::string(200, 'x')).find("too long") != std::string::npos);
    }
    CHECK(exists(regular));

    const std::string path = (directory.path / "admin.sock").string();
    {
        server::AdminSocket first(runtime);
        REQUIRE_EQ(first.open(path), "");
        {
            server::AdminSocket second(runtime);
            CHECK(second.open(path).find("Address already in use") != std::string::npos);
        }
        CHECK(exists(path));  // the refused socket left the first one's file alone
    }
    CHECK(!exists(path));
}
