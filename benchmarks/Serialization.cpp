/* Licensed under the MIT License. Timing is informational, never a test threshold. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
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
        Program context;
        auto m = mirnext::ir::detail::access (context).create_module ("serialization");
        if (!(m).has_value ()) return 1;
        auto name = [&] (std::string_view prefix, unsigned n) {
          return std::string (prefix)
                 + (long_names ? "_identifier_with_more_than_small_string_capacity_" : "_")
                 + std::to_string (n);
        };
        auto data = mirnext::ir::detail::access (((m)).value ())
                      .create_data (name ("data", 0), ZeroData{8});
        if (!(data).has_value ()) return 1;
        auto f = mirnext::ir::detail::access (((m)).value ())
                   .create_function (name ("entry", 0), {{Type::i64 ()}, {}});
        if (!(f).has_value ()) return 1;
        auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
        if (!(b).has_value ()) return 1;
        InstructionBuilder builder ((b).value ());
        auto value = mirnext::ir::detail::access (((f)).value ())
                       .create_register (Type::i64 (), name ("value", 0));
        if (!(value).has_value ()) return 1;
        for (unsigned n = 0; n < count; ++n) {
          auto destination = distinct ? mirnext::ir::detail::access (((f)).value ())
                                          .create_register (Type::i64 (), name ("destination", n))
                                      : value;
          if (!(destination).has_value ()
              || !(builder.assign ((destination).value (), Operand (data.value ().symbol ())))
                    .has_value ()
              || !(builder.emit (Opcode::Add, {Operand ((value).value ()),
                                               Operand ((destination).value ()), Operand (n)}))
                    .has_value ())
            return 1;
        }
        if (!(builder.ret (Operand ((value).value ()))).has_value ()) return 1;
        auto expected = serialization::write_binary (context, serialization::BinaryEncoding::Raw);
        if (!(expected).has_value ()) return 1;
        auto parsed
          = serialization::read_binary ((expected).value (), {serialization::BinaryEncoding::Raw});
        if (!(parsed).has_value ()) return 1;
        auto canonical
          = serialization::write_binary ((parsed).value (), serialization::BinaryEncoding::Raw);
        if (!(canonical).has_value () || (canonical).value () != (expected).value ()) return 1;
        std::array<double, 7> times;
        for (auto &time : times) {
          auto start = std::chrono::steady_clock::now ();
          auto bytes = serialization::write_binary (context, serialization::BinaryEncoding::Raw);
          auto end = std::chrono::steady_clock::now ();
          if (!(bytes).has_value () || (bytes).value () != (expected).value ()) return 1;
          time = std::chrono::duration<double, std::micro> (end - start).count ();
        }
        std::sort (times.begin (), times.end ());
        std::cout << "long_names=" << long_names << " distinct=" << distinct << " pairs=" << count
                  << " bytes=" << expected.value ().size ()
                  << " fingerprint=" << fingerprint ((expected).value ())
                  << " median_us=" << times[times.size () / 2] << '\n';
      }
}
