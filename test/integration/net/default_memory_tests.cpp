// exsim_exchanged at default sizes stays small: a config with no [engine]
// section, default max_connections, one OUCH and one FIX user peaks below
// 96 MiB (192 MiB under ASan), well below the 360 MiB the old defaults made
// resident.

#include "test/framework/test.hpp"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr const char* kDefaultSizedConfig = R"(
[exchange]
bind_address = 127.0.0.1
ouch_port = 0
fix_port = 0
ilink_port = 0

[market_data]
address = 127.0.0.1
port = 0

[instrument AAPL]
tick_size = 0.01
lot_size = 1
min_price = 0.01
max_price = 1000.00
reference_price = 150.00

[user ALICE]
password = alice
protocol = ouch42
session_id = 1

[user BOB]
password = bob
protocol = fix44
session_id = 2
)";

// The exchange binary is built with the same flags as this test. ASan adds
// shadow memory and its own runtime (about 120 MiB peak at default sizes),
// still far below the 300+ MiB the old defaults prefaulted.
#if defined(__SANITIZE_ADDRESS__)
constexpr std::uint64_t kPeakLimitBytes = std::uint64_t{192} << 20;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr std::uint64_t kPeakLimitBytes = std::uint64_t{192} << 20;
#else
constexpr std::uint64_t kPeakLimitBytes = std::uint64_t{96} << 20;
#endif
#else
constexpr std::uint64_t kPeakLimitBytes = std::uint64_t{96} << 20;
#endif

std::filesystem::path exchange_binary() {
    const char* build = std::getenv("EXSIM_BUILD");
    return std::filesystem::path(build ? build : "build/release") / "exsim_exchanged";
}

}  // namespace

TEST_CASE(exchange_process_memory, default_sizes_peak_below_the_limit) {
    char directory_template[] = "/tmp/exsim-memory-XXXXXX";
    const char* directory = ::mkdtemp(directory_template);
    REQUIRE(directory != nullptr);
    const std::filesystem::path config = std::filesystem::path(directory) / "exchange.conf";
    std::ofstream(config) << kDefaultSizedConfig;

    int ready_pipe[2] = {-1, -1};
    REQUIRE_EQ(::pipe(ready_pipe), 0);
    const std::string binary = exchange_binary().string();
    const std::string config_path = config.string();
    const pid_t pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        ::dup2(ready_pipe[1], STDOUT_FILENO);
        ::close(ready_pipe[0]);
        ::close(ready_pipe[1]);
        ::execl(binary.c_str(), binary.c_str(), "--config", config_path.c_str(), nullptr);
        ::_exit(127);
    }
    ::close(ready_pipe[1]);
    // The ready line is printed after start(): every startup allocation is done.
    char byte = 0;
    while (::read(ready_pipe[0], &byte, 1) == 1 && byte != '\n') {}
    ::close(ready_pipe[0]);
    CHECK_EQ(byte, '\n');
    ::kill(pid, SIGTERM);
    int status = 0;
    rusage usage{};
    REQUIRE_EQ(::wait4(pid, &status, 0, &usage), pid);
    std::filesystem::remove_all(directory);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

#ifdef __APPLE__
    const auto peak_bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    const auto peak_bytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
    CHECK_GT(peak_bytes, 0u);
    CHECK_LT(peak_bytes, kPeakLimitBytes);
}
