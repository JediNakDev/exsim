// Admin socket commands, one per line, run in turn against one exchange.
#include "test/fuzz/exchange.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Exchange exchange;
    std::string_view input(reinterpret_cast<const char*>(data), size);
    while (!input.empty()) {
        const std::string_view line = input.substr(0, input.find('\n'));
        input.remove_prefix(std::min(input.size(), line.size() + 1));
        std::string reply;
        exsim::server::CoreAdmin(exchange.core).execute_line(line, reply);
    }
    return 0;
}
