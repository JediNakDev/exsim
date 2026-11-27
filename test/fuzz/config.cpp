// The exchange config file parser.
#include "server/config.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    exsim::server::ServerConfig config;
    std::string error;
    (void)exsim::server::parse_config({reinterpret_cast<const char*>(data), size}, config, error);
    return 0;
}
