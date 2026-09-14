#include <cstddef>
#include <cstdint>
#include <string_view>

#include <js/frontend/frontend.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto* chars = reinterpret_cast<const char*>(data);
    const std::string_view source(chars, size);
    (void)js::frontend::parse_program(source);
    return 0;
}
