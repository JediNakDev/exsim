#include "server/admin/admin_socket.hpp"
#include "server/config.hpp"
#include "server/runtime/server_runtime.hpp"

#include <csignal>
#include <filesystem>
#include <iostream>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {
// One JSON line on stdout once every listener is open, so a harness can read
// the actual ports (listener port 0 in the config asks the OS for one).
// Addresses passed parse_ipv4_address in start() and the session is digits,
// so no value needs JSON escaping.
std::string ready_line(exsim::server::ServerRuntime& exchange) {
    using namespace exsim::server;
    const auto& config = exchange.config();
    const auto& market_data = config.market_data;
    const bool mdp = market_data.protocol == MarketDataProtocol::Mdp3;
    std::string line;
    const auto add = [&line](std::string_view key, const std::string& value) {
        line += line.empty() ? "{\"" : ",\"";
        line.append(key).append("\":").append(value);
    };
    const auto text = [](std::string_view value) { return "\"" + std::string(value) + "\""; };
    add("state", text("running"));
    add("address", text(config.exchange.bind_address));
    add("orderEntryPort", std::to_string(exchange.listen_endpoint<runtime::OuchGateway>().port));
    add("fixOrderEntryPort", std::to_string(exchange.listen_endpoint<runtime::FixGateway>().port));
    add("ilinkOrderEntryPort", std::to_string(exchange.listen_endpoint<runtime::IlinkGateway>().port));
    add("marketDataProtocol", text(mdp ? "mdp3_sbe" : "itch50_moldudp64"));
    add("marketDataAddress", text(market_data.address));
    add("marketDataPort", std::to_string(market_data.port));
    add("recoveryPort", std::to_string(exchange.retransmission_endpoint().port));
    add("recoveryTransport", text(mdp ? "tcp" : "udp"));
    add("snapshotPort", std::to_string(market_data.snapshot_port));
    add("instrumentReplayPort", std::to_string(market_data.instrument_port));
    add("backupMarketDataPort", std::to_string(market_data.backup_port));
    add("channelId", std::to_string(market_data.channel_id));
    add("session", text(market_data.session));
    return line + "}";
}
}  // namespace

int main(int argc, char** argv) {
    std::string path;
    std::string admin_path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            path = argv[++i];
        } else if (arg == "--admin-socket" && i + 1 < argc) {
            admin_path = argv[++i];
        } else {
            path.clear();
            break;
        }
    }
    if (path.empty()) {
        std::cerr << "usage: exsim_exchanged --config PATH [--admin-socket PATH]\n";
        return 2;
    }
    std::signal(SIGPIPE, SIG_IGN);
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    try {
        exsim::server::ServerConfig config;
        std::string error;
        if (!exsim::server::load_config(path.c_str(), config, error))
            throw std::runtime_error("invalid exchange config: " + error);
        // A relative journal path is relative to the config file, not the working directory.
        if (!config.journal_path.empty() && config.journal_path.front() != '/')
            config.journal_path = (std::filesystem::path(path).parent_path() / config.journal_path).string();
        const auto session =
            std::to_string(static_cast<unsigned long>(::getpid())) + std::to_string(exsim::common::monotonic_ns());
        config.market_data.session = session.substr(session.size() - 10);
        config.exchange.soup_session = config.market_data.session;
        exsim::server::ServerRuntimeOptions options;
        exsim::server::ServerRuntime runtime(config, options);
        exsim::server::AdminSocket admin(runtime);
        if (!admin_path.empty() && !(error = admin.open(admin_path)).empty()) throw std::runtime_error(error);
        if (!(error = runtime.start()).empty()) throw std::runtime_error(error);
        if (!admin_path.empty() && !(error = admin.start()).empty()) throw std::runtime_error(error);
        std::cout << ready_line(runtime) << std::endl;
        while (!runtime.finished()) {
            // macOS has no sigtimedwait; consume blocked signals between polls.
            sigset_t pending;
            sigpending(&pending);
            if (sigismember(&pending, SIGINT) || sigismember(&pending, SIGTERM)) {
                int signal = 0;
                sigwait(&signals, &signal);
                runtime.request_shutdown();
            }
            runtime.poll_once();
        }
    } catch (const std::exception& error) {
        std::cerr << "exsim_exchanged: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
