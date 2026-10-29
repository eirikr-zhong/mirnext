/* Licensed under the MIT License. Store-rooted, same-block SIMD128 packing. */
#include "mirnext/Transforms/Vectorization.hpp"
#include "mirnext/Transforms/VectorUtilities.hpp"
#include "mirnext/Target/VectorCost.hpp"
#include "mirnext/IR/Access.hpp"
#include <algorithm>
#include <bit>
#include <unordered_set>
namespace mirnext::transforms {
namespace {
using namespace ir;
using detail::vector_input;
using detail::vector_output;
unsigned width (Type t) {
  switch (t.kind ()) {
  case Type::Kind::I8:
  case Type::Kind::U8: return 1;
  case Type::Kind::I16:
  case Type::Kind::U16: return 2;
  case Type::Kind::I32:
  case Type::Kind::U32:
  case Type::Kind::F32: return 4;
  case Type::Kind::I64:
  case Type::Kind::U64:
  case Type::Kind::F64: return 8;
  default: return 0;
  }
}
Type packed_type (Type t) {
  return Type::vector128 (static_cast<Type::VectorElement> (static_cast<unsigned> (t.kind ()) - 1),
                          16 / width (t));
}
bool move (Opcode op) { return op == Opcode::Mov || op == Opcode::FMov || op == Opcode::DMov; }
unsigned extension_width (Opcode op) {
  switch (op) {
  case Opcode::Ext8:
  case Opcode::UExt8: return 1;
  case Opcode::Ext16:
  case Opcode::UExt16: return 2;
  case Opcode::Ext32:
  case Opcode::UExt32: return 4;
  default: return 0;
  }
}
std::optional<Opcode> vector_operation (Opcode op, Type lane) {
  bool integer = lane.is_integer (), f32 = lane.kind () == Type::Kind::F32,
       f64 = lane.kind () == Type::Kind::F64;
  switch (op) {
#define MAP(i, s, f, d, v)                              \
  case Opcode::i:                                       \
    if (integer) return Opcode::v;                      \
    break;                                              \
  case Opcode::s:                                       \
    if (integer && width (lane) <= 4) return Opcode::v; \
    break;                                              \
  case Opcode::f:                                       \
    if (f32) return Opcode::v;                          \
    break;                                              \
  case Opcode::d:                                       \
    if (f64) return Opcode::v;                          \
    break;
    MAP (Add, Adds, FAdd, DAdd, VAdd)
    MAP (Sub, Subs, FSub, DSub, VSub)
    MAP (Mul, Muls, FMul, DMul, VMul)
    MAP (Neg, Negs, FNeg, DNeg, VNeg)
#undef MAP
  case Opcode::And:
  case Opcode::Ands:
    if (integer && (op == Opcode::And || width (lane) <= 4)) return Opcode::VAnd;
    break;
  case Opcode::Or:
  case Opcode::Ors:
    if (integer && (op == Opcode::Or || width (lane) <= 4)) return Opcode::VOr;
    break;
  case Opcode::Xor:
  case Opcode::Xors:
    if (integer && (op == Opcode::Xor || width (lane) <= 4)) return Opcode::VXor;
    break;
  case Opcode::FDiv:
    if (f32) return Opcode::VDiv;
    break;
  case Opcode::DDiv:
    if (f64) return Opcode::VDiv;
    break;
  default: break;
  }
  return {};
}
std::optional<std::uint64_t> constant_bits (const SSAForm &form, const Instruction &i, Type lane) {
  if (!move (i.opcode ())) return {};
  auto operand = &i.operands ()[1];
  if (auto memory = operand->get<Memory> (); memory.has_value ()) {
    auto provenance = memory.value ().literal_source ().get ();
    auto base = vector_input (form, i, 1, SSAOperandPart::MemoryBase);
    auto definition = base ? base->instruction ().get () : nullptr;
    auto reference = definition && definition->opcode () == Opcode::Mov
                       ? definition->operands ()[1].get<SymbolReference> ().get ()
                       : nullptr;
    auto source = reference ? reference->symbol ().data ().get () : nullptr;
    auto data = source ? std::get_if<InitializedData> (&source->initializer ()) : nullptr;
    // Literal provenance alone is insufficient: the SSA address must still
    // designate this immutable pool entry, with no index or displacement.
    if (!source || (provenance && provenance != source) || source->writable ()
        || !source->is_literal_pool () || !data || data->type != lane || data->values.size () != 1
        || !reference || &reference->symbol () != &source->symbol () || reference->displacement
        || memory.value ().displacement || memory.value ().index ().has_value ())
      return {};
    operand = &data->values[0];
  }
  if (lane.is_integer ()) {
    if (auto v = operand->get<std::int64_t> (); v.has_value ()) return std::uint64_t (v.value ());
    if (auto v = operand->get<std::uint64_t> (); v.has_value ()) return v.value ();
  } else if (lane.kind () == Type::Kind::F32) {
    if (auto v = operand->get<float> (); v.has_value ())
      return std::bit_cast<std::uint32_t> (v.value ());
  } else if (auto v = operand->get<double> (); v.has_value ())
    return std::bit_cast<std::uint64_t> (v.value ());
  return {};
}
struct Region {
  Memory memory;
  SSAValue *base, *index;
  unsigned bytes;
};
std::optional<std::uint64_t> address_constant (const SSAValue *value) {
  auto i = value ? value->instruction ().get () : nullptr;
  if (!i || i->opcode () != Opcode::Mov) return {};
  if (auto s = i->operands ()[1].get<std::int64_t> (); s.has_value ())
    return std::uint64_t (s.value ());
  if (auto u = i->operands ()[1].get<std::uint64_t> (); u.has_value ()) return u.value ();
  return {};
}
Region original_region (const SSAForm &form, const Instruction &i, std::size_t p,
                        unsigned bytes = 0) {
  auto m = i.operands ()[p].get<Memory> ().value ();
  return {m, vector_input (form, i, p, SSAOperandPart::MemoryBase),
          vector_input (form, i, p, SSAOperandPart::MemoryIndex), bytes ? bytes : width (m.type)};
}
Region region (const SSAForm &form, const Instruction &i, std::size_t p, unsigned bytes = 0) {
  auto m = i.operands ()[p].get<Memory> ().value ();
  auto base = vector_input (form, i, p, SSAOperandPart::MemoryBase);
  auto index = vector_input (form, i, p, SSAOperandPart::MemoryIndex);
  // simplify may express displacements as SSA add chains. Recover a bounded
  // affine address without changing IR or treating distinct pointers as disjoint.
  auto displacement = std::uint64_t (m.displacement);
  for (unsigned depth = 0; base && depth < 16; ++depth) {
    auto definition = base->instruction ().get ();
    if (!definition || !base->reg ().hardware_binding ().empty ()) break;
    auto op = definition->opcode ();
    auto left = vector_input (form, *definition, 1);
    if (op == Opcode::Mov && left) {
      base = left;
      continue;
    }
    if (op != Opcode::Add && op != Opcode::Sub) break;
    auto right = vector_input (form, *definition, 2);
    auto constant = address_constant (right);
    if (!constant && op == Opcode::Add) {
      constant = address_constant (left);
      std::swap (left, right);
    }
    if (!constant && op == Opcode::Add && !index && left && right) {
      base = left;
      index = right;
      m.scale = 1;
      continue;
    }
    if (!constant || !left) break;
    displacement += op == Opcode::Sub ? -*constant : *constant;
    base = left;
  }
  for (unsigned depth = 0; index && depth < 16; ++depth) {
    auto definition = index->instruction ().get ();
    if (!definition || !index->reg ().hardware_binding ().empty ()) break;
    auto op = definition->opcode ();
    auto left = vector_input (form, *definition, 1);
    if (op == Opcode::Mov && left) {
      index = left;
      continue;
    }
    if (op != Opcode::Add && op != Opcode::Sub) break;
    auto right = vector_input (form, *definition, 2);
    auto constant = address_constant (right);
    if (!constant && op == Opcode::Add) {
      constant = address_constant (left);
      std::swap (left, right);
    }
    if (!constant || !left) break;
    displacement += (op == Opcode::Sub ? -*constant : *constant) * m.scale;
    index = left;
  }
  if (base && index && m.scale == 1 && std::less<SSAValue *>{}(index, base))
    std::swap (base, index);
  m.displacement = std::bit_cast<std::int64_t> (displacement);
  ir::detail::NodeAccess::base (m) = base ? &base->reg () : nullptr;
  ir::detail::NodeAccess::index (m) = index ? &index->reg () : nullptr;
  if (!index) m.scale = 1;
  return {m, base, index, bytes ? bytes : width (m.type)};
}
bool same_address (const Region &a, const Region &b) {
  return a.base == b.base && a.index == b.index && a.memory.scale == b.memory.scale;
}
bool disjoint (const Region &a, const Region &b) {
  // Match MIR's existing alias/nonalias classes; pointer inequality is not proof.
  if ((!a.memory.alias.empty () && !b.memory.alias.empty () && a.memory.alias != b.memory.alias)
      || (!a.memory.nonalias.empty () && a.memory.nonalias == b.memory.nonalias))
    return true;
  if (!a.bytes || !b.bytes || !same_address (a, b)) return false;
  auto x = std::uint64_t (a.memory.displacement), y = std::uint64_t (b.memory.displacement);
  // Distances in both directions also handle ranges across signed displacement
  // boundaries, without overflowing intermediate signed integers.
  return y - x >= a.bytes && x - y >= b.bytes;
}
bool next_lane (const Region &a, const Region &b, unsigned lane, unsigned bytes) {
  auto distance = std::uint64_t (lane) * bytes;
  return same_address (a, b) && a.memory.type == b.memory.type && a.memory.alias == b.memory.alias
         && a.memory.nonalias == b.memory.nonalias
         && a.memory.displacement <= INT64_MAX - std::int64_t (distance)
         && b.memory.displacement == a.memory.displacement + std::int64_t (distance);
}
struct Pack {
  Opcode opcode = Opcode::VMov;
  std::vector<SSAValue *> lanes;
  std::vector<std::size_t> children;
  std::optional<VectorConstant> constant;
  std::optional<Region> load;
  SSAValue *splat = nullptr;
};
struct Candidate {
  SSAForm &form;
  BasicBlock &block;
  const target::VectorCostModel &cost;
  const VectorizationOptions &options;
  const std::unordered_map<const Instruction *, std::size_t> &positions;
  std::vector<Instruction *> stores;
  Type lane, type;
  std::vector<Pack> packs;
  std::unordered_set<const Instruction *> covered;
  std::size_t anchor, first, last;
  unsigned vector_cost = 1;
  Candidate (SSAForm &s, BasicBlock &b, const target::VectorCostModel &c,
             const VectorizationOptions &o,
             const std::unordered_map<const Instruction *, std::size_t> &p,
             std::vector<Instruction *> roots, Type t)
    : form (s),
      block (b),
      cost (c),
      options (o),
      positions (p),
      stores (std::move (roots)),
      lane (t),
      type (packed_type (t)),
      anchor (p.at (stores.front ())),
      first (anchor),
      last (p.at (stores.back ())) {}
  bool include (const Instruction *i) {
    auto p = positions.find (i);
    if (p == positions.end () || p->second > last
        || (p->second < anchor && anchor - p->second > options.max_store_distance))
      return false;
    first = std::min (first, p->second);
    covered.insert (i);
    return covered.size () <= options.max_pack_nodes;
  }
  bool available (SSAValue *v) const {
    if (!v || v->kind () == SSAValue::Kind::Undefined || !v->reg ().hardware_binding ().empty ())
      return false;
    auto i = v->instruction ().get ();
    return !i || &i->block () != &block || positions.at (i) < anchor;
  }
  std::optional<std::size_t> plan (std::vector<SSAValue *> values, unsigned depth = 0) {
    if (depth > 32 || packs.size () >= options.max_pack_nodes) return {};
    for (auto v : values)
      if (!v || v->kind () == SSAValue::Kind::Undefined || !v->reg ().hardware_binding ().empty ()
          || (lane.is_float () ? v->reg ().type () != lane
                               : (!v->reg ().type ().is_integer ()
                                  || v->reg ().type ().kind () == Type::Kind::Pointer)))
        return {};
    for (std::size_t n = 0; n < packs.size (); ++n)
      if (packs[n].lanes == values) return n;
    Pack pack;
    pack.lanes = values;
    // Shared scalar inputs do not need to be deleted or duplicated.
    if (std::ranges::all_of (values, [&] (auto v) { return v == values.front (); })
        && available (values.front ())) {
      pack.opcode = Opcode::VSplat;
      pack.splat = values.front ();
      vector_cost += cost.splat (type);
    } else {
      std::vector<const Instruction *> defs;
      for (auto v : values) {
        auto i = v->instruction ().get ();
        if (!i || !include (i)) return {};
        defs.push_back (i);
      }
      auto op = defs.front ()->opcode ();
      if (!std::ranges::all_of (defs, [op] (auto i) { return i->opcode () == op; })) return {};
      bool constants = std::ranges::all_of (defs, [&] (auto i) {
        return constant_bits (form, *i, lane).has_value ();
      });
      if (constants) {
        VectorConstant constant{type};
        for (unsigned n = 0; n < defs.size (); ++n) {
          auto bits = *constant_bits (form, *defs[n], lane);
          for (unsigned byte = 0; byte < width (lane); ++byte)
            constant.bytes[n * width (lane) + byte] = std::byte (bits >> (8 * byte));
        }
        pack.constant = constant;
        vector_cost += cost.constant ();
      } else if (move (op) && defs.front ()->operands ()[1].get<Memory> ().has_value ()) {
        auto start = region (form, *defs.front (), 1);
        if (start.memory.type != lane) return {};
        for (unsigned n = 0; n < defs.size (); ++n) {
          if (!defs[n]->operands ()[1].get<Memory> ().has_value ()
              || !next_lane (start, region (form, *defs[n], 1), n, width (lane)))
            return {};
        }
        if (!available (start.base) && start.base) return {};
        if (!available (start.index) && start.index) return {};
        start.memory.type = type;
        start.bytes = 16;
        pack.load = start;
        ++vector_cost;
      } else if (move (op) && defs.front ()->operands ()[1].get<Register> ().has_value ()) {
        std::vector<SSAValue *> inputs;
        for (auto i : defs) inputs.push_back (vector_input (form, *i, 1));
        return plan (std::move (inputs), depth + 1);
      } else if (auto w = extension_width (op); lane.is_integer () && w >= width (lane)) {
        std::vector<SSAValue *> inputs;
        for (auto i : defs) inputs.push_back (vector_input (form, *i, 1));
        return plan (std::move (inputs), depth + 1);
      } else if (move (op)) {
        return {};
      } else {
        auto vector = vector_operation (op, lane);
        auto estimate = vector ? cost.operation (*vector, type) : std::optional<unsigned>{};
        if (!estimate) return {};
        pack.opcode = *vector;
        vector_cost += *estimate;
        for (std::size_t p = 1; p < defs.front ()->operands ().size (); ++p) {
          std::vector<SSAValue *> inputs;
          for (auto i : defs) inputs.push_back (vector_input (form, *i, p));
          auto child = plan (std::move (inputs), depth + 1);
          if (!child) return {};
          pack.children.push_back (*child);
        }
      }
    }
    packs.push_back (std::move (pack));
    return packs.size () - 1;
  }
  bool safe (const std::vector<Instruction *> &instructions) const {
    std::unordered_set<const Instruction *> roots (stores.begin (), stores.end ());
    // Every deleted scalar definition must be local to this pack.
    for (auto i : covered)
      for (auto &value : form.definitions (*i))
        for (auto &use : value.uses ()) {
          auto site = std::get_if<SSAInstructionUse> (&use.site ());
          if (!site
              || (!covered.contains (&site->instruction ())
                  && !roots.contains (&site->instruction ())))
            return false;
        }
    auto destination = region (form, *stores.front (), 0, 16);
    if ((destination.base && !available (destination.base))
        || (destination.index && !available (destination.index)))
      return false;
    for (std::size_t p = first; p <= last; ++p) {
      if (!instructions[p]) return false;
      auto &i = *instructions[p];
      if (roots.contains (&i)) continue;
      for (auto &operand : i.operands ())
        if (auto r = operand.get<Register> ();
            r.has_value () && !r.value ().hardware_binding ().empty ())
          return false;
      if (move (i.opcode ()) && i.operands ()[0].get<Memory> ().has_value ()) {
        auto write = region (form, i, 0);
        if (p >= anchor && !disjoint (destination, write)) return false;
        for (auto &pack : packs)
          if (pack.load) {
            for (auto value : pack.lanes) {
              auto original = positions.at (value->instruction ().get ());
              if (p >= std::min (original, anchor) && p < std::max (original, anchor)
                  && !disjoint (*pack.load, write))
                return false;
            }
          }
        continue;
      }
      if (move (i.opcode ()) && i.operands ()[1].get<Memory> ().has_value ()) {
        // Moving all stores to the first store must not overwrite a later read.
        if (p > anchor && !disjoint (destination, region (form, i, 1))) return false;
        continue;
      }
      if (move (i.opcode ()) || extension_width (i.opcode ())
          || vector_operation (i.opcode (), Type (Type::Kind::I32))
          || vector_operation (i.opcode (), Type (Type::Kind::F32))
          || vector_operation (i.opcode (), Type (Type::Kind::F64)))
        continue;
      // No calls, barriers, branches, allocations, overflow flags, or unknown effects.
      return false;
    }
    // Loads moved to the anchor must also respect earlier packed stores.
    for (auto &pack : packs)
      if (pack.load)
        for (auto value : pack.lanes) {
          auto original = positions.at (value->instruction ().get ());
          if (original > anchor && !disjoint (destination, *pack.load)) return false;
        }
    unsigned scalar_cost = static_cast<unsigned> (stores.size ());
    for (auto i : covered) scalar_cost += cost.scalar (i->opcode ());
    return vector_cost < scalar_cost;
  }
  Result<void> emit (std::size_t root) {
    SSAUpdater edit (form);
    auto before = stores.front ();
    std::vector<SSAValue *> outputs;
    for (auto &pack : packs) {
      auto reg = edit.fresh_register (type);
      if (!reg.has_value ()) return reg.error ();
      std::vector<Operand> operands{Operand (reg.value ())};
      std::vector<SSAValue *> inputs;
      if (pack.load) {
        // Address matching uses affine regions, but emission retains simplify's
        // single-base representation required by the subsequent SSA combine.
        auto source = original_region (form, *pack.lanes.front ()->instruction ().get (), 1, 16);
        source.memory.type = type;
        operands.emplace_back (source.memory);
        if (source.base) inputs.push_back (source.base);
        if (source.index) inputs.push_back (source.index);
      } else if (pack.constant)
        operands.emplace_back (*pack.constant);
      else if (pack.splat) {
        operands.emplace_back (pack.splat->reg ());
        inputs.push_back (pack.splat);
      } else
        for (auto child : pack.children) {
          operands.emplace_back (outputs[child]->reg ());
          inputs.push_back (outputs[child]);
        }
      auto added = edit.insert (block, before, pack.opcode, operands, inputs);
      if (!added.has_value ()) return added.error ();
      outputs.push_back (vector_output (form, added.value ()));
    }
    auto destination = original_region (form, *before, 0, 16);
    destination.memory.type = type;
    std::array operands{Operand (destination.memory), Operand (outputs[root]->reg ())};
    std::vector<SSAValue *> inputs;
    if (destination.base) inputs.push_back (destination.base);
    if (destination.index) inputs.push_back (destination.index);
    inputs.push_back (outputs[root]);
    auto added = edit.insert (block, before, Opcode::VMov, operands, inputs);
    if (!added.has_value ()) return added.error ();
    for (auto i : stores) {
      auto erased = edit.erase_instruction (*i);
      if (!erased.has_value ()) return erased;
    }
    return form.verify ();
  }
};
}  // namespace
Result<PassResult> SLPVectorizePass::run (ir::MIRBuilder &edit,
                                          analysis::FunctionAnalysisManager &analyses) {
  auto checked = detail::vector_form (edit, analyses, name ());
  if (!checked.has_value ()) return checked.error ();
  auto costs = target::VectorCostModel::get (target_);
  if (!costs.has_value ()) return costs.error ();
  auto &form = checked.value ();
  std::size_t changes = 0;
  for (auto &block : edit.function ()->blocks ()) {
    // Snapshot identities. Erased stores are marked null; new vector instructions
    // cannot become candidates again in this invocation.
    std::vector<Instruction *> instructions;
    std::unordered_map<const Instruction *, std::size_t> positions;
    for (auto &i : block.instructions ()) {
      positions.emplace (&i, instructions.size ());
      instructions.push_back (&i);
    }
    std::size_t packed = 0;
    for (std::size_t p = 0; p < instructions.size () && packed < options_.max_block_packs; ++p) {
      auto i = instructions[p];
      if (!i || !move (i->opcode ()) || !i->operands ()[0].get<Memory> ().has_value ()) continue;
      auto start = region (form, *i, 0);
      auto bytes = width (start.memory.type);
      if (!bytes) continue;
      std::vector<Instruction *> stores{i};
      for (std::size_t q = p + 1; q < instructions.size () && q - p <= options_.max_store_distance
                                  && stores.size () < 16 / bytes;
           ++q) {
        auto next = instructions[q];
        if (!next) break;
        if (move (next->opcode ()) && next->operands ()[0].get<Memory> ().has_value ()) {
          if (!next_lane (start, region (form, *next, 0), static_cast<unsigned> (stores.size ()),
                          bytes))
            break;
          stores.push_back (next);
        }
      }
      if (stores.size () != 16 / bytes) continue;
      Candidate candidate (form, block, costs.value (), options_, positions, stores,
                           start.memory.type);
      std::vector<SSAValue *> values;
      for (auto store : stores) values.push_back (vector_input (form, *store, 1));
      auto root = candidate.plan (std::move (values));
      if (!root || !candidate.safe (instructions)) continue;
      auto result = candidate.emit (*root);
      if (!result.has_value ()) return result.error ();
      for (auto store : stores) instructions[positions.at (store)] = nullptr;
      ++packed;
      ++changes;
    }
  }
  return PassResult{changes, detail::vector_preserved (changes)};
}
}  // namespace mirnext::transforms
