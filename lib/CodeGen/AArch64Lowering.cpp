#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/IR/Access.hpp"
/* Target rules derived from MIR, Copyright (C) 2020-2024 Vladimir Makarov.
   Licensed under the MIT License. Direct lowering of block-owned machine IR. */
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/CodeGen/Detail/TargetLowering.hpp"
#include <algorithm>
#include <limits>

namespace mirnext::codegen {
namespace {
using ir::Opcode;
using ir::Type;
using Kind = Type::Kind;
using Operand = MachineOperand;
using Instruction = MachineInstruction;
bool is_register (const Operand &o) {
  return detail::operand_access (o).get<const MachineVirtualRegister *> ()
         || detail::operand_access (o).get<target::PhysicalRegister> ();
}
std::optional<MachineRegister> register_value (const Operand &o) {
  if (auto v = detail::operand_access (o).get<const MachineVirtualRegister *> ())
    return detail::NodeAccess::make_MachineRegister (*v);
  if (auto p = detail::operand_access (o).get<target::PhysicalRegister> ()) return *p;
  return {};
}
Opcode move (Type t) {
  return t.kind () == Kind::F32          ? Opcode::FMov
         : t.kind () == Kind::F64        ? Opcode::DMov
         : t.kind () == Kind::LongDouble ? Opcode::LDMov
         : t.is_vector ()                ? Opcode::VMov
                                         : Opcode::Mov;
}
std::optional<Opcode> extension (Type t) {
  switch (t.kind ()) {
  case Kind::I8: return Opcode::Ext8;
  case Kind::U8: return Opcode::UExt8;
  case Kind::I16: return Opcode::Ext16;
  case Kind::U16: return Opcode::UExt16;
  case Kind::I32: return Opcode::Ext32;
  case Kind::U32: return Opcode::UExt32;
  default: return {};
  }
}
std::size_t align16 (std::size_t n) { return (n + 15) / 16 * 16; }
struct BuiltinDescription {
  Builtin id;
  std::string_view name;
  ir::Signature signature;
};
std::optional<BuiltinDescription> builtin (Opcode code) {
  auto i = Type::i64 (), f = Type (Kind::F32), d = Type (Kind::F64), ld = Type (Kind::LongDouble);
  switch (code) {
  case Opcode::I2LD:
    return BuiltinDescription{Builtin::IntegerToLongDouble, "mir.i2ld", {{ld}, {i}}};
  case Opcode::UI2LD:
    return BuiltinDescription{Builtin::UnsignedToLongDouble, "mir.ui2ld", {{ld}, {i}}};
  case Opcode::F2LD: return BuiltinDescription{Builtin::FloatToLongDouble, "mir.f2ld", {{ld}, {f}}};
  case Opcode::D2LD:
    return BuiltinDescription{Builtin::DoubleToLongDouble, "mir.d2ld", {{ld}, {d}}};
  case Opcode::LD2I:
    return BuiltinDescription{Builtin::LongDoubleToInteger, "mir.ld2i", {{i}, {ld}}};
  case Opcode::LD2F: return BuiltinDescription{Builtin::LongDoubleToFloat, "mir.ld2f", {{f}, {ld}}};
  case Opcode::LD2D:
    return BuiltinDescription{Builtin::LongDoubleToDouble, "mir.ld2d", {{d}, {ld}}};
  case Opcode::LDAdd:
    return BuiltinDescription{Builtin::LongDoubleAdd, "mir.ldadd", {{ld}, {ld, ld}}};
  case Opcode::LDSub:
    return BuiltinDescription{Builtin::LongDoubleSub, "mir.ldsub", {{ld}, {ld, ld}}};
  case Opcode::LDMul:
    return BuiltinDescription{Builtin::LongDoubleMul, "mir.ldmul", {{ld}, {ld, ld}}};
  case Opcode::LDDiv:
    return BuiltinDescription{Builtin::LongDoubleDiv, "mir.lddiv", {{ld}, {ld, ld}}};
  case Opcode::LDNeg: return BuiltinDescription{Builtin::LongDoubleNeg, "mir.ldneg", {{ld}, {ld}}};
  case Opcode::LDEq: return BuiltinDescription{Builtin::LongDoubleEq, "mir.ldeq", {{i}, {ld, ld}}};
  case Opcode::LDNe: return BuiltinDescription{Builtin::LongDoubleNe, "mir.ldne", {{i}, {ld, ld}}};
  case Opcode::LDLt: return BuiltinDescription{Builtin::LongDoubleLt, "mir.ldlt", {{i}, {ld, ld}}};
  case Opcode::LDLe: return BuiltinDescription{Builtin::LongDoubleLe, "mir.ldle", {{i}, {ld, ld}}};
  case Opcode::LDGt: return BuiltinDescription{Builtin::LongDoubleGt, "mir.ldgt", {{i}, {ld, ld}}};
  case Opcode::LDGe: return BuiltinDescription{Builtin::LongDoubleGe, "mir.ldge", {{i}, {ld, ld}}};
  case Opcode::VaArg:
    return BuiltinDescription{Builtin::VariadicScalar, "mir.va_arg", {{i}, {i, i}}};
  case Opcode::VaBlockArg:
    return BuiltinDescription{Builtin::VariadicBlock, "mir.va_block_arg", {{}, {i, i, i, i}}};
  default: return {};
  }
}
// This is the scalar tag of the existing variadic runtime ABI, not an IR
// opcode/type enum cast or a file-format dependency.
std::int64_t variadic_tag (Type type) {
  switch (type.kind ()) {
  case Kind::I8: return 0;
  case Kind::U8: return 1;
  case Kind::I16: return 2;
  case Kind::U16: return 3;
  case Kind::I32: return 4;
  case Kind::U32: return 5;
  case Kind::U64: return 7;
  case Kind::F32: return 8;
  case Kind::F64: return 9;
  case Kind::LongDouble: return 10;
  case Kind::Pointer: return 11;
  default: return 6;
  }
}
class AArch64Lowering {
 public:
  explicit AArch64Lowering (MachineFunction &f)
    : f (f), rw (f), state (f.state ()), mac (f.target ().abi () == target::ABI::AArch64MacOS) {}
  Result<void> run ();

 private:
  MachineFunction &f;
  MachineRewriter rw;
  MachineFunctionState state;
  bool mac;
  std::optional<Error> error;
  std::size_t integer_parameters = 0, floating_parameters = 0, stack_parameters = 0;
  void fail (std::string reason) {
    if (!error)
      error = Error{ErrorCode::InvalidOperand,
                    std::string (f.module_name ()) + '/' + std::string (f.name ()) + " ["
                      + std::string (f.target ().name ()) + ", machinize]: " + std::move (reason)};
  }
  void checked (Result<void> result) {
    if (!(result).has_value () && !error) fail (result.error ().message);
  }
  Operand physical (unsigned n) {
    return detail::NodeAccess::make_MachineOperand (f.target ().registers ()[n].reg);
  }
  MachineRegister physical_register (unsigned n) { return f.target ().registers ()[n].reg; }
  Operand integer (std::size_t n) {
    return detail::NodeAccess::make_MachineOperand (static_cast<std::int64_t> (n));
  }
  Operand temporary (Type t = Type::i64 ()) {
    if (error) return detail::NodeAccess::make_MachineOperand (std::int64_t (0));
    auto reg = detail::access (f).create_register (t);
    if (!(reg).has_value ()) {
      fail (reg.error ().message);
      return detail::NodeAccess::make_MachineOperand (std::int64_t (0));
    }
    return detail::NodeAccess::make_MachineOperand ((reg).value ());
  }
  Instruction *before (Instruction &at, Opcode code, std::vector<Operand> operands) {
    if (error) return &at;
    auto result = rw.insert_before (at, code, std::move (operands));
    if (!(result).has_value ()) {
      fail (result.error ().message);
      return &at;
    }
    return &(result).value ();
  }
  Instruction *after (Instruction &at, Opcode code, std::vector<Operand> operands) {
    if (error) return &at;
    auto result = rw.insert_after (at, code, std::move (operands));
    if (!(result).has_value ()) {
      fail (result.error ().message);
      return &at;
    }
    return &(result).value ();
  }
  void replace (Instruction &at, Opcode code, std::vector<Operand> operands) {
    if (!error) checked (rw.replace (at, code, std::move (operands)));
  }
  void erase (Instruction &at) {
    if (!error) checked (rw.erase (at));
  }
  Operand memory (Type t, std::int64_t offset, MachineRegister base,
                  std::optional<MachineRegister> index = {}) {
    return detail::NodeAccess::make_MachineOperand (
      detail::NodeAccess::make_MachineMemory (t, base, index, offset, 1, {}, {}));
  }
  Operand memory (Type t, std::int64_t offset, const Operand &base) {
    auto reg = register_value (base);
    if (!reg) {
      fail ("memory base must be a register");
      return detail::NodeAccess::make_MachineOperand (std::int64_t (0));
    }
    return memory (t, offset, *reg);
  }
  Operand address_memory (Instruction &, Type, std::int64_t, MachineRegister, bool hard);
  std::optional<unsigned> argument_register (Type, std::size_t &, std::size_t &);
  const MachineSymbol *prototype (const BuiltinDescription &);
  void copy_block (Instruction &, std::size_t, unsigned, std::size_t, MachineRegister, std::size_t,
                   std::size_t);
  void parameters (Instruction &);
  void lower_call (Instruction &);
  void lower_builtin (Instruction &, Opcode, const BuiltinDescription &);
  void lower_variadic_start (Instruction &);
  void lower_return (Instruction &);
  Type argument_type (const ir::Signature &, std::size_t, const Operand &);
  void check_call_area (const ir::Signature &, std::span<const Operand>);
  Result<void> preflight ();
};
Operand AArch64Lowering::address_memory (Instruction &at, Type t, std::int64_t offset,
                                         MachineRegister base, bool hard) {
  std::size_t scale = t.kind () == Kind::I8 || t.kind () == Kind::U8     ? 1
                      : t.kind () == Kind::I16 || t.kind () == Kind::U16 ? 2
                      : t.kind () == Kind::I32 || t.kind () == Kind::U32 || t.kind () == Kind::F32
                        ? 4
                      : t.kind () == Kind::LongDouble || t.is_vector () ? 16
                                                                        : 8;
  if (offset >= 0 && offset % scale == 0 && static_cast<std::uint64_t> (offset) / scale < 4096)
    return memory (t, offset, base);
  auto temp = hard ? physical (10) : temporary ();
  before (at, Opcode::Mov, {temp, detail::NodeAccess::make_MachineOperand (offset)});
  auto operand
    = std::visit ([] (auto value) { return detail::NodeAccess::make_MachineOperand (value); },
                  detail::NodeAccess::storage (base));
  before (at, Opcode::Add, {temp, temp, operand});
  return memory (t, 0, temp);
}
std::optional<unsigned> AArch64Lowering::argument_register (Type type, std::size_t &ints,
                                                            std::size_t &floats) {
  if (type.is_float () || type.is_vector ()) {
    auto n = floats++;
    return n < 8 ? std::optional<unsigned> (32 + n) : std::nullopt;
  }
  auto n = ints++;
  return n < 8 ? std::optional<unsigned> (n) : std::nullopt;
}
const MachineSymbol *AArch64Lowering::prototype (const BuiltinDescription &b) {
  // Builtins inhabit an owned namespace separate from user module identities.
  auto p = detail::access (f).intern_symbol ({"$builtin",
                                              std::string (b.name) + ".p",
                                              MachineSymbol::Kind::BuiltinPrototype,
                                              b.signature,
                                              false,
                                              {},
                                              b.id});
  if (!(p).has_value ()) {
    fail (p.error ().message);
    return nullptr;
  }
  return &(p).value ();
}
void AArch64Lowering::copy_block (Instruction &at, std::size_t to, unsigned to_base,
                                  std::size_t from, MachineRegister from_base, std::size_t words,
                                  std::size_t save) {
  auto temp = temporary ();
  if (words <= 16) {
    for (; words && !error; --words, to += 8, from += 8) {
      auto source = address_memory (at, Type::i64 (), from, from_base, false);
      before (at, Opcode::Mov, {temp, source});
      auto destination = address_memory (at, Type::i64 (), to, physical_register (to_base), true);
      before (at, Opcode::Mov, {destination, temp});
    }
    return;
  }
  auto t2 = temporary (), t3 = temporary ();
  if (save > 0) before (at, Opcode::Mov, {temp, physical (0)});
  if (save > 1) before (at, Opcode::Mov, {t2, physical (1)});
  if (save > 2) before (at, Opcode::Mov, {t3, physical (2)});
  BuiltinDescription b{Builtin::CopyBlock,
                       "mir.blk_mov",
                       {{}, {Type::i64 (), Type::i64 (), Type::i64 ()}}};
  auto proto = prototype (b);
  if (!proto) return;
  auto callee = temporary ();
  before (at, Opcode::Mov,
          {callee, detail::NodeAccess::make_MachineOperand (BuiltinReference{b.id})});
  before (at, Opcode::Add, {physical (0), physical (to_base), integer (to)});
  before (at, Opcode::Add,
          {physical (1),
           std::visit ([] (auto value) { return detail::NodeAccess::make_MachineOperand (value); },
                       detail::NodeAccess::storage (from_base)),
           integer (from)});
  before (at, Opcode::Mov, {physical (2), integer (words)});
  auto call = before (at, Opcode::Call,
                      {detail::NodeAccess::make_MachineOperand (
                         detail::NodeAccess::make_MachineSymbolReference (proto)),
                       callee, physical (0), physical (1), physical (2)});
  if (!error)
    checked (
      rw.lower_call (*call, Opcode::Call,
                     std::vector<Operand> (call->operands ().begin (), call->operands ().end ())));
  if (save > 0) before (at, Opcode::Mov, {physical (0), temp});
  if (save > 1) before (at, Opcode::Mov, {physical (1), t2});
  if (save > 2) before (at, Opcode::Mov, {physical (2), t3});
}
void AArch64Lowering::parameters (Instruction &at) {
  auto &area = std::get<MachineFunctionState::AArch64> (state.target).aggregate_save_bytes;
  area = 0;
  state.has_block_arguments = false;
  for (std::size_t i = 0;
       i < ir::detail::ReferenceAccess::pointers (f.arguments ()).size () && !error; ++i) {
    auto type = f.signature ().parameters[i];
    auto arg = detail::NodeAccess::make_MachineOperand (
      *ir::detail::ReferenceAccess::pointers (f.arguments ())[i]);
    if (type.kind () == Kind::ReturnBlock && !i) {
      before (at, Opcode::Mov, {arg, physical (8)});
      continue;
    }
    auto words = (type.block_size () + 7) / 8;
    if (type.kind () == Kind::Block && words <= 2) {
      if (integer_parameters + words <= 8) {
        area += words * 8;
        before (at, Opcode::Sub, {arg, physical (29), integer (area)});
        if (words)
          before (at, Opcode::Mov, {memory (Type::i64 (), 0, arg), physical (integer_parameters)});
        if (words == 2)
          before (at, Opcode::Mov,
                  {memory (Type::i64 (), 8, arg), physical (integer_parameters + 1)});
      } else {
        if (!state.has_block_arguments) {
          state.has_block_arguments = true;
          before (at, Opcode::Mov,
                  {physical (8), memory (Type::i64 (), 16, physical_register (29))});
        }
        before (at, Opcode::Add, {arg, physical (8), integer (stack_parameters)});
        stack_parameters += words * 8;
      }
      integer_parameters += words;
      continue;
    }
    auto reg = argument_register (type, integer_parameters, floating_parameters);
    if (reg)
      before (at, move (type), {arg, physical (*reg)});
    else {
      if (!state.has_block_arguments) {
        state.has_block_arguments = true;
        before (at, Opcode::Mov, {physical (8), memory (Type::i64 (), 16, physical_register (29))});
      }
      auto mem_type = (type.is_float () || type.is_vector ()) ? type : Type::i64 ();
      if (type.kind () == Kind::LongDouble || type.is_vector ())
        stack_parameters = align16 (stack_parameters);
      auto mem = address_memory (at, mem_type, stack_parameters, physical_register (8), true);
      before (at, move (type), {arg, mem});
      stack_parameters += type.kind () == Kind::LongDouble || type.is_vector () ? 16 : 8;
    }
  }
}
Type AArch64Lowering::argument_type (const ir::Signature &s, std::size_t position,
                                     const Operand &o) {
  if (position < s.parameters.size ()) return s.parameters[position];
  if (auto mem = detail::operand_access (o).get<MachineMemory> ()) return mem->type;
  if (auto v = detail::operand_access (o).get<const MachineVirtualRegister *> ())
    return (*v)->type ();
  if (detail::operand_access (o).get<double> ()) return Type (Kind::F64);
  if (detail::operand_access (o).get<long double> ()) return Type (Kind::LongDouble);
  if (detail::operand_access (o).get<float> ()) return Type (Kind::F32);
  return Type::i64 ();
}
void AArch64Lowering::check_call_area (const ir::Signature &s, std::span<const Operand> args) {
  // Check the same two layout walks before any copy/temporary is emitted. Each
  // individual block may be valid while their combined aligned area overflows.
  constexpr auto limit = static_cast<std::size_t> (INT64_MAX);
  auto add = [&] (std::size_t &value, std::size_t n) {
    if (n > limit - value) {
      fail ("outgoing argument area exceeds the signed address range");
      return;
    }
    value += n;
  };
  auto align = [&] (std::size_t &value) {
    if (value > limit - 15) {
      fail ("aligned argument area exceeds the signed address range");
      return;
    }
    value = align16 (value);
  };
  std::size_t ints = 0, floats = 0, block = 0, stack = 0;
  for (std::size_t i = 0; i < args.size () && !error; ++i) {
    auto t = argument_type (s, i, args[i]);
    if (t.kind () == Kind::ReturnBlock && !i) continue;
    if (mac && i == s.parameters.size ()) ints = floats = 8;
    auto words = (t.block_size () + 7) / 8;
    if (t.kind () == Kind::Block && words <= 2) {
      if (ints + words > 8) add (block, words * 8);
      ints += words;
    } else if (!argument_register (t, ints, floats)) {
      if (t.kind () == Kind::LongDouble || t.is_vector ()) align (block);
      add (block, t.kind () == Kind::LongDouble || t.is_vector () ? 16 : 8);
    }
  }
  if (error) return;
  align (block);
  ints = floats = 0;
  for (std::size_t i = 0; i < args.size () && !error; ++i) {
    auto t = argument_type (s, i, args[i]);
    if (t.kind () == Kind::ReturnBlock && !i) continue;
    if (mac && i == s.parameters.size ()) ints = floats = 8;
    auto words = (t.block_size () + 7) / 8;
    if (t.kind () == Kind::Block) {
      if (words <= 2) {
        if (ints + words > 8) {
          add (stack, words * 8);
          add (block, words * 8);
        }
        ints += words;
        continue;
      }
      add (block, words * 8);
    }
    if (!argument_register (t, ints, floats)) {
      if (t.kind () == Kind::LongDouble || t.is_vector ()) align (stack);
      add (stack, t.kind () == Kind::LongDouble || t.is_vector () ? 16 : 8);
    }
  }
  if (error) return;
  align (block);
  if (block) stack = block;
  align (stack);
}
void AArch64Lowering::lower_call (Instruction &at) {
  auto ops = std::vector<Operand> (at.operands ().begin (), at.operands ().end ());
  const auto signature = *detail::NodeAccess::symbol (
                            *(detail::operand_access (ops[0]).get<MachineSymbolReference> ()))
                            ->signature;
  const auto start = 2 + signature.results.size ();
  auto code = std::get<Opcode> (at.opcode ());
  if (code == Opcode::Inline) code = Opcode::Call;
  // Capture the original predecessor before inserting callee/argument moves.
  auto predecessor = rw.previous (at);
  if (!(predecessor).has_value ()) {
    fail (predecessor.error ().message);
    return;
  }
  Instruction *previous = predecessor.value ().get ();
  Instruction *prefix = previous;
  auto prefix_insert = [&] (Opcode op, std::vector<Operand> values) {
    if (prefix)
      prefix = after (*prefix, op, std::move (values));
    else {
      auto &first = *const_cast<MachineBasicBlock &> (at.block ()).instructions ().begin ();
      prefix = before (first, op, std::move (values));
    }
    checked (rw.set_entry_setup (*prefix, at.is_entry_setup ()));
    return prefix;
  };
  if (!is_register (ops[1])) {
    auto temp = temporary ();
    before (at, Opcode::Mov, {temp, ops[1]});
    ops[1] = temp;
  }
  std::size_t ints = 0, floats = 0, stack = 0, block = 0;
  for (std::size_t i = start; i < ops.size (); ++i) {
    auto t = argument_type (signature, i - start, ops[i]);
    if (t.kind () == Kind::ReturnBlock && i == start) continue;
    if (mac && i - start == signature.parameters.size ()) ints = floats = 8;
    auto words = (t.block_size () + 7) / 8;
    if (t.kind () == Kind::Block && words <= 2) {
      if (ints + words > 8) block += words * 8;
      ints += words;
    } else if (!argument_register (t, ints, floats)) {
      if (t.kind () == Kind::LongDouble || t.is_vector ()) block = align16 (block);
      block += t.kind () == Kind::LongDouble || t.is_vector () ? 16 : 8;
    }
  }
  block = align16 (block);
  ints = floats = 0;
  for (std::size_t i = start; i < ops.size () && !error; ++i) {
    if (mac && i - start == signature.parameters.size ()) ints = floats = 8;
    auto arg = ops[i];
    auto type = argument_type (signature, i - start, arg);
    auto ext = extension (type);
    std::optional<Operand> original;
    if (ext) {
      original = arg;
      arg = temporary ();
      ops[i] = arg;
    }
    auto mem = detail::operand_access (arg).get<MachineMemory> ();
    if (type.kind () == Kind::ReturnBlock && i == start) {
      before (at, Opcode::Mov,
              {physical (8),
               std::visit ([] (auto r) { return detail::NodeAccess::make_MachineOperand (r); },
                           detail::NodeAccess::storage (*mem->base))});
      ops[i] = physical (8);
      continue;
    }
    if (type.kind () == Kind::Block) {
      auto words = (type.block_size () + 7) / 8;
      auto base = *mem->base;
      if (words <= 2) {
        if (ints + words <= 8) {
          ops[i]
            = memory (Type (Kind::Undef), 0, physical_register (ints),
                      words < 2
                        ? std::nullopt
                        : detail::NodeAccess::optional_register (physical_register (ints + 1)));
          if (words) before (at, Opcode::Mov, {physical (ints++), memory (Type::i64 (), 0, base)});
          if (words == 2)
            before (at, Opcode::Mov, {physical (ints++), memory (Type::i64 (), 8, base)});
        } else {
          copy_block (at, stack, 31, 0, base, words, ints);
          ops[i] = memory (Type (Kind::Undef), stack, physical_register (31));
          stack += words * 8;
          block += words * 8;
          ints += words;
        }
        continue;
      }
      copy_block (at, block, 31, 0, base, words, ints);
      arg = temporary ();
      // The original traversal advances the insertion cursor to the old next
      // instruction after this address computation, ahead of argument moves.
      auto added = prefix_insert (Opcode::Add, {arg, physical (31), integer (block)});
      auto next = rw.next (*added);
      if (!(next).has_value ()) {
        fail (next.error ().message);
        return;
      }
      if (next.value ().has_value ()) prefix = next.value ().get ();
      block += words * 8;
    }
    auto reg = argument_register (type, ints, floats);
    if (reg) {
      if (original) before (at, *ext, {arg, *original});
      auto source = arg;
      if (type.kind () == Kind::ReturnBlock)
        source = std::visit ([] (auto r) { return detail::NodeAccess::make_MachineOperand (r); },
                             detail::NodeAccess::storage (*mem->base));
      before (at, move (type), {physical (*reg), source});
      ops[i] = type.kind () == Kind::ReturnBlock
                 ? memory (type, type.block_size (), physical_register (*reg))
                 : physical (*reg);
    } else {
      if (type.kind () == Kind::LongDouble || type.is_vector ()) stack = align16 (stack);
      auto mem_type = (type.is_float () || type.is_vector ()) ? type : Type::i64 ();
      std::size_t scale = mem_type.kind () == Kind::F32                                   ? 4
                          : mem_type.kind () == Kind::LongDouble || mem_type.is_vector () ? 16
                                                                                          : 8;
      auto destination = memory (mem_type, stack, physical_register (31));
      if (stack % scale || stack / scale >= 4096) {
        prefix_insert (Opcode::Mov, {physical (10), integer (stack)});
        prefix_insert (Opcode::Add, {physical (10), physical (10), physical (31)});
        destination = memory (mem_type, 0, physical_register (10));
      }
      if (original) prefix_insert (*ext, {arg, *original});
      auto source
        = type.kind () == Kind::ReturnBlock
            ? std::visit ([] (auto r) { return detail::NodeAccess::make_MachineOperand (r); },
                          detail::NodeAccess::storage (*mem->base))
            : arg;
      prefix_insert (move (type), {destination, source});
      ops[i] = destination;
      stack += type.kind () == Kind::LongDouble || type.is_vector () ? 16 : 8;
    }
  }
  block = align16 (block);
  if (block) stack = block;
  std::size_t ret_ints = 0, ret_floats = 0;
  for (std::size_t i = 0; i < signature.results.size () && !error; ++i) {
    auto t = signature.results[i];
    auto &count = (t.is_float () || t.is_vector ()) ? ret_floats : ret_ints;
    auto physical_result = physical (((t.is_float () || t.is_vector ()) ? 32 : 0) + count++);
    auto result = ops[i + 2];
    auto moved = after (at, move (t), {result, physical_result});
    ops[i + 2] = physical_result;
    if (auto ext = extension (t)) after (*moved, *ext, {result, result});
  }
  if (stack) {
    stack = align16 (stack);
    auto adjusted
      = previous ? after (*previous, Opcode::Sub, {physical (31), physical (31), integer (stack)})
                 : before (*const_cast<MachineBasicBlock &> (at.block ()).instructions ().begin (),
                           Opcode::Sub, {physical (31), physical (31), integer (stack)});
    checked (rw.set_entry_setup (*adjusted, at.is_entry_setup ()));
    after (at, Opcode::Add, {physical (31), physical (31), integer (stack)});
  }
  if (!error) checked (rw.lower_call (at, code, std::move (ops)));
  state.is_leaf = false;
}
void AArch64Lowering::lower_builtin (Instruction &at, Opcode code, const BuiltinDescription &b) {
  auto p = prototype (b);
  if (!p) return;
  auto operands = std::vector<Operand> (at.operands ().begin (), at.operands ().end ());
  auto callee = temporary ();
  before (at, Opcode::Mov,
          {callee, detail::NodeAccess::make_MachineOperand (BuiltinReference{b.id})});
  std::vector<Operand> call{detail::NodeAccess::make_MachineOperand (
                              detail::NodeAccess::make_MachineSymbolReference (p)),
                            callee};
  if (code == Opcode::VaArg || code == Opcode::VaBlockArg) {
    auto type = temporary ();  // Keep temporary allocation order, including the block case.
    if (code == Opcode::VaArg) {
      before (at, Opcode::Mov,
              {type, detail::NodeAccess::make_MachineOperand (variadic_tag (
                       detail::operand_access (operands[2]).get<MachineMemory> ()->type))});
      call.insert (call.end (), {operands[0], operands[1], type});
    } else
      call.insert (call.end (), operands.begin (), operands.end ());
  } else
    call.insert (call.end (), operands.begin (), operands.end ());
  auto created = before (at, Opcode::Call, std::move (call));
  if (!error) {
    erase (at);
    lower_call (*created);
  }
}
void AArch64Lowering::lower_variadic_start (Instruction &at) {
  auto va = at.operands ()[0];
  std::optional<Operand> temp;
  if (!mac) temp = temporary ();
  auto previous_sp = temporary ();
  if (!mac) {
    before (at, Opcode::Mov,
            {*temp,
             detail::NodeAccess::make_MachineOperand (
               integer_parameters >= 8 ? std::int64_t (0)
                                       : static_cast<std::int64_t> (8 * integer_parameters) - 64)});
    before (at, Opcode::Mov, {memory (Type (Kind::U32), 24, va), *temp});
    before (at, Opcode::Mov,
            {*temp, detail::NodeAccess::make_MachineOperand (
                      floating_parameters >= 8
                        ? std::int64_t (0)
                        : static_cast<std::int64_t> (16 * floating_parameters) - 128)});
    before (at, Opcode::Mov, {memory (Type (Kind::U32), 28, va), *temp});
  }
  before (at, Opcode::Mov, {previous_sp, memory (Type::i64 (), 16, physical_register (29))});
  if (mac)
    before (at, Opcode::Mov, {memory (Type::i64 (), 0, va), previous_sp});
  else {
    before (at, Opcode::Add, {*temp, previous_sp, integer (stack_parameters)});
    before (at, Opcode::Mov, {memory (Type::i64 (), 0, va), *temp});
    before (at, Opcode::Mov, {memory (Type::i64 (), 8, va), previous_sp});
    before (at, Opcode::Sub, {*temp, previous_sp, integer (64)});
    before (at, Opcode::Mov, {memory (Type::i64 (), 16, va), *temp});
  }
  erase (at);
}
void AArch64Lowering::lower_return (Instruction &at) {
  auto ops = std::vector<Operand> (at.operands ().begin (), at.operands ().end ());
  std::size_t ints = 0, floats = 0;
  for (std::size_t i = 0; i < ops.size (); ++i) {
    auto type = f.signature ().results[i];
    auto reg = physical ((type.is_float () || type.is_vector ()) ? 32 + floats++ : ints++);
    before (at, move (type), {reg, ops[i]});
    ops[i] = reg;
  }
  replace (at, Opcode::Ret, std::move (ops));
}
Result<void> AArch64Lowering::preflight () {
  auto valid = f.verify ();
  if (!(valid).has_value ()) {
    fail (valid.error ().message);
    return *error;
  }
  if (f.state ().machinized) {
    fail ("machine function is already lowered");
    return *error;
  }
  auto returns_valid = [&] (const ir::Signature &signature) {
    std::size_t ints = 0, floats = 0;
    for (auto t : signature.results)
      if (((t.is_float () || t.is_vector ()) ? ++floats : ++ints) > 8) return false;
    return true;
  };
  if (!returns_valid (f.signature ()))
    fail ("aarch64 cannot handle this combination of return values");
  for (auto &block : f.blocks ())
    for (auto &insn : block.instructions ()) {
      auto code = insn.opcode ();
      auto op = std::get_if<Opcode> (&code);
      if (!op) continue;
      auto desc = ir::instruction_descriptor (*op).get ();
      if (desc->count == ir::OperandCount::Call) {
        if (insn.has_lowered_call ()) {
          fail ("call is already lowered");
          continue;
        }
        auto &s = *detail::NodeAccess::symbol (
                     *(detail::operand_access (insn.operands ()[0]).get<MachineSymbolReference> ()))
                     ->signature;
        if (!returns_valid (s)) fail ("aarch64 cannot handle this combination of call results");
        auto start = 2 + s.results.size ();
        for (std::size_t p = start; p < insn.operands ().size (); ++p) {
          auto &o = insn.operands ()[p];
          auto t = argument_type (s, p - start, o);
          if (t.is_block ()) {
            auto m = detail::operand_access (o).get<MachineMemory> ();
            if (!m || !m->base || m->index || m->displacement != 0)
              fail ("block arguments require a base register without an offset or index");
          } else if (!is_register (o))
            fail ("call scalar operands must be simplified to registers");
          if (p - start >= s.parameters.size () && t.kind () == Kind::F32)
            fail ("passing float variadic argument (must be promoted to double)");
        }
        if (!error) check_call_area (s, insn.operands ().subspan (start));
      }
      if (*op == Opcode::VaStart && !f.signature ().variadic)
        fail ("va_start requires a variadic function");
      if (*op == Opcode::Ret)
        for (auto &o : insn.operands ())
          if (!is_register (o)) fail ("return operands must be simplified to registers");
    }
  return error ? Result<void> (*error) : Result<void> ();
}
Result<void> AArch64Lowering::run () {
  auto valid = preflight ();
  if (!(valid).has_value ()) return valid;
  // Preparation keeps an entry block separate from any user label at entry.
  // A temporary USE supplies a checked insertion anchor even for an empty block.
  auto &entry = *f.blocks ().begin ();
  auto anchor = rw.append (entry, MachinePseudoOpcode::Use, {});
  if (!(anchor).has_value ()) return anchor.error ();
  auto setup = rw.set_entry_setup ((anchor).value ());
  if (!(setup).has_value ()) return setup;
  if (entry.instructions ().begin () != --entry.instructions ().end ()) {
    auto first = &*entry.instructions ().begin ();
    auto moved = rw.move_before ((anchor).value (), *first);
    if (!(moved).has_value ()) return moved;
  }
  parameters ((anchor).value ());
  erase ((anchor).value ());
  if (error) return *error;
  state.uses_alloca = false;
  state.is_leaf = true;
  std::vector<Instruction *> work;
  work.reserve (f.instruction_count ());
  for (auto &block : f.blocks ())
    for (auto &insn : block.instructions ()) work.push_back (&insn);
  for (auto insn : work) {
    if (error) break;
    auto machine_code = insn->opcode ();
    auto op = std::get_if<Opcode> (&machine_code);
    if (!op) continue;
    auto code = *op;
    if (code == Opcode::LDBeq || code == Opcode::LDBne || code == Opcode::LDBlt
        || code == Opcode::LDBle || code == Opcode::LDBgt || code == Opcode::LDBge) {
      auto result = temporary ();
      auto args = insn->operands ();
      auto compare = code == Opcode::LDBeq   ? Opcode::LDEq
                     : code == Opcode::LDBne ? Opcode::LDNe
                     : code == Opcode::LDBlt ? Opcode::LDLt
                     : code == Opcode::LDBle ? Opcode::LDLe
                     : code == Opcode::LDBgt ? Opcode::LDGt
                                             : Opcode::LDGe;
      auto test = before (*insn, compare, {result, args[1], args[2]});
      auto label = args[0];
      replace (*insn, Opcode::Bt, {label, result});
      if (auto b = builtin (compare)) lower_builtin (*test, compare, *b);
    } else if (auto b = builtin (code))
      lower_builtin (*insn, code, *b);
    else if (code == Opcode::VaStart)
      lower_variadic_start (*insn);
    else if (code == Opcode::VaEnd)
      erase (*insn);
    else if (code == Opcode::Call || code == Opcode::Inline || code == Opcode::JCall)
      lower_call (*insn);
    else if (code == Opcode::Alloca)
      state.uses_alloca = true;
    else if (code == Opcode::FBlt || code == Opcode::FBle || code == Opcode::DBlt
             || code == Opcode::DBle) {
      auto args = insn->operands ();
      replace (*insn,
               code == Opcode::FBlt   ? Opcode::FBgt
               : code == Opcode::FBle ? Opcode::FBge
               : code == Opcode::DBlt ? Opcode::DBgt
                                      : Opcode::DBge,
               {args[0], args[2], args[1]});
    } else if (code == Opcode::Ret)
      lower_return (*insn);
  }
  if (error) return *error;
  state.machinized = true;
  checked (rw.set_state (state));
  if (error) return *error;
  return f.verify ();
}
}  // namespace
Result<void> machinize_aarch64 (MachineFunction &function) {
  if (function.target ().architecture () != target::Architecture::AArch64)
    return Error{ErrorCode::InvalidArgument, std::string (function.module_name ()) + '/'
                                               + std::string (function.name ()) + " ["
                                               + std::string (function.target ().name ())
                                               + ", machinize]: unsupported target lowering"};
  return AArch64Lowering (function).run ();
}
}  // namespace mirnext::codegen
