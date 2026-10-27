/* Licensed under the MIT License. Timing is informational, never a test threshold. */
#include "mirnext/Serialization/Format.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
using namespace mirnext;
using namespace mirnext::ir;
namespace {
std::uint64_t fingerprint (std::span<const std::byte> bytes) {
  std::uint64_t value = UINT64_C (14695981039346656037);
  for (auto byte : bytes)
    value = (value ^ std::to_integer<unsigned> (byte)) * UINT64_C (1099511628211);
  return value;
}
}  // namespace
int main () {
  for (bool long_names : {false, true})
    for (bool distinct : {false, true})
      for (unsigned count : {4096, 65536}) {
        Context context;
        auto m = context.create_module ("serialization");
        if (!m) return 1;
        auto name = [&] (std::string_view prefix, unsigned n) {
          return std::string (prefix)
                 + (long_names ? "_identifier_with_more_than_small_string_capacity_" : "_")
                 + std::to_string (n);
        };
        auto data = (*m)->create_data (name ("data", 0), ZeroData{8});
        if (!data) return 1;
        auto f = (*m)->create_function (name ("entry", 0), {{Type::i64 ()}, {}});
        if (!f) return 1;
        auto b = (*f)->create_block ();
        if (!b) return 1;
        IRBuilder builder (**b);
        auto value = (*f)->create_register (Type::i64 (), name ("value", 0));
        if (!value) return 1;
        for (unsigned n = 0; n < count; ++n) {
          auto destination
            = distinct ? (*f)->create_register (Type::i64 (), name ("destination", n)) : value;
          if (!destination || !builder.assign (**destination, Operand ((*data)->symbol ()))
              || !builder.emit (Opcode::Add,
                                {Operand (**value), Operand (**destination), Operand (n)}))
            return 1;
        }
        if (!builder.ret (Operand (**value))) return 1;
        auto expected = serialization::write_binary (context, serialization::BinaryEncoding::Raw);
        if (!expected) return 1;
        auto parsed = serialization::read_binary (*expected, {serialization::BinaryEncoding::Raw});
        if (!parsed) return 1;
        auto canonical = serialization::write_binary (**parsed, serialization::BinaryEncoding::Raw);
        if (!canonical || *canonical != *expected) return 1;
        std::array<double, 7> times;
        for (auto &time : times) {
          auto start = std::chrono::steady_clock::now ();
          auto bytes = serialization::write_binary (context, serialization::BinaryEncoding::Raw);
          auto end = std::chrono::steady_clock::now ();
          if (!bytes || *bytes != *expected) return 1;
          time = std::chrono::duration<double, std::micro> (end - start).count ();
        }
        std::sort (times.begin (), times.end ());
        std::cout << "long_names=" << long_names << " distinct=" << distinct << " pairs=" << count
                  << " bytes=" << expected->size () << " fingerprint=" << fingerprint (*expected)
                  << " median_us=" << times[times.size () / 2] << '\n';
      }
}
