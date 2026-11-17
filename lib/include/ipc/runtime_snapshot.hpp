#ifndef EXSIM_IPC_RUNTIME_SNAPSHOT_HPP
#define EXSIM_IPC_RUNTIME_SNAPSHOT_HPP
#include "server/server_runtime.hpp"
#include <string>
namespace exsim::ipc {
// The JSON object a managed exchange reports over its control socket and the
// API returns as an exchange's "runtime". It lives with the control socket,
// not in api/, so the management service never depends on the server runtime.
std::string runtime_snapshot_json(server::ServerRuntime& runtime);
}
#endif
