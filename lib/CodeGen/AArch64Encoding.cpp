/* MIR encoding rules, Copyright (C) 2018-2024 Vladimir Makarov. MIT License. */
#include "mirnext/CodeGen/Detail/CompiledFunction.hpp"
#include "mirnext/CodeGen/Detail/AArch64Patterns.hpp"
#include "mirnext/CodeGen/Detail/Encoding.hpp"
#include "mirnext/CodeGen/Detail/CompiledBlock.hpp"
#include <algorithm>
#include <bit>
#include <array>
namespace mirnext::codegen {
Result<std::uint32_t> aarch64_branch_displacement (std::int64_t difference, bool short_branch) {
  auto bits = short_branch ? 19u : 26u;
  auto limit = std::int64_t{1} << (bits + 1);
  if ((difference & 3) || difference < -limit || difference >= limit)
    return Error{ErrorCode::InvalidOperand, "aarch64 branch displacement out of range"};
  auto value = static_cast<std::uint32_t> (difference / 4) & ((1u << bits) - 1);
  return short_branch ? value << 5 : value;
}
namespace {
using ir::Opcode;
using ir::Type;
std::optional<std::uint64_t> integer (const MachineOperand &o) {
  if (auto v = o.get<std::uint64_t> ()) return *v;
  if (auto v = o.get<std::int64_t> ()) return std::bit_cast<std::uint64_t> (*v);
  return {};
}
unsigned scale (Type t) {
  using K = Type::Kind;
  switch (t.kind ()) {
  case K::I8:
  case K::U8: return 1;
  case K::I16:
  case K::U16: return 2;
  case K::I32:
  case K::U32:
  case K::F32: return 4;
  case K::LongDouble: return 16;
  default: return 8;
  }
}
}  // namespace
namespace detail {
struct EncodingEngine {
  const MachineFunction &f;
  MachineEncodingOptions options;
  CompiledFunctionData result;
  struct LabelFixup {
    const MachineLabel *label;
    std::size_t offset;
    bool absolute, short_branch;
    std::size_t operand = 0;
    bool address = false;
  };
  std::vector<LabelFixup> fixups;
  std::unordered_map<const MachineLabel *, std::size_t> labels;
  struct KnownAddress {
    std::optional<MachineSymbolReference> symbol;
    std::optional<Builtin> builtin;
    std::uint64_t value;
  };
  std::vector<KnownAddress> known;
  EncodingEngine (const MachineFunction &f, const MachineFrameResult &frame,
                  MachineEncodingOptions options)
    : f (f),
      options (std::move (options)),
      result (std::string (f.module_name ()), std::string (f.name ()), f.target ().architecture (),
              frame.layout ()) {}
  Error invalid (std::string why) const {
    return {ErrorCode::InvalidOperand,
            std::string (f.module_name ()) + '/' + std::string (f.name ()) + " ["
              + std::string (f.target ().name ()) + ", encode]: " + std::move (why)};
  }
  void put (std::uint64_t v, unsigned size) {
    for (unsigned n = 0; n < size; ++n, v >>= 8) result.bytes_.push_back (v & 255);
  }
  std::uint32_t word (std::size_t at) const {
    std::uint32_t v = 0;
    for (unsigned n = 0; n < 4; ++n) v |= std::uint32_t (result.bytes_[at + n]) << (8 * n);
    return v;
  }
  void set (std::size_t at, std::uint64_t v, unsigned size) {
    for (unsigned n = 0; n < size; ++n, v >>= 8) result.bytes_[at + n] = v & 255;
  }
  Result<std::uint64_t> resolve (const MachineOperand &o) {
    auto symbol = o.get<MachineSymbolReference> ();
    auto builtin = o.get<BuiltinReference> ();
    if (!symbol && !builtin) return invalid ("not a symbol operand");
    for (auto &v : known)
      if ((symbol ? v.symbol == *symbol : !v.symbol)
          && (builtin ? v.builtin == builtin->builtin : !v.builtin))
        return v.value;
    auto value
      = options.known_addresses ({symbol ? symbol->symbol : nullptr,
                                  builtin ? std::optional (builtin->builtin) : std::nullopt});
    if (!value) {
      auto error = invalid (value.error ().message);
      error.code = value.error ().code;
      return error;
    }
    auto address = *value + (symbol ? std::bit_cast<std::uint64_t> (symbol->displacement) : 0);
    known.push_back ({symbol ? std::optional (*symbol) : std::nullopt,
                      builtin ? std::optional (builtin->builtin) : std::nullopt, address});
    return address;
  }
  Result<void> instruction (MachineOpcode opcode, std::span<const MachineOperand> ops,
                            AArch64Overflow overflow, bool lowered_call = false,
                            std::uint64_t order = 0) {
    std::optional<std::vector<MachineOperand>> normalized;
    auto change = [&] (std::size_t n, MachineOperand o) {
      if (!normalized) normalized.emplace (ops.begin (), ops.end ());
      (*normalized)[n] = std::move (o);
      ops = *normalized;
    };
    if (opcode == MachineOpcode (Opcode::Alloca))
      if (auto size = integer (ops[1])) {
        if (*size > std::uint64_t (INT64_MAX) - 15)
          return invalid ("invalid stack allocation size");
        change (1, MachineOperand ((*size + 15) & ~std::uint64_t{15}));
      }
    if (options.known_addresses)
      for (std::size_t n = 0; n < ops.size (); ++n) {
        if (auto s = ops[n].get<MachineSymbolReference> ();
            s && s->symbol->signature && n == 0 && lowered_call)
          continue;
        if (ops[n].get<MachineSymbolReference> () || ops[n].get<BuiltinReference> ()) {
          auto value = resolve (ops[n]);
          if (!value) return value.error ();
          change (n, MachineOperand (*value));
        }
      }
    auto recipe = aarch64_encoding_recipe (opcode, ops, overflow);
    if (!recipe)
      return invalid ("no target encoding for " + std::string (opcode_name (opcode)) + " #"
                      + std::to_string (order));
    std::optional<std::size_t> switch_start;
    for (auto &w : recipe->words) {
      auto code = w.opcode, mask = w.mask;
      auto at = result.bytes_.size ();
      auto field = [&] (std::uint32_t v, unsigned bits, unsigned shift) -> bool {
        auto bound = (std::uint32_t{1} << bits) - 1;
        auto m = bound << shift;
        if (v > bound || (mask & m)) return false;
        code |= v << shift;
        mask |= m;
        return true;
      };
      auto set_field = [&] (std::uint32_t v, unsigned bits, unsigned shift) -> Result<void> {
        if (!field (v, bits, shift)) return invalid ("invalid encoding field");
        return {};
      };
      for (auto &spec : recipe->fields.subspan (w.first_field, w.field_count)) {
        using K = AArch64FieldKind;
        Result<void> status;
        switch (spec.kind) {
        case K::Register: {
          unsigned r = spec.parameter;
          if (spec.flags != 1) {
            if (r >= ops.size ()) return invalid ("missing register operand");
            auto p = ops[r].get<target::PhysicalRegister> ();
            if (!p) return invalid ("unallocated encoding register");
            r = p->encoding ();
            if (spec.flags == 2) {
              if (r < 32) return invalid ("expected vector register");
              r -= 32;
            }
          }
          status = set_field (r, 5, spec.shift);
          break;
        }
        case K::IndexedMemory:
        case K::DisplacedMemory: {
          auto m = ops[0].get<MachineMemory> ();
          if (!m && ops.size () > 1) m = ops[1].get<MachineMemory> ();
          if (!m || !m->base || !std::holds_alternative<target::PhysicalRegister> (*m->base))
            return invalid ("invalid encoded memory base");
          status = set_field (std::get<target::PhysicalRegister> (*m->base).encoding (), 5, 5);
          if (!status) break;
          if (spec.kind == K::IndexedMemory) {
            unsigned index = 31;
            if (m->index) {
              auto p = std::get_if<target::PhysicalRegister> (&*m->index);
              if (!p) return invalid ("unallocated memory index");
              index = p->encoding ();
            }
            status = set_field (index, 5, 16);
            if (status) status = set_field (m->scale == 1 ? 6 : 7, 4, 12);
          } else {
            auto unit = scale (m->type);
            if (m->displacement < 0 || m->displacement % unit)
              return invalid ("invalid memory displacement");
            status = set_field (static_cast<unsigned> (m->displacement / unit), 12, 10);
          }
          break;
        }
        case K::RightShift:
        case K::LeftShift: {
          auto v = integer (ops[2]);
          auto bits = spec.kind == K::RightShift ? 64u : unsigned (spec.parameter);
          if (!v || *v >= bits) return invalid ("invalid shift immediate");
          status = set_field (spec.kind == K::RightShift ? *v : (bits - *v) & (bits - 1), 6, 16);
          if (status && spec.kind == K::LeftShift) status = set_field (bits - 1 - *v, 6, 10);
          break;
        }
        case K::WideImmediate: {
          auto value = integer (ops[1]);
          if (!value) {
            auto s = ops[1].get<MachineSymbolReference> ();
            auto b = ops[1].get<BuiltinReference> ();
            if ((!s && !b) || spec.flags) return invalid ("invalid symbolic immediate");
            auto shift = unsigned (spec.parameter) * 16;
            result.relocations_.push_back ({CodeRelocation::Kind::AArch64WideMove, at, shift,
                                            s ? std::optional (*s->symbol) : std::nullopt,
                                            b ? std::optional (b->builtin) : std::nullopt,
                                            s ? s->displacement : 0});
            status = set_field (0, 16, 5);
            if (status) status = set_field (spec.parameter, 2, 21);
            break;
          }
          std::array<std::pair<unsigned, unsigned>, 4> chunks{};
          unsigned count = 0;
          auto v = spec.flags ? ~*value : *value;
          if (!v) chunks[count++] = {0, 0};
          for (unsigned n = 0; n < 4; ++n)
            if (auto chunk = (v >> (16 * n)) & 65535) {
              auto immediate = spec.flags && count ? (~chunk & 65535) : chunk;
              chunks[count++] = {static_cast<unsigned> (immediate), n};
            }
          if (spec.parameter >= count) return invalid ("invalid wide immediate index");
          status = set_field (chunks[spec.parameter].first, 16, 5);
          if (status) status = set_field (chunks[spec.parameter].second, 2, 21);
          break;
        }
        case K::ArithmeticImmediate: {
          auto value = integer (ops[spec.flags ? 1 : 2]);
          if (!value) return invalid ("noninteger arithmetic immediate");
          auto v = *value;
          if (spec.flags) {
            if (v > UINT64_MAX - 15) return invalid ("arithmetic roundup overflow");
            v = (v + 15) & ~std::uint64_t{15};
          }
          unsigned shift = v >= 4096;
          if (shift && ((v & 4095) || (v >> 12) >= 4096))
            return invalid ("arithmetic immediate out of range");
          status = set_field (shift ? v >> 12 : v, 12, 10);
          if (status) status = set_field (shift, 2, 22);
          break;
        }
        case K::ImmediateR: status = set_field (spec.parameter, 6, 16); break;
        case K::ImmediateS: status = set_field (spec.parameter, 6, 10); break;
        case K::ShortLabel:
        case K::Label: {
          auto n = opcode == MachineOpcode (Opcode::LAddr) ? 1u : 0u;
          auto l = ops[n].get<const MachineLabel *> ();
          if (!l) return invalid ("nonlabel branch relocation");
          fixups.push_back ({*l, at, false, spec.kind == K::ShortLabel, n,
                             opcode == MachineOpcode (Opcode::LAddr)});
          break;
        }
        case K::SwitchTable:
          if (switch_start) return invalid ("duplicate switch table");
          switch_start = at;
          break;
        }
        if (!status) return status.error ();
      }
      put (code, 4);
    }
    if (switch_start) {
      if (result.bytes_.size () % 8 == 4) put (0, 4);
      auto displacement = (result.bytes_.size () - *switch_start) / 4;
      if (displacement >= (1u << 19)) return invalid ("switch table offset out of range");
      set (*switch_start, word (*switch_start) | static_cast<std::uint32_t> (displacement << 5), 4);
      for (std::size_t n = 1; n < ops.size (); ++n) {
        auto l = ops[n].get<const MachineLabel *> ();
        if (!l) return invalid ("nonlabel switch target");
        fixups.push_back ({*l, result.bytes_.size (), true, false, n});
        put (0, 8);
      }
    }
    return {};
  }
  Result<CompiledFunction> run () {
    AArch64Overflow overflow = AArch64Overflow::Default;
    for (auto &b : f.blocks ()) {
      if (!b.labels ().empty ()) overflow = AArch64Overflow::Default;
      for (auto label : b.labels ()) labels.emplace (label, result.bytes_.size ());
      for (auto &i : b.instructions ()) {
        if (i.opcode () == MachineOpcode (MachinePseudoOpcode::Use)) continue;
        auto r = instruction (i.opcode (), i.operands (), overflow, i.has_lowered_call (),
                              i.creation_order ());
        if (!r) return r.error ();
        auto code = i.opcode ();
        if (code == MachineOpcode (Opcode::Subo) || code == MachineOpcode (Opcode::Subos))
          overflow = AArch64Overflow::Subtract;
        else if (code == MachineOpcode (Opcode::Mulo) || code == MachineOpcode (Opcode::Mulos)
                 || code == MachineOpcode (Opcode::UMulo) || code == MachineOpcode (Opcode::UMulos))
          overflow = AArch64Overflow::Multiply;
        else if (code == MachineOpcode (Opcode::Addo) || code == MachineOpcode (Opcode::Addos))
          overflow = AArch64Overflow::Default;
        else if (auto op = std::get_if<Opcode> (&code);
                 op && ir::instruction_descriptor (*op)->flow == ir::ControlFlow::Branch)
          overflow = AArch64Overflow::Default;
      }
    }
    for (auto &ref : fixups) {
      auto label = labels.find (ref.label);
      if (label == labels.end ()) return invalid ("unknown code label");
      if (ref.absolute) {
        set (ref.offset, label->second, 8);
        result.relocations_.push_back ({CodeRelocation::Kind::LocalAddress, ref.offset});
      } else {
        auto difference
          = static_cast<std::int64_t> (label->second) - static_cast<std::int64_t> (ref.offset);
        if (ref.address) {
          // ADR carries a signed byte offset split across immlo[30:29] and
          // immhi[23:5]. It does not use the conditional-branch word offset.
          if (difference < -(1 << 20) || difference >= (1 << 20))
            return invalid ("label address displacement out of range");
          auto value = static_cast<std::uint32_t> (difference) & 0x1fffff;
          set (ref.offset, word (ref.offset) | ((value & 3) << 29) | ((value >> 2) << 5), 4);
        } else {
          auto value = aarch64_branch_displacement (difference, ref.short_branch);
          if (!value) return invalid (value.error ().message);
          set (ref.offset, word (ref.offset) | *value, 4);
        }
      }
    }
    while (result.bytes_.size () % 16) put (0, 1);
    for (auto &l : f.labels ())
      result.labels_.push_back ({std::string (l.name ()), labels.at (&l), l.source_ordinal ()});
    return ArtifactBuilder::finish (std::move (result));
  }
  Result<CompiledBlock> run_block (const BBVVersion &version) {
    const auto &plan = *version.specialization ();
    BlockTargets targets (plan);
    std::vector<BlockRelocation> successors;
    AArch64Overflow overflow = AArch64Overflow::Default;
    bool conditional = false;
    for (auto &emission : plan.emissions ()) {
      auto &i = *emission.instruction;
      auto ops = i.operands ();
      if (emission.opcode != i.opcode ()) ops = ops.first (1);  // folded property branch
      auto start = fixups.size ();
      auto status
        = instruction (emission.opcode, ops, overflow, i.has_lowered_call (), i.creation_order ());
      if (!status) return status.error ();
      for (std::size_t n = start; n < fixups.size (); ++n) {
        auto &r = fixups[n];
        auto target = targets.find (&i, r.operand);
        if (!target) return target.error ();
        using K = BlockRelocation::Kind;
        successors.push_back ({r.absolute       ? K::AbsoluteAddress
                               : r.address      ? K::AArch64Address21
                               : r.short_branch ? K::AArch64Branch19
                                                : K::AArch64Branch26,
                               r.offset, 0, *target});
      }
      auto code = emission.opcode;
      if (code == MachineOpcode (Opcode::Subo) || code == MachineOpcode (Opcode::Subos))
        overflow = AArch64Overflow::Subtract;
      else if (code == MachineOpcode (Opcode::Mulo) || code == MachineOpcode (Opcode::Mulos)
               || code == MachineOpcode (Opcode::UMulo) || code == MachineOpcode (Opcode::UMulos))
        overflow = AArch64Overflow::Multiply;
      else if (code == MachineOpcode (Opcode::Addo) || code == MachineOpcode (Opcode::Addos))
        overflow = AArch64Overflow::Default;
      else if (auto op = std::get_if<Opcode> (&code);
               op && ir::instruction_descriptor (*op)->flow == ir::ControlFlow::Branch) {
        overflow = AArch64Overflow::Default;
        conditional = true;
      }
    }
    for (auto &edge : plan.successors ())
      if (edge.kind == BBVSuccessor::Kind::Fallthrough) {
        successors.push_back (
          {BlockRelocation::Kind::AArch64Branch26, result.bytes_.size (), 0, edge.version->id ()});
        put (0x14000000, 4);
      }
    if (conditional) put (0xd503201f, 4);  // original far-conditional expansion slot
    while (result.bytes_.size () % 16) put (0, 1);
    for (auto label : version.block ().labels ())
      result.labels_.push_back ({std::string (label->name ()), 0, label->source_ordinal ()});
    auto code = ArtifactBuilder::finish (std::move (result));
    if (!code) return code.error ();
    return BlockArtifactBuilder::finish (version.id (), std::move (*code), std::move (successors));
  }
};
Result<CompiledFunction> encode_aarch64 (const MachineFunction &f, const MachineFrameResult &frame,
                                         MachineEncodingOptions options) {
  EncodingEngine engine (f, frame, std::move (options));
  return engine.run ();
}
Result<CompiledBlock> encode_aarch64_block (const BBVPreparation &p, const BBVVersion &version,
                                            MachineEncodingOptions options) {
  EncodingEngine engine (p.function (), p.prepared ().frame (), std::move (options));
  return engine.run_block (version);
}
}  // namespace detail
}  // namespace mirnext::codegen
