// FIX 4.4 decoder, session layer and order decoding: one order-entry connection's input.
#include "test/fuzz/exchange.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Exchange exchange;
    exchange.receive<fuzz::FixGateway>({reinterpret_cast<const char*>(data), size});
    return 0;
}
