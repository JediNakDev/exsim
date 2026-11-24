// SoupBinTCP framing and OUCH 4.2 inbound messages: one order-entry connection's input.
#include "fuzz/exchange.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Exchange exchange;
    exchange.receive<fuzz::OuchGateway>({reinterpret_cast<const char*>(data), size});
    return 0;
}
