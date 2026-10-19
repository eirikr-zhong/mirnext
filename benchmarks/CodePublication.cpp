/* Licensed under the MIT License. One immutable batch versus individual pages. */
#include "mirnext/Execution/CodeMemory.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
using namespace mirnext; using namespace mirnext::execution;
namespace {
std::vector<std::uint8_t> bytes() {
#if defined(__aarch64__) || defined(_M_ARM64)
  std::array<std::uint32_t,2> words{0xd2800540u,0xd65f03c0u};
  std::vector<std::uint8_t> code(sizeof words); std::memcpy(code.data(),words.data(),code.size()); return code;
#else
  return {0xb8,42,0,0,0,0xc3};
#endif
}
Result<double> measure(std::size_t count,bool batch) {
  auto code = bytes(); std::vector<std::size_t> sizes(batch ? count : 1,code.size());
  std::vector<std::span<const std::uint8_t>> buffers(sizes.size(),code); std::array<double,7> times;
  for (auto &time : times) {
    CodeMemory memory; std::vector<ExecutableCode> handles; handles.reserve(count);
    auto start = std::chrono::steady_clock::now();
    for (std::size_t n = 0; n < (batch ? 1 : count); ++n) {
      auto reserved = memory.reserve(sizes); if (!reserved) return reserved.error();
      auto result = reserved->publish(buffers); if (!result) return result.error();
      for (auto &handle : *result) handles.push_back(std::move(handle));
    }
    time = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - start).count();
    // Validation and unmapping are outside both timing intervals.
    for (auto &handle : handles) {
      if (!std::equal(code.begin(),code.end(),handle.bytes().begin(),handle.bytes().end())
          || reinterpret_cast<int (*)()>(handle.address())() != 42)
        return Error{ErrorCode::InvalidOperand,"publication changed code"};
    }
  }
  std::sort(times.begin(),times.end()); return times[times.size()/2];
}
}
int main() {
  for (std::size_t count : {64u,256u,1024u}) {
    auto individual = measure(count,false); auto batch = measure(count,true);
    if (!individual || !batch) { std::cerr << (!individual ? individual.error().message : batch.error().message) << '\n'; return 1; }
    std::cout << "functions=" << count << " individual_ms=" << *individual << " batch_ms=" << *batch << '\n';
  }
}
