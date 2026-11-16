#include "api/api_json.hpp"
#include "ipc/exchange_control.hpp"
#include "ipc/runtime_snapshot.hpp"
#include "server/admin/core_admin.hpp"
#include "server/config.hpp"
#include "server/server_runtime.hpp"
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
// Commands run on the reactor thread. IPC failures request shutdown.
struct ControlSocket {
    exsim::net::Socket& socket;
    exsim::server::ServerRuntime& runtime;
};

// True while the non-blocking socket has unread bytes, EOF or an error, so the
// next receive_control either reads a command or reports the failure.
bool control_has_input(int fd) noexcept {
    char byte = 0;
    const ssize_t count = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return count >= 0 || (errno != EINTR && !exsim::net::is_would_block(errno));
}

void handle_control_commands(void* context) noexcept {
    auto& control = *static_cast<ControlSocket*>(context);
    while (control.socket.is_open() && control_has_input(control.socket.fd())) {
        try {
            const auto command = exsim::ipc::receive_control(control.socket.fd());
            if (command.starts_with("admin ")) {
                std::string reply;
                exsim::server::CoreAdmin(control.runtime.core()).execute_line(std::string_view(command).substr(6), reply);
                exsim::ipc::send_control(control.socket.fd(), reply);
                continue;
            }
            if (command == "stop") control.runtime.request_shutdown();
            else if (command != "status") throw std::runtime_error("invalid control command");
            exsim::ipc::send_control(control.socket.fd(), exsim::ipc::runtime_snapshot_json(control.runtime));
        } catch (...) {
            control.runtime.request_shutdown();
            control.runtime.unwatch_readable_fd();
            control.socket.close();
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    exsim::net::Socket control;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            path = argv[++i];
        } else if (arg == "--control-fd" && i + 1 < argc && std::string_view(argv[++i]) == "3") {
            control = exsim::net::Socket(3);
        } else {
            std::cerr << "usage: exsim_exchanged --config PATH [--control-fd 3]\n";
            return 2;
        }
    }
    std::signal(SIGPIPE, SIG_IGN);
    sigset_t signals; sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    try {
        exsim::server::ServerConfig config;
        std::string error;
        if (path.empty() || !exsim::server::load_config(path.c_str(), config, error))
            throw std::runtime_error("invalid exchange config: " + error);
        // A relative journal path is relative to the config file, not the working directory.
        if (!config.journal_path.empty() && config.journal_path.front() != '/')
            config.journal_path = (std::filesystem::path(path).parent_path() / config.journal_path).string();
        if (control.is_open()) {
            (void)exsim::net::set_non_blocking(control.fd());
            (void)exsim::net::set_no_sigpipe(control.fd());
        }
        const auto session = std::to_string(static_cast<unsigned long>(::getpid())) + std::to_string(exsim::net::monotonic_ns());
        config.market_data.session = session.substr(session.size() - 10);
        config.exchange.soup_session = config.market_data.session;
        exsim::server::ServerRuntimeOptions options;
        exsim::server::ServerRuntime runtime(config, options);
        error = runtime.start();
        if (!error.empty()) throw std::runtime_error(error);
        const auto ready = exsim::ipc::runtime_snapshot_json(runtime);
        if (control.is_open()) exsim::ipc::send_control(control.fd(), ready, 10000);
        std::cout << ready << std::endl;
        ControlSocket control_socket{control, runtime};
        if (control.is_open()) {
            if (const exsim::net::ErrorCode watch_error =
                    runtime.watch_readable_fd(control.fd(), handle_control_commands, &control_socket);
                watch_error != 0) {
                throw std::runtime_error("control socket poller: " + std::string(std::strerror(watch_error)));
            }
        }
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
        if (control.is_open()) {
            try {
                exsim::api::ApiJsonWriter json;
                json.begin_object().key("error").string(error.what()).end_object();
                exsim::ipc::send_control(control.fd(), json.take());
            } catch (...) {
            }
        }
        std::cerr << "exsim_exchanged: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
