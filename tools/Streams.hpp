/* Licensed under the MIT License. Buffered command-line I/O, independent of the C oracle. */
#pragma once
#include "mirnext/Support/Result.hpp"
#include <array>
#include <cstdio>
#include <span>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
namespace mirnext::tools {
inline Error invalid(std::string message) { return {ErrorCode::InvalidArgument,std::move(message)}; }
inline int report(const Error &error) { std::cerr << error.message << '\n'; return 1; }
inline Result<void> binary_stdio() {
#ifdef _WIN32
  if (_setmode(_fileno(stdin),_O_BINARY) == -1 || _setmode(_fileno(stdout),_O_BINARY) == -1)
    return invalid("cannot set standard streams to binary mode");
#endif
  std::ios::sync_with_stdio(false); std::cin.tie(nullptr);
  return {};
}
inline Result<std::string> read_stream(std::istream &input,std::string_view name) {
  constexpr std::size_t limit = 256 * 1024 * 1024;
  std::string data; std::array<char,16384> buffer;
  while (input) {
    input.read(buffer.data(),buffer.size()); auto count = std::size_t(input.gcount());
    if (count > limit-data.size()) return invalid(std::string(name)+": input exceeds 256 MiB limit");
    data.append(buffer.data(),count);
  }
  if (input.bad() || !input.eof()) return invalid(std::string(name)+": input read failed");
  return data;
}
inline Result<void> write_stream(std::string_view data) {
  std::cout.write(data.data(),static_cast<std::streamsize>(data.size())); std::cout.flush();
  if (!std::cout) return invalid("standard output write failed"); return {};
}
inline std::span<const std::byte> bytes(std::string_view data) { return std::as_bytes(std::span(data.data(),data.size())); }
}
