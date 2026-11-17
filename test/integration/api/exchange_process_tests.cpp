#include "api/exchange_process.hpp"
#include "test/framework/test.hpp"

#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unistd.h>

using namespace exsim;

namespace {
struct ExchangeConfigDirectory {
    std::filesystem::path path;
    ExchangeConfigDirectory() {
        char text[] = "/tmp/exsim-process-XXXXXX";
        REQUIRE(::mkdtemp(text) != nullptr);
        path = text;
        std::ifstream fixture("test/fixtures/exchange.conf");
        REQUIRE(fixture.good());
        std::string config((std::istreambuf_iterator<char>(fixture)), {});
        for (const std::string_view key : {"ouch_port = 15000", "port = 16000", "retransmit_port = 16001"}) {
            const auto position = config.find(key);
            REQUIRE(position != std::string::npos);
            config.replace(position, key.size(), std::string(key.substr(0, key.find('='))) + "= 0");
        }
        std::ofstream(path / "exchange.conf") << config;
    }
    ~ExchangeConfigDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

// Spawns exsim_exchanged and waits for its ready snapshot on the control socket.
bool spawn_and_await_readiness(const std::filesystem::path& config_directory) {
    const char* build = std::getenv("EXSIM_BUILD");
    const auto binary = std::filesystem::path(build ? build : "build/release") / "exsim_exchanged";
    api::ExchangeProcess process;
    const bool is_ready = process.spawn(binary, config_directory / "exchange.conf") && process.await_readiness();
    process.stop();
    return is_ready;
}
}  // namespace

// The child must inherit the API's stdin, stdout and stderr. If they were
// closed, the child's first opens would take fds 0 to 2 and its ready line
// on stdout would land in whatever file got fd 1, such as the journal.
TEST_CASE(api_exchange_process, child_ready_line_reaches_parent_stdout) {
    ExchangeConfigDirectory directory;
    const auto captured_path = directory.path / "stdout.txt";
    const int captured = ::open(captured_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    REQUIRE(captured >= 0);
    std::cout.flush();
    const int saved_stdout = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 10);
    ::dup2(captured, STDOUT_FILENO);
    ::close(captured);
    const bool is_ready = spawn_and_await_readiness(directory.path);
    ::dup2(saved_stdout, STDOUT_FILENO);
    ::close(saved_stdout);

    std::ifstream captured_file(captured_path);
    const std::string child_stdout((std::istreambuf_iterator<char>(captured_file)), {});
    CHECK(is_ready);
    CHECK(child_stdout.starts_with("{\"state\":\"running\""));
}

// An API started with stdin closed gets its control socketpair on fds 0 and
// 3, so the child end is already fd 3 and the spawn's dup2 onto fd 3 is a no-op.
TEST_CASE(api_exchange_process, control_end_already_on_fd_3_reaches_child) {
    ExchangeConfigDirectory directory;
    const int saved_stdin = ::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 10);
    const int saved_fd_3 = ::fcntl(3, F_DUPFD_CLOEXEC, 10);
    ::close(STDIN_FILENO);
    ::close(3);
    const bool is_ready = spawn_and_await_readiness(directory.path);
    ::dup2(saved_stdin, STDIN_FILENO);
    ::close(saved_stdin);
    if (saved_fd_3 >= 0) {
        ::dup2(saved_fd_3, 3);
        ::close(saved_fd_3);
    }
    CHECK(is_ready);
}
