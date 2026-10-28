/* Licensed under the MIT License. Executable public scalar-opcode fixtures. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#pragma once
#include <doctest/doctest.h>
#include "mirnext/IR/IR.hpp"
#include <bit>
#include <limits>
namespace mirnext::test {
inline std::unique_ptr<ir::Program> interpreter_scalars () {
  using namespace ir;
  auto context = std::make_unique<Program> ();
  auto module = mirnext::ir::detail::access (*(context)).create_module ("scalars");
  REQUIRE ((module).has_value ());
  auto type = [] (char role) {
    return Type (role == 'f' || role == 'F'   ? Type::Kind::F32
                 : role == 'd' || role == 'D' ? Type::Kind::F64
                 : role == 'e' || role == 'E' ? Type::Kind::LongDouble
                                              : Type::Kind::I64);
  };
  for (auto &d : instruction_descriptors ()) {
    bool compare = d.flow == ControlFlow::Branch && d.roles.size () == 3
                   && d.opcode != Opcode::PrBeq && d.opcode != Opcode::PrBne;
    bool numeric = d.count == OperandCount::Fixed && d.roles.size () >= 2
                   && std::string_view ("IFDE").find (d.roles[0]) != std::string_view::npos
                   && d.opcode != Opcode::Addr && d.opcode != Opcode::Addr8
                   && d.opcode != Opcode::Addr16 && d.opcode != Opcode::Addr32
                   && d.opcode != Opcode::LAddr && d.opcode != Opcode::Alloca
                   && d.opcode != Opcode::VaArg;
    if (!numeric && !compare) continue;
    for (unsigned variant = 0; variant < 3; ++variant) {
      auto f
        = mirnext::ir::detail::access (((module)).value ())
            .create_function ("scalar_" + std::string (d.name) + "_" + std::to_string (variant),
                              {{compare ? Type::i64 () : type (d.roles[0])}, {}});
      REQUIRE ((f).has_value ());
      auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
      REQUIRE ((b).has_value ());
      InstructionBuilder builder ((b).value ());
      std::vector<Operand> operands;
      BasicBlock *yes = nullptr, *fallthrough = nullptr;
      if (compare) {
        auto no = mirnext::ir::detail::access (((f)).value ()).create_block ();
        REQUIRE ((no).has_value ());
        fallthrough = &(no).value ();
        auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
        REQUIRE ((block).has_value ());
        yes = &(block).value ();
        auto label = mirnext::ir::detail::access (((f)).value ()).create_label (*yes);
        REQUIRE ((label).has_value ());
        operands.emplace_back ((label).value ());
      } else {
        auto r = mirnext::ir::detail::access (((f)).value ()).create_register (type (d.roles[0]));
        REQUIRE ((r).has_value ());
        operands.emplace_back ((r).value ());
      }
      for (std::size_t n = 1; n < d.roles.size (); ++n) {
        auto t = type (d.roles[n]);
        auto v = n == 1 ? (variant == 1 ? -7.5 : 7.5) : 2.0;
        if (t.kind () == Type::Kind::F32)
          operands.emplace_back (float (v));
        else if (t.kind () == Type::Kind::F64)
          operands.emplace_back (v);
        else if (t.kind () == Type::Kind::LongDouble)
          operands.emplace_back (static_cast<long double> (v));
        else {
          auto first = variant == 1 ? -7LL : variant == 2 ? 0x100008080LL : 7LL;
          // Keep signed left shifts within their defined domain.
          if (d.opcode == Opcode::Lsh || d.opcode == Opcode::Lshs)
            first = variant == 2 ? 0x100008080LL : 7LL;
          operands.emplace_back (std::int64_t (n == 1 ? first : 2));
        }
      }
      REQUIRE ((builder.emit (d.opcode, operands)).has_value ());
      if (compare) {
        InstructionBuilder no (*fallthrough);
        REQUIRE ((no.ret (0)).has_value ());
        InstructionBuilder target (*yes);
        REQUIRE ((target.ret (1)).has_value ());
      } else {
        // MIR leaves the high half of a 32-bit result undefined. Observe it only
        // after an explicit extension, including when comparing the two JITs.
        if (d.name.ends_with ('s') && type (d.roles[0]).is_integer ())
          REQUIRE ((builder.emit (Opcode::Ext32, {operands[0], operands[0]})).has_value ());
        REQUIRE ((builder.ret (operands[0])).has_value ());
      }
    }
  }
  REQUIRE ((context->verify ()).has_value ());
  return context;
}
inline std::unique_ptr<ir::Program> interpreter_ieee () {
  using namespace ir;
  auto c = std::make_unique<Program> ();
  auto m = mirnext::ir::detail::access (*(c)).create_module ("scalars");
  REQUIRE ((m).has_value ());
  for (bool wide : {false, true})
    for (unsigned variant = 0; variant < 3; ++variant) {
      for (auto &d : instruction_descriptors ()) {
        auto prefix = wide ? 'd' : 'f';
        bool comparison = d.name.size () >= 2 && d.name[0] == prefix && d.roles.size () == 3
                          && d.roles[1] == prefix
                          && (d.roles[0] == 'I' || d.flow == ControlFlow::Branch);
        bool move = d.opcode == (wide ? Opcode::DMov : Opcode::FMov)
                    || d.opcode == (wide ? Opcode::DNeg : Opcode::FNeg);
        if (!comparison && !move) continue;
        bool branch = d.flow == ControlFlow::Branch;
        auto type = wide ? Type (Type::Kind::F64) : Type (Type::Kind::F32);
        auto f
          = mirnext::ir::detail::access (((m)).value ())
              .create_function ("ieee_" + std::string (d.name) + "_" + std::to_string (variant),
                                {{comparison ? Type::i64 () : type}, {}});
        REQUIRE ((f).has_value ());
        auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
        REQUIRE ((b).has_value ());
        InstructionBuilder builder ((b).value ());
        std::vector<Operand> operands;
        BasicBlock *yes = nullptr, *no = nullptr;
        if (branch) {
          auto n = mirnext::ir::detail::access (((f)).value ()).create_block (),
               y = mirnext::ir::detail::access (((f)).value ()).create_block ();
          REQUIRE ((n).has_value ());
          REQUIRE ((y).has_value ());
          no = &(n).value ();
          yes = &(y).value ();
          auto label = mirnext::ir::detail::access (((f)).value ()).create_label (*yes);
          REQUIRE ((label).has_value ());
          operands.emplace_back ((label).value ());
        } else {
          auto r = mirnext::ir::detail::access (((f)).value ())
                     .create_register (comparison ? Type::i64 () : type);
          REQUIRE ((r).has_value ());
          operands.emplace_back ((r).value ());
        }
        auto dvalue = variant == 0   ? std::bit_cast<double> (UINT64_C (0x7ff8000000000042))
                      : variant == 1 ? -0.0
                                     : std::numeric_limits<double>::infinity ();
        auto fvalue = variant == 0   ? std::bit_cast<float> (UINT32_C (0x7fc00042))
                      : variant == 1 ? -0.0f
                                     : std::numeric_limits<float>::infinity ();
        operands.push_back (wide ? Operand (dvalue) : Operand (fvalue));
        if (comparison) operands.push_back (wide ? Operand (0.0) : Operand (0.0f));
        REQUIRE ((builder.emit (d.opcode, operands)).has_value ());
        if (branch) {
          REQUIRE ((InstructionBuilder (*no).ret (0)).has_value ());
          REQUIRE ((InstructionBuilder (*yes).ret (1)).has_value ());
        } else
          REQUIRE ((builder.ret (operands[0])).has_value ());
      }
    }
  REQUIRE ((c->verify ()).has_value ());
  return c;
}
inline std::unique_ptr<ir::Program> interpreter_overflow () {
  using namespace ir;
  auto c = std::make_unique<Program> ();
  auto m = mirnext::ir::detail::access (*(c)).create_module ("overflow");
  REQUIRE ((m).has_value ());
  const std::pair<std::int64_t, std::int64_t> inputs[]
    = {{0, 0},         {INT64_MAX, 1},  {INT64_MIN, -1}, {-1, 2},
       {INT32_MAX, 1}, {INT32_MIN, -1}, {UINT32_MAX, 2}};
  for (auto op : {Opcode::Addo, Opcode::Subo, Opcode::Mulo, Opcode::UMulo, Opcode::Addos,
                  Opcode::Subos, Opcode::Mulos, Opcode::UMulos})
    for (unsigned n = 0; n < std::size (inputs); ++n)
      for (auto branch : {Opcode::Bo, Opcode::Bno, Opcode::UBo, Opcode::UBno}) {
        if ((op == Opcode::Mulo || op == Opcode::Mulos)
            && (branch == Opcode::UBo || branch == Opcode::UBno))
          continue;
        if ((op == Opcode::UMulo || op == Opcode::UMulos)
            && (branch == Opcode::Bo || branch == Opcode::Bno))
          continue;
        auto f
          = mirnext::ir::detail::access (((m)).value ())
              .create_function ("flags_" + std::string (instruction_descriptor (op).get ()->name)
                                  + "_" + std::string (instruction_descriptor (branch).get ()->name)
                                  + "_" + std::to_string (n),
                                {{Type::i64 ()}, {}});
        REQUIRE ((f).has_value ());
        auto b = mirnext::ir::detail::access (((f)).value ()).create_block (),
             no = mirnext::ir::detail::access (((f)).value ()).create_block (),
             yes = mirnext::ir::detail::access (((f)).value ()).create_block ();
        REQUIRE ((b).has_value ());
        REQUIRE ((no).has_value ());
        REQUIRE ((yes).has_value ());
        auto label = mirnext::ir::detail::access (((f)).value ()).create_label ((yes).value ());
        auto r = mirnext::ir::detail::access (((f)).value ()).create_register (Type::i64 ());
        REQUIRE ((label).has_value ());
        REQUIRE ((r).has_value ());
        InstructionBuilder builder ((b).value ());
        REQUIRE ((builder.emit (op, {Operand ((r).value ()), Operand (inputs[n].first),
                                     Operand (inputs[n].second)}))
                   .has_value ());
        REQUIRE ((builder.emit (branch, {Operand ((label).value ())})).has_value ());
        REQUIRE ((InstructionBuilder ((no).value ()).ret (0)).has_value ());
        REQUIRE ((InstructionBuilder ((yes).value ()).ret (1)).has_value ());
      }
  REQUIRE ((c->verify ()).has_value ());
  return c;
}
}  // namespace mirnext::test
