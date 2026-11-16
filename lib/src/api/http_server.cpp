#include "api/http_server.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>

namespace exsim::api {

namespace {

// Waits until the socket is ready for `events` or `timeout` expires. False on timeout or poll error.
bool wait_until_ready(int socket_descriptor, short events, std::chrono::milliseconds timeout) {
    pollfd descriptor{socket_descriptor, events, 0};
    const int timeout_ms = static_cast<int>(std::min<std::chrono::milliseconds::rep>(timeout.count(), INT_MAX));
    int ready;
    do {
        ready = ::poll(&descriptor, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    return ready > 0;
}

int status_for(HttpParseStatus status) noexcept {
    switch (status) {
        case HttpParseStatus::HeadersTooLarge: return 431;
        case HttpParseStatus::BodyTooLarge: return 413;
        case HttpParseStatus::NotImplemented: return 501;
        default: return 400;
    }
}

// False when the peer is gone, the write timeout expired or no byte moved.
bool send_all(int socket_descriptor, std::string_view bytes, std::chrono::milliseconds write_timeout) {
    while (!bytes.empty()) {
        const ssize_t sent = net::send_some(socket_descriptor, bytes.data(), bytes.size());
        if (sent > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(sent));
            continue;
        }
        if (sent < 0 && net::is_would_block(errno) && wait_until_ready(socket_descriptor, POLLOUT, write_timeout))
            continue;
        return false;
    }
    return true;
}

}  // namespace

HttpServer::HttpServer(HttpServerOptions options, HttpRequestHandler handler,
                       HttpRejectionResponder rejection_responder)
    : options_(std::move(options)),
      handler_(std::move(handler)),
      rejection_responder_(std::move(rejection_responder)) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::start() {
    if (accept_thread_.joinable()) throw std::runtime_error("HTTP server is already running");
    std::uint32_t address = 0;
    if (!net::parse_ipv4_address(options_.bind_address, address))
        throw std::runtime_error("invalid IPv4 bind address: " + options_.bind_address);
    net::Socket listener;
    if (const net::ErrorCode error = net::open_tcp_listener({address, options_.port}, 128, 0, 0, listener))
        throw std::runtime_error("listen on " + options_.bind_address + ":" + std::to_string(options_.port) + ": " +
                                 std::strerror(error));
    net::Ipv4Endpoint bound{};
    if (net::local_endpoint(listener.fd(), bound) == 0) bound_port_ = bound.port;
    connection_slots_ = std::vector<ConnectionSlot>(options_.max_connections);
    stopping_ = false;
    listener_ = std::move(listener);
    try {
        accept_thread_ = std::jthread([this] { accept_loop(); });
    } catch (...) {
        listener_.close();
        throw;
    }
}

void HttpServer::stop() {
    if (!accept_thread_.joinable()) return;
    stopping_ = true;
    accept_thread_.join();
    listener_.close();
    for (ConnectionSlot& slot : connection_slots_)
        if (slot.thread.joinable()) ::shutdown(slot.socket.fd(), SHUT_RDWR);
    for (ConnectionSlot& slot : connection_slots_) {
        if (!slot.thread.joinable()) continue;
        slot.thread.join();
        slot.socket.close();
    }
}

void HttpServer::join_finished_connections() {
    for (ConnectionSlot& slot : connection_slots_) {
        if (!slot.thread.joinable() || !slot.is_finished.load()) continue;
        slot.thread.join();
        slot.socket.close();
    }
}

void HttpServer::accept_loop() {
    while (!stopping_.load()) {
        pollfd listener{listener_.fd(), POLLIN, 0};
        const int ready = ::poll(&listener, 1, 100);
        join_finished_connections();
        if (ready <= 0) continue;
        net::Socket client;
        net::Ipv4Endpoint peer{};
        if (net::accept_connection(listener_.fd(), client, peer) != 0) continue;
        (void)net::set_tcp_no_delay(client.fd(), true);
        const auto free_slot = std::find_if(connection_slots_.begin(), connection_slots_.end(),
                                            [](const ConnectionSlot& slot) { return !slot.thread.joinable(); });
        if (free_slot != connection_slots_.end()) {
            ConnectionSlot& slot = *free_slot;
            slot.socket = std::move(client);
            slot.is_finished = false;
            ++active_connections_;
            try {
                slot.thread = std::jthread([this, &slot] {
                    try {
                        serve_connection(slot.socket.fd());
                    } catch (...) {
                        // Allocation failure or a throwing rejection responder drops this connection only.
                    }
                    // Send FIN now instead of when the accept thread next reaps the slot.
                    ::shutdown(slot.socket.fd(), SHUT_WR);
                    slot.is_finished = true;
                    --active_connections_;
                });
                continue;
            } catch (...) {
                // No thread for this connection: answer it like a full server.
                --active_connections_;
                client = std::move(slot.socket);
            }
        }
        try {
            HttpResponse response =
                rejection_responder_(503, "service_unavailable", "too many concurrent connections; retry shortly");
            response.close_connection = true;
            (void)send_all(client.fd(), serialize_http_response(response, false), options_.write_timeout);
        } catch (...) {
            // The connection is closed below either way.
        }
        ::shutdown(client.fd(), SHUT_RDWR);
    }
}

void HttpServer::serve_connection(int socket_descriptor) {
    // Bytes from request_start on are not served yet. Serving a request only
    // advances request_start; consumed bytes are dropped once, right before
    // the next read, so pipelined requests cost no per-request memmove.
    std::string received_bytes;
    std::size_t request_start = 0;
    HttpRequest request;
    // Set once the current request's line and headers are parsed, so a body
    // arriving over many reads does not re-parse them.
    bool has_request_head = false;
    std::size_t request_head_bytes = 0;
    std::size_t request_body_bytes = 0;
    std::size_t requests_served = 0;
    // A started request must be complete by this time. Timing each read alone
    // would let a client hold its slot by sending one byte per idle timeout.
    std::chrono::steady_clock::time_point request_deadline;
    char chunk[16 * 1024];
    while (!stopping_.load()) {
        const std::string_view pending = std::string_view(received_bytes).substr(request_start);
        if (!has_request_head) {
            const HttpParseResult head = parse_http_request_head(pending, options_.limits, request);
            if (head.status == HttpParseStatus::Complete) {
                has_request_head = true;
                request_head_bytes = head.consumed_bytes;
                request_body_bytes = head.body_bytes;
            } else if (head.status != HttpParseStatus::NeedMoreData) {
                HttpResponse response =
                    rejection_responder_(status_for(head.status), "invalid_request", head.error_message);
                (void)send_all(socket_descriptor, serialize_http_response(response, false), options_.write_timeout);
                return;
            }
        }
        if (has_request_head && pending.size() - request_head_bytes >= request_body_bytes) {
            request.body.assign(pending.substr(request_head_bytes, request_body_bytes));
            request_start += request_head_bytes + request_body_bytes;
            has_request_head = false;
            ++requests_served;
            HttpResponse response;
            try {
                response = handler_(request);
            } catch (const std::exception& error) {
                response = rejection_responder_(500, "internal_error", error.what());
            } catch (...) {
                response = rejection_responder_(500, "internal_error", "request handler failed");
            }
            const bool keep_alive = request.keep_alive && !response.close_connection &&
                                    requests_served < options_.max_requests_per_connection && !stopping_.load();
            if (!send_all(socket_descriptor, serialize_http_response(response, keep_alive), options_.write_timeout) ||
                !keep_alive)
                return;
            request = HttpRequest{};
            request_deadline = std::chrono::steady_clock::now() + options_.idle_timeout;  // for pipelined bytes
            continue;
        }

        received_bytes.erase(0, request_start);
        request_start = 0;
        const ssize_t received = net::receive_some(socket_descriptor, chunk, sizeof(chunk));
        if (received > 0) {
            if (received_bytes.empty()) request_deadline = std::chrono::steady_clock::now() + options_.idle_timeout;
            received_bytes.append(chunk, static_cast<std::size_t>(received));
            continue;
        }
        if (received < 0 && net::is_would_block(errno)) {
            // An idle connection waits the full timeout; a started request only what is left of its deadline.
            const auto timeout = received_bytes.empty() ? options_.idle_timeout
                                                        : std::chrono::duration_cast<std::chrono::milliseconds>(
                                                              request_deadline - std::chrono::steady_clock::now());
            if (timeout.count() > 0 && wait_until_ready(socket_descriptor, POLLIN, timeout)) continue;
            if (!received_bytes.empty()) {
                HttpResponse response =
                    rejection_responder_(408, "invalid_request", "request incomplete before the idle timeout");
                (void)send_all(socket_descriptor, serialize_http_response(response, false), options_.write_timeout);
            }
        }
        return;  // peer closed, timeout or error
    }
}

}  // namespace exsim::api
