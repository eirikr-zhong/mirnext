/* Licensed under the MIT License. Text/binary/embedded-array conversion on owning C++ IR. */
#include "Streams.hpp"
#include "mirnext/Serialization/Format.hpp"
#ifndef MIRNEXT_FORMAT_MODE
#error Missing format tool mode
#endif
int main (int argc, char **argv) {
  using namespace mirnext;
  using namespace mirnext::serialization;
  using namespace mirnext::tools;
  if (argc != 1) {
    std::cerr << "usage: " << argv[0] << " < input > output\n";
    return 1;
  }
  auto mode = binary_stdio ();
  if (!(mode).has_value ()) return report (mode.error ());
  auto input = read_stream (std::cin, "<stdin>");
  if (!(input).has_value ()) return report (input.error ());
  auto context = MIRNEXT_FORMAT_MODE == 0
                   ? parse_text (input.value (), {.source_name = "<stdin>"})
                   : read_binary (bytes (input.value ()), {.source_name = "<stdin>"});
  if (!context.has_value ()) return report (context.error ());
  if constexpr (MIRNEXT_FORMAT_MODE == 1) {
    auto text = write_text (context.value ());
    if (!text.has_value ()) return report (text.error ());
    auto written = write_stream (text.value ());
    return written.has_value () ? 0 : report (written.error ());
  } else {
    auto binary = write_binary (context.value ());
    if (!binary.has_value ()) return report (binary.error ());
    if constexpr (MIRNEXT_FORMAT_MODE == 0) {
      auto written = write_stream (
        {reinterpret_cast<const char *> (binary.value ().data ()), binary.value ().size ()});
      return written.has_value () ? 0 : report (written.error ());
    } else {
      // Emit a self-contained C++ header with buffered byte output.
      std::string output
        = "#pragma once\n#include <cstdint>\n\ninline constexpr std::uint8_t mir_code[] = {\n";
      constexpr char hex[] = "0123456789abcdef";
      std::size_t count = 0;
      for (auto byte : binary.value ()) {
        auto value = std::to_integer<unsigned> (byte);
        output += "0x";
        output += hex[value >> 4];
        output += hex[value & 15];
        output += ", ";
        if (++count % 16 == 0) output += '\n';
        if (output.size () >= 16384) {
          auto written = write_stream (output);
          if (!written.has_value ()) return report (written.error ());
          output.clear ();
        }
      }
      if (count % 16) output += '\n';
      output += "};\n";
      auto written = write_stream (output);
      return (written).has_value () ? 0 : report (written.error ());
    }
  }
}
