/* Licensed under the MIT License. Text/binary/embedded-array conversion on owning C++ IR. */
#include "Streams.hpp"
#include "mirnext/Serialization/Format.hpp"
#ifndef MIRNEXT_FORMAT_MODE
#error Missing format tool mode
#endif
int main(int argc,char **argv) {
  using namespace mirnext; using namespace mirnext::serialization; using namespace mirnext::tools;
  if (argc != 1) { std::cerr << "usage: " << argv[0] << " < input > output\n"; return 1; }
  auto mode = binary_stdio(); if (!mode) return report(mode.error());
  auto input = read_stream(std::cin,"<stdin>"); if (!input) return report(input.error());
  auto context = MIRNEXT_FORMAT_MODE == 0 ? parse_text(*input,{.source_name="<stdin>"})
    : read_binary(bytes(*input),{.source_name="<stdin>"});
  if (!context) return report(context.error());
  if constexpr (MIRNEXT_FORMAT_MODE == 1) {
    auto text = write_text(**context); if (!text) return report(text.error());
    auto written = write_stream(*text); return written ? 0 : report(written.error());
  } else {
    auto binary = write_binary(**context); if (!binary) return report(binary.error());
    if constexpr (MIRNEXT_FORMAT_MODE == 0) {
      auto written = write_stream({reinterpret_cast<const char *>(binary->data()),binary->size()});
      return written ? 0 : report(written.error());
    } else {
      // Buffered output, rather than fprintf for each byte. Keep
      // the existing uint8_t array spelling usable by C and C++ consumers.
      std::string output = "static const uint8_t mir_code[] = {\n";
      constexpr char hex[] = "0123456789abcdef"; std::size_t count = 0;
      for (auto byte : *binary) {
        auto value = std::to_integer<unsigned>(byte); output += "0x";
        output += hex[value >> 4]; output += hex[value & 15]; output += ", ";
        if (++count % 16 == 0) output += '\n';
        if (output.size() >= 16384) {
          auto written = write_stream(output); if (!written) return report(written.error()); output.clear();
        }
      }
      if (count % 16) output += '\n'; output += "};\n";
      auto written = write_stream(output); return written ? 0 : report(written.error());
    }
  }
}
