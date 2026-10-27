/* Licensed under the MIT License. Compile a validated b2cpp array into a native runner. */
#include "Streams.hpp"
#include "Run.hpp"
#include <cstdint>
#include MIRNEXT_EMBEDDED_HEADER
int main (int argc, char **argv, char **environment) {
  auto input = std::as_bytes (std::span (mir_code));
  auto result = mirnext::tools::run_program (input, argc, argv, environment);
  return result ? *result : mirnext::tools::report (result.error ());
}
