#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/IR/Access.hpp"
/* MIR encoding rules, Copyright (C) 2018-2024 Vladimir Makarov. MIT License. */
#include "mirnext/CodeGen/Detail/X86Encoding.hpp"
#include "mirnext/CodeGen/Detail/CompiledFunction.hpp"
#include "mirnext/CodeGen/Detail/CompiledBlock.hpp"
#include <algorithm>
#include <bit>
#include <climits>
#include <array>
namespace mirnext::codegen::detail {
namespace {
using ir::Opcode;
using R = target::X86Register;
std::optional<std::uint64_t> integer (const MachineOperand &o) {
  if (auto n = detail::operand_access (o).get<std::int64_t> ())
    return std::bit_cast<std::uint64_t> (*n);
  if (auto n = detail::operand_access (o).get<std::uint64_t> ()) return *n;
  return {};
}
}  // namespace
Result<X86AddressEncoding> encode_x86_address (std::optional<R> base, std::optional<R> index,
                                               std::int64_t disp, unsigned scale) {
  auto invalid = [] {
    return Error{ErrorCode::InvalidOperand, "x86 address: invalid register, scale or displacement"};
  };
  if ((base && static_cast<unsigned> (*base) >= 16)
      || (index && (static_cast<unsigned> (*index) >= 16 || *index == R::SP)) || disp < INT32_MIN
      || disp > INT32_MAX || (index && scale != 1 && scale != 2 && scale != 4 && scale != 8))
    return invalid ();
  X86AddressEncoding out;
  out.sib = index || !base || *base == R::SP || *base == R::R12;
  out.rex_b = base && static_cast<unsigned> (*base) >= 8;
  out.rex_x = index && static_cast<unsigned> (*index) >= 8;
  out.rm = out.sib ? 4 : static_cast<unsigned> (*base) & 7;
  out.base = base ? static_cast<unsigned> (*base) & 7 : 5;
  out.index = index ? static_cast<unsigned> (*index) & 7 : 4;
  out.scale = !index || scale == 1 ? 0 : scale == 2 ? 1 : scale == 4 ? 2 : 3;
  if (!base)
    out.displacement_bytes = 4;
  else if (disp || *base == R::BP || *base == R::R13) {
    out.mod = disp >= -128 && disp <= 127 ? 1 : 2;
    out.displacement_bytes = out.mod == 1 ? 1 : 4;
  }
  out.displacement = static_cast<std::uint32_t> (disp);
  return out;
}
struct X86EncodingEngine {
  const MachineFunction &f;
  const MachineFrameResult &frame;
  MachineEncodingOptions options;
  CompiledFunctionData result;
  struct LabelFixup {
    const MachineLabel *label;
    std::size_t offset, next;
    unsigned size;
    std::size_t operand = 0;
  };
  struct PoolFixup {
    MachineOperand value;
    std::size_t offset, next;
    bool call;
  };
  struct Known {
    MachineOperand operand;
    std::uint64_t value;
  };
  struct Prepared {
    const MachineInstruction *instruction;
    std::optional<std::vector<MachineOperand>> normalized;
    std::size_t pattern = 0;
    std::span<const MachineOperand> operands () const {
      return normalized ? std::span<const MachineOperand> (*normalized) : instruction->operands ();
    }
    void change (std::size_t n, MachineOperand o) {
      if (!normalized)
        normalized.emplace (instruction->operands ().begin (), instruction->operands ().end ());
      (*normalized)[n] = std::move (o);
    }
  };
  std::vector<Prepared> prepared;
  std::vector<LabelFixup> labels;
  std::vector<PoolFixup> pool;
  std::vector<Known> known;
  std::unordered_map<const MachineLabel *, std::size_t> offsets;
  std::unordered_set<const MachineLabel *> aligned;
  X86EncodingEngine (const MachineFunction &f, const MachineFrameResult &frame,
                     MachineEncodingOptions options)
    : f (f),
      frame (frame),
      options (std::move (options)),
      result (std::string (f.module_name ()), std::string (f.name ()), f.target ().architecture (),
              frame.layout ()) {
    aligned.insert (ir::detail::ReferenceAccess::pointers (frame.aligned_loop_labels ()).begin (),
                    ir::detail::ReferenceAccess::pointers (frame.aligned_loop_labels ()).end ());
  }
  Error invalid (std::string why) const {
    return {ErrorCode::InvalidOperand,
            std::string (f.module_name ()) + '/' + std::string (f.name ()) + " ["
              + std::string (f.target ().name ()) + ", encode]: " + std::move (why)};
  }
  void put (std::uint64_t value, unsigned size) {
    for (unsigned n = 0; n < size; ++n, value >>= 8) result.bytes_.push_back (value & 255);
  }
  void set (std::size_t at, std::uint64_t value, unsigned size) {
    for (unsigned n = 0; n < size; ++n, value >>= 8) result.bytes_[at + n] = value & 255;
  }
  Result<unsigned> physical (const MachineOperand &o) const {
    auto r = detail::operand_access (o).get<target::PhysicalRegister> ();
    if (!r || r->architecture () != target::Architecture::X86_64 || r->encoding () >= 32)
      return invalid ("expected allocated x86 register");
    return r->encoding ();
  }
  Result<std::uint64_t> resolve (const MachineOperand &operand) {
    if (auto n = integer (operand)) return *n;
    auto symbol = detail::operand_access (operand).get<MachineSymbolReference> ();
    auto builtin = detail::operand_access (operand).get<BuiltinReference> ();
    if ((!symbol && !builtin) || !options.known_addresses) return invalid ("unresolved immediate");
    for (auto &v : known)
      if (v.operand == operand) return v.value;
    auto r = options.known_addresses (
      CodeSymbolRequest (::mirnext::detail::borrow_optional (
                           symbol ? detail::NodeAccess::symbol (*(symbol)) : nullptr),
                         builtin ? std::optional (builtin->builtin) : std::nullopt));
    if (!(r).has_value ()) {
      auto error = invalid (r.error ().message);
      error.code = r.error ().code;
      return error;
    }
    auto value = (r).value () + (symbol ? std::bit_cast<std::uint64_t> (symbol->displacement) : 0);
    known.push_back ({operand, value});
    return value;
  }
  Result<void> relocation (const MachineOperand &operand, std::size_t offset) {
    auto symbol = detail::operand_access (operand).get<MachineSymbolReference> ();
    auto builtin = detail::operand_access (operand).get<BuiltinReference> ();
    if (!symbol && !builtin) return invalid ("invalid symbolic immediate");
    result.relocations_.push_back (
      {CodeRelocation::Kind::AbsoluteSymbol, offset, 0,
       symbol ? std::optional (*detail::NodeAccess::symbol (*(symbol))) : std::nullopt,
       builtin ? std::optional (builtin->builtin) : std::nullopt,
       symbol ? symbol->displacement : 0});
    return {};
  }
  Result<void> normalize (Prepared &p) {
    auto ops = p.operands ();
    if (p.instruction->opcode () == MachineOpcode (Opcode::Alloca))
      if (auto n = integer (ops[1])) {
        if (*n > std::uint64_t (INT64_MAX) - 15)
          return invalid ("stack allocation roundup overflow");
        p.change (1, detail::NodeAccess::make_MachineOperand ((*n + 15) & ~std::uint64_t{15}));
      }
    if (options.known_addresses)
      for (std::size_t n = 0; n < ops.size (); ++n) {
        if (n == 0 && p.instruction->has_lowered_call ()) continue;
        if (detail::operand_access (ops[n]).get<MachineSymbolReference> ()
            || detail::operand_access (ops[n]).get<BuiltinReference> ()) {
          auto r = resolve (ops[n]);
          if (!(r).has_value ()) return r.error ();
          p.change (n, detail::NodeAccess::make_MachineOperand ((r).value ()));
        }
      }
    return {};
  }
  Result<void> instruction (const Prepared &p, std::size_t pattern) {
    auto recipe = x86_encoding_recipe (pattern);
    if (!recipe) return invalid ("invalid encoding recipe");
    auto ops = p.operands ();
    std::optional<std::size_t> table_at;
    for (auto &word : recipe->words) {
      std::array<unsigned, 3> opcode{};
      unsigned opcode_count = 0;
      std::optional<unsigned> prefix, mod, reg, rm, scale, index, base, disp_size, imm_size;
      unsigned rex = 0;
      bool force_rex = false;
      std::optional<unsigned> low_opcode;
      std::uint64_t disp = 0, imm = 0;
      const MachineOperand *symbolic_imm = nullptr;
      const MachineLabel *label = nullptr;
      std::size_t label_operand = 0;
      const MachineOperand *constant = nullptr;
      bool call = false, table = false;
      MachineOperand literal (std::uint64_t{0});
      auto operand = [&] (std::uint64_t n) -> const MachineOperand * {
        return n < ops.size () ? &ops[n] : nullptr;
      };
      auto number = [&] (std::uint64_t n) -> Result<unsigned> {
        auto o = operand (n);
        return o ? physical (*o) : Result<unsigned> (invalid ("missing register operand"));
      };
      auto encoded_reg = [&] (unsigned n, unsigned bit, bool byte = false) {
        if (n >= 16) n -= 16;
        if (byte && n >= 4) force_rex = true;
        if (n >= 8) rex |= bit;
        return n & 7;
      };
      auto memory
        = [&] (std::optional<R> b, std::optional<R> i, std::int64_t d, unsigned s) -> Result<void> {
        if (mod || rm || scale || index || base || disp_size)
          return invalid ("duplicate address fields");
        auto a = encode_x86_address (b, i, d, s);
        if (!(a).has_value ()) return invalid (a.error ().message);
        mod = a.value ().mod;
        rm = a.value ().rm;
        if (a.value ().rex_b) rex |= 1;
        if (a.value ().rex_x) rex |= 2;
        if (a.value ().sib) {
          scale = a.value ().scale;
          index = a.value ().index;
          base = a.value ().base;
        }
        if (a.value ().displacement_bytes) {
          disp_size = a.value ().displacement_bytes;
          disp = a.value ().displacement;
        }
        return {};
      };
      for (auto &field : recipe->operations.subspan (word.first_operation, word.operation_count)) {
        using K = X86EncodingKind;
        auto n = field.value;
        switch (field.kind) {
        case K::Byte:
          if (opcode_count == opcode.size ()) return invalid ("too many opcode bytes");
          opcode[opcode_count++] = n;
          break;
        case K::Rex:
          if (opcode_count) {
            if (opcode_count != 1 || prefix) return invalid ("duplicate prefix");
            prefix = opcode[0];
            opcode_count = 0;
          }
          if (n == 'X') rex |= 8;
          if (n == 'Z') force_rex = true;
          break;
        case K::Register:
        case K::RegisterRM:
        case K::ByteRegisterRM:
        case K::OpcodeRegister: {
          auto r = number (n);
          if (!(r).has_value ()) return r.error ();
          auto v = encoded_reg ((r).value (), field.kind == K::Register ? 4 : 1,
                                field.kind == K::ByteRegisterRM);
          if (field.kind == K::Register)
            reg = v;
          else if (field.kind == K::OpcodeRegister)
            low_opcode = v;
          else {
            rm = v;
            mod = 3;
          }
          break;
        }
        case K::PhysicalRegister: reg = encoded_reg (n, 4); break;
        case K::PhysicalRM:
          rm = encoded_reg (n, 1);
          mod = 3;
          break;
        case K::OpcodePhysical: low_opcode = encoded_reg (n, 1); break;
        case K::Extension: reg = n; break;
        case K::Memory: {
          auto o = operand (n);
          auto m = o ? detail::operand_access (*(o)).get<MachineMemory> () : nullptr;
          if (!m) return invalid ("missing memory operand");
          std::optional<R> b, i;
          if (m->base) {
            auto r
              = std::get_if<target::PhysicalRegister> (detail::NodeAccess::storage (&*m->base));
            if (!r) return invalid ("unallocated address base");
            b = static_cast<R> (r->encoding ());
          }
          if (m->index) {
            auto r
              = std::get_if<target::PhysicalRegister> (detail::NodeAccess::storage (&*m->index));
            if (!r) return invalid ("unallocated address index");
            i = static_cast<R> (r->encoding ());
          }
          auto r = memory (b, i, m->displacement, m->scale);
          if (!(r).has_value ()) return r.error ();
          break;
        }
        case K::TemporaryMemory: {
          auto r = memory (R::SP, {}, -16, 1);
          if (!(r).has_value ()) return r.error ();
          break;
        }
        case K::SwitchMemory: {
          auto r = number (0);
          if (!(r).has_value ()) return r.error ();
          auto a = memory (R::R11, static_cast<R> ((r).value ()), 0, 8);
          if (!(a).has_value ()) return a.error ();
          break;
        }
        case K::AddressAdd:
        case K::AddressDisplacement:
        case K::AddressScale: {
          auto r = number (1);
          if (!(r).has_value ()) return r.error ();
          std::optional<R> b = static_cast<R> ((r).value ()), i;
          std::int64_t d = 0;
          unsigned s = 1;
          if (field.kind == K::AddressDisplacement)
            d = std::bit_cast<std::int64_t> (n);
          else {
            auto o = operand (2);
            if (!o) return invalid ("missing address operand");
            if (field.kind == K::AddressScale) {
              auto v = integer (*o);
              if (!v || (*v != 1 && *v != 2 && *v != 4 && *v != 8))
                return invalid ("invalid address scale");
              s = *v;
              i = b;
              b.reset ();
            } else if (detail::operand_access (*(o)).get<target::PhysicalRegister> ()) {
              auto r = physical (*o);
              if (!(r).has_value ()) return r.error ();
              i = static_cast<R> ((r).value ());
            } else {
              auto v = integer (*o);
              if (!v) return invalid ("noninteger address displacement");
              d = std::bit_cast<std::int64_t> (*v);
            }
          }
          auto a = memory (b, i, d, s);
          if (!(a).has_value ()) return a.error ();
          break;
        }
        case K::Immediate8:
        case K::Immediate32:
        case K::Immediate64: {
          auto o = operand (n);
          if (!o) return invalid ("missing immediate operand");
          imm_size = field.kind == K::Immediate8 ? 1 : field.kind == K::Immediate32 ? 4 : 8;
          if (auto v = integer (*o))
            imm = *v;
          else if (*imm_size == 8)
            symbolic_imm = o;
          else
            return invalid ("unresolved narrow immediate");
          break;
        }
        case K::Literal8:
        case K::Literal32:
          imm_size = field.kind == K::Literal8 ? 1 : 4;
          imm = n;
          break;
        case K::RIPMemory:
          mod = 0;
          rm = 5;
          break;
        case K::SwitchTable:
          if (table_at) return invalid ("duplicate switch table");
          table = true;
          mod = 0;
          rm = 5;
          break;
        case K::Label8:
        case K::Label32: {
          auto o = operand (n);
          auto l = o ? detail::operand_access (*(o)).get<const MachineLabel *> () : nullptr;
          if (!l || label || disp_size) return invalid ("invalid label field");
          label = *l;
          label_operand = n;
          disp_size = field.kind == K::Label8 ? 1 : 4;
          break;
        }
        case K::PoolConstant:
          literal = detail::NodeAccess::make_MachineOperand (n);
          constant = &literal;
          mod = 0;
          rm = 5;
          disp_size = 4;
          break;
        case K::CallPointer:
          constant = operand (n);
          if (!constant) return invalid ("missing call target");
          call = true;
          mod = 0;
          rm = 5;
          disp_size = 4;
          break;
        }
      }
      if (!opcode_count) return invalid ("invalid opcode bytes");
      auto start = result.bytes_.size ();
      if (prefix) put (*prefix, 1);
      if (rex || force_rex) put (0x40 | rex, 1);
      for (std::size_t n = 0; n < opcode_count; ++n)
        put (opcode[n] | (n == 0 ? low_opcode.value_or (0) : 0), 1);
      if (mod || reg || rm)
        put ((mod.value_or (0) << 6) | (reg.value_or (0) << 3) | rm.value_or (0), 1);
      if (scale || index || base)
        put ((scale.value_or (0) << 6) | (index.value_or (0) << 3) | base.value_or (0), 1);
      auto displacement_at = result.bytes_.size ();
      if (disp_size) put (disp, *disp_size);
      if (imm_size) {
        if (symbolic_imm) {
          auto r = relocation (*symbolic_imm, result.bytes_.size ());
          if (!(r).has_value ()) return r.error ();
        }
        put (imm, *imm_size);
      }
      if (table) {
        table_at = result.bytes_.size ();
        put (0, 4);
      }
      if (label)
        labels.push_back (
          {label, displacement_at, result.bytes_.size (), *disp_size, label_operand});
      if (constant) pool.push_back ({*constant, displacement_at, result.bytes_.size (), call});
      if (result.bytes_.size () - start > word.maximum_size)
        return invalid ("encoding exceeds conservative instruction size");
    }
    if (table_at) {
      while (result.bytes_.size () % 8) put (0, 1);
      auto displacement = result.bytes_.size () - *table_at - 4;
      if (displacement > INT32_MAX) return invalid ("switch table too far");
      set (*table_at, displacement, 4);
      for (std::size_t n = 1; n < ops.size (); ++n) {
        auto l = detail::operand_access (ops[n]).get<const MachineLabel *> ();
        if (!l) return invalid ("invalid switch target");
        labels.push_back ({*l, result.bytes_.size (), 0, 8, n});
        put (0, 8);
      }
    }
    return {};
  }
  Result<CompiledFunction> run () {
    prepared.reserve (f.instruction_count ());
    std::size_t estimate = 0;
    for (auto &b : f.blocks ()) {
      for (auto l : ir::detail::ReferenceAccess::pointers (b.labels ())) {
        if (aligned.contains (l)) estimate += 8;
        offsets.emplace (l, estimate);
      }
      for (auto &i : b.instructions ()) {
        if (i.opcode () == MachineOpcode (MachinePseudoOpcode::Use)) continue;
        prepared.push_back ({&i, {}});
        auto &p = prepared.back ();
        auto normalized = normalize (p);
        if (!(normalized).has_value ()) return normalized.error ();
        auto selected
          = x86_instruction_pattern (i.opcode (), p.operands (), {.allow_short_branches = false});
        if (!selected)
          return invalid ("no encoding for " + std::string (opcode_name (i.opcode ())));
        p.pattern = selected->index;
        estimate += x86_encoding_recipe (p.pattern)->maximum_size;
        if (i.opcode () == MachineOpcode (Opcode::Switch)) {
          if (p.operands ().size () - 1 > std::size_t (INT32_MAX) / 8)
            return invalid ("switch table too large");
          estimate += (p.operands ().size () - 1) * 8;
        }
        if (estimate > INT32_MAX) return invalid ("function exceeds rel32 layout range");
      }
    }
    result.bytes_.reserve (estimate);
    constexpr std::uint8_t nops[8][7]{{},
                                      {0x90},
                                      {0x66, 0x90},
                                      {0x0f, 0x1f, 0xc0},
                                      {0x48, 0x0f, 0x1f, 0xc0},
                                      {0x0f, 0x1f, 0x44, 0, 0},
                                      {0x66, 0x0f, 0x1f, 0x44, 0, 0},
                                      {0x0f, 0x1f, 0x80, 0, 0, 0, 0}};
    std::size_t instruction_index = 0;
    for (auto &b : f.blocks ()) {
      for (auto l : ir::detail::ReferenceAccess::pointers (b.labels ())) {
        if (aligned.contains (l)) {
          auto size = (8 - result.bytes_.size () % 8) % 8;
          for (std::size_t n = 0; n < size; ++n) put (nops[size][n], 1);
        }
        offsets[l] = result.bytes_.size ();
      }
      for (auto &i : b.instructions ()) {
        if (i.opcode () == MachineOpcode (MachinePseudoOpcode::Use)) continue;
        auto &p = prepared[instruction_index++];
        auto pattern = p.pattern;
        auto code = i.opcode ();
        auto op = std::get_if<Opcode> (&code);
        if (op
            && (ir::instruction_descriptor (*op).get ()->flow == ir::ControlFlow::Branch
                || *op == Opcode::Jmp)) {
          auto operands = p.operands ();
          auto l = operands.empty ()
                     ? nullptr
                     : detail::operand_access (operands[0]).get<const MachineLabel *> ();
          if (!l) return invalid ("branch has no label");
          auto selected
            = x86_instruction_pattern (i.opcode (), operands,
                                       {.branch_range
                                        = X86BranchRange{static_cast<std::int64_t> (offsets.at (*l))
                                                           - static_cast<std::int64_t> (
                                                             result.bytes_.size ()),
                                                         0},
                                        .encoding_layout = true});
          if (!selected) return invalid ("branch encoding not found");
          pattern = selected->index;
        }
        auto r = instruction (p, pattern);
        if (!(r).has_value ()) return r.error ();
      }
    }
    for (auto &r : labels) {
      auto at = offsets.find (r.label);
      if (at == offsets.end ()) return invalid ("missing code label");
      if (r.size == 8) {
        set (r.offset, at->second, 8);
        result.relocations_.push_back ({CodeRelocation::Kind::LocalAddress, r.offset});
      } else {
        auto difference
          = static_cast<std::int64_t> (at->second) - static_cast<std::int64_t> (r.next);
        if (r.size == 1 ? difference < -128 || difference > 127
                        : difference < INT32_MIN || difference > INT32_MAX)
          return invalid ("branch displacement out of range");
        set (r.offset, std::bit_cast<std::uint64_t> (difference), r.size);
      }
    }
    auto finished = finish_pool ();
    if (!(finished).has_value ()) return finished.error ();
    for (auto &l : f.labels ())
      result.labels_.push_back ({std::string (l.name ()), offsets.at (&l), l.source_ordinal ()});
    return ArtifactBuilder::finish (std::move (result));
  }
  Result<void> finish_pool () {
    while (result.bytes_.size () % 16) put (0, 1);
    for (auto &r : pool) {
      const auto at = result.bytes_.size ();
      if (at - r.next > INT32_MAX) return invalid ("constant pool too far");
      set (r.offset, at - r.next, 4);
      if (auto n = integer (r.value))
        put (*n, 8);
      else {
        auto status = relocation (r.value, at);
        if (!(status).has_value ()) return status.error ();
        put (0, 8);
      }
      put (0, 8);
      result.constants_.push_back ({at, r.offset, r.next, r.call});
    }
    return {};
  }
  Result<CompiledBlock> run_block (const BBVVersion &version) {
    const auto &plan = *version.specialization ().get ();
    BlockTargets targets (plan);
    std::vector<BlockRelocation> successors;
    for (auto &emission : plan.emissions ()) {
      auto &i = *detail::NodeAccess::instruction (emission);
      Prepared p{&i, {}};
      if (emission.opcode != i.opcode ())
        p.normalized.emplace (i.operands ().begin (), i.operands ().begin () + 1);
      auto normalized = normalize (p);
      if (!(normalized).has_value ()) return normalized.error ();
      // BBV targets live in separately published blocks. Match MIR's forced
      // rel32 selection, never whole-function branch shortening or alignment.
      auto selected
        = x86_instruction_pattern (emission.opcode, p.operands (), {.allow_short_branches = false});
      if (!selected)
        return invalid ("no BBV encoding for " + std::string (opcode_name (emission.opcode)));
      auto start = labels.size ();
      auto status = instruction (p, selected->index);
      if (!(status).has_value ()) return status.error ();
      for (std::size_t n = start; n < labels.size (); ++n) {
        auto &r = labels[n];
        if (r.size != 4 && r.size != 8) return invalid ("short branch in BBV artifact");
        auto target = targets.find (&i, r.operand);
        if (!(target).has_value ()) return target.error ();
        successors.push_back ({r.size == 8 ? BlockRelocation::Kind::AbsoluteAddress
                                           : BlockRelocation::Kind::X86Relative32,
                               r.offset, r.next, (target).value ()});
      }
    }
    for (auto &edge : plan.successors ())
      if (edge.kind == BBVSuccessor::Kind::Fallthrough) {
        put (0xe9, 1);
        auto at = result.bytes_.size ();
        put (0, 4);
        successors.push_back ({BlockRelocation::Kind::X86Relative32, at, result.bytes_.size (),
                               detail::NodeAccess::version (edge)->id ()});
      }
    auto finished = finish_pool ();
    if (!(finished).has_value ()) return finished.error ();
    for (auto label : ir::detail::ReferenceAccess::pointers (version.block ().labels ()))
      result.labels_.push_back ({std::string (label->name ()), 0, label->source_ordinal ()});
    auto code = ArtifactBuilder::finish (std::move (result));
    if (!(code).has_value ()) return code.error ();
    return BlockArtifactBuilder::finish (version.id (), std::move ((code).value ()),
                                         std::move (successors));
  }
};
Result<CompiledFunction> encode_x86 (const MachineFunction &f, const MachineFrameResult &frame,
                                     MachineEncodingOptions options) {
  X86EncodingEngine engine (f, frame, std::move (options));
  return engine.run ();
}
Result<CompiledBlock> encode_x86_block (const BBVPreparation &p, const BBVVersion &version,
                                        MachineEncodingOptions options) {
  X86EncodingEngine engine (p.function (), p.prepared ().frame (), std::move (options));
  return engine.run_block (version);
}
}  // namespace mirnext::codegen::detail
