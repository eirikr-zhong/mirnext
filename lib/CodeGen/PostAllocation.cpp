#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/PostState.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/IR/Access.hpp"
/* Post-allocation MIR algorithms, Copyright (C) 2018-2024 Vladimir Makarov.
   Licensed under the MIT License. Transform block-owned machine instructions. */
#include "mirnext/CodeGen/PostAllocation.hpp"
#include "mirnext/CodeGen/Detail/InstructionPatterns.hpp"
#include "mirnext/CodeGen/Detail/RegisterEffects.hpp"
#include <algorithm>
namespace mirnext::codegen {
std::string_view stage_name (MachinePostStage stage) {
  switch (stage) {
  case MachinePostStage::StripProperties: return "strip-properties";
  case MachinePostStage::Liveness: return "live-after-allocation";
  case MachinePostStage::Combine: return "post-allocation-combine";
  case MachinePostStage::DeadCode: return "post-allocation-dce";
  }
  return "post-allocation";
}
namespace {
using ir::Opcode;
Error invalid (const MachineFunction &f, std::string why,
               std::string_view stage = "post-allocation") {
  return {ErrorCode::InvalidOperand, std::string (f.module_name ()) + "/" + std::string (f.name ())
                                       + " [" + std::string (f.target ().name ()) + ", "
                                       + std::string (stage) + "]: " + std::move (why)};
}
bool is (MachineOpcode c, Opcode op) { return c == MachineOpcode (op); }
bool move (MachineOpcode c) {
  return is (c, Opcode::Mov) || is (c, Opcode::FMov) || is (c, Opcode::DMov)
         || is (c, Opcode::LDMov);
}
bool property (MachineOpcode c) {
  return is (c, Opcode::PrSet) || is (c, Opcode::PrBeq) || is (c, Opcode::PrBne);
}
std::pair<int, bool> extension (MachineOpcode c) {
  auto p = std::get_if<Opcode> (&c);
  if (!p) return {};
  switch (*p) {
  case Opcode::Ext8: return {8, true};
  case Opcode::UExt8: return {8, false};
  case Opcode::Ext16: return {16, true};
  case Opcode::UExt16: return {16, false};
  case Opcode::Ext32: return {32, true};
  case Opcode::UExt32: return {32, false};
  default: return {};
  }
}
bool overflow (MachineOpcode c) {
  auto p = std::get_if<Opcode> (&c);
  if (!p) return false;
  switch (*p) {
  case Opcode::Addo:
  case Opcode::Addos:
  case Opcode::Subo:
  case Opcode::Subos:
  case Opcode::Mulo:
  case Opcode::Mulos:
  case Opcode::UMulo:
  case Opcode::UMulos: return true;
  default: return false;
  }
}
bool overflow_branch (MachineOpcode c) {
  return is (c, Opcode::Bo) || is (c, Opcode::Bno) || is (c, Opcode::UBo) || is (c, Opcode::UBno);
}
bool observable (MachineOpcode c) {
  return detail::is_call (c) || is (c, Opcode::Ret) || is (c, Opcode::JRet)
         || is (c, Opcode::Alloca) || is (c, Opcode::BStart) || is (c, Opcode::BEnd)
         || is (c, Opcode::VaStart) || is (c, Opcode::VaArg) || is (c, Opcode::VaEnd);
}
std::optional<target::PhysicalRegister> physical (const MachineOperand &o) {
  if (auto r = detail::operand_access (o).get<target::PhysicalRegister> ()) return *r;
  return {};
}
std::optional<Opcode> commutative (Opcode insn_code) {
  switch (insn_code) {
    /* we can not change fp comparison and branches because NaNs */
  case Opcode::Add:
  case Opcode::Adds:
  case Opcode::FAdd:
  case Opcode::DAdd:
  case Opcode::LDAdd:
  case Opcode::Mul:
  case Opcode::Muls:
  case Opcode::Mulo:
  case Opcode::Mulos:
  case Opcode::UMulo:
  case Opcode::UMulos:
  case Opcode::FMul:
  case Opcode::DMul:
  case Opcode::LDMul:
  case Opcode::And:
  case Opcode::Or:
  case Opcode::Xor:
  case Opcode::Ands:
  case Opcode::Ors:
  case Opcode::Xors:
  case Opcode::Eq:
  case Opcode::Eqs:
  case Opcode::FEq:
  case Opcode::DEq:
  case Opcode::LDEq:
  case Opcode::Ne:
  case Opcode::Nes:
  case Opcode::FNe:
  case Opcode::DNe:
  case Opcode::LDNe:
  case Opcode::Beq:
  case Opcode::Beqs:
  case Opcode::FBeq:
  case Opcode::DBeq:
  case Opcode::LDBeq:
  case Opcode::Bne:
  case Opcode::Bnes:
  case Opcode::FBne:
  case Opcode::DBne:
  case Opcode::LDBne: return insn_code; break;
  case Opcode::Lt: return Opcode::Gt;
  case Opcode::Lts: return Opcode::Gts;
  case Opcode::ULt: return Opcode::UGt;
  case Opcode::ULts: return Opcode::UGts;
  case Opcode::Le: return Opcode::Ge;
  case Opcode::Les: return Opcode::Ges;
  case Opcode::ULe: return Opcode::UGe;
  case Opcode::ULes: return Opcode::UGes;
  case Opcode::Gt: return Opcode::Lt;
  case Opcode::Gts: return Opcode::Lts;
  case Opcode::UGt: return Opcode::ULt;
  case Opcode::UGts: return Opcode::ULts;
  case Opcode::Ge: return Opcode::Le;
  case Opcode::Ges: return Opcode::Les;
  case Opcode::UGe: return Opcode::ULe;
  case Opcode::UGes: return Opcode::ULes;
  case Opcode::Blt: return Opcode::Bgt;
  case Opcode::Blts: return Opcode::Bgts;
  case Opcode::UBlt: return Opcode::UBgt;
  case Opcode::UBlts: return Opcode::UBgts;
  case Opcode::Ble: return Opcode::Bge;
  case Opcode::Bles: return Opcode::Bges;
  case Opcode::UBle: return Opcode::UBge;
  case Opcode::UBles: return Opcode::UBges;
  case Opcode::Bgt: return Opcode::Blt;
  case Opcode::Bgts: return Opcode::Blts;
  case Opcode::UBgt: return Opcode::UBlt;
  case Opcode::UBgts: return Opcode::UBlts;
  case Opcode::Bge: return Opcode::Ble;
  case Opcode::Bges: return Opcode::Bles;
  case Opcode::UBge: return Opcode::UBle;
  case Opcode::UBges: return Opcode::UBles;
  default: return {};
  }
}

}  // namespace
namespace detail {
struct PostAllocationEngine {
  MachineFunction &f;
  MachineRewriter rw;
  MachinePostOptions options;
  MachinePostResult result;
  MachineGraphAnalysis input_graph;
  std::vector<MachineBlockLiveness> boundary;
  std::unordered_map<const MachineInstruction *, std::vector<MachineRegister>> deaths;
  struct Reference {
    MachineInstruction *instruction = nullptr;
    std::size_t number = 0;
    bool definition = false, deleted = false;
  };
  std::vector<Reference> refs;
  std::unordered_map<const MachineVirtualRegister *, std::size_t> virtual_indices;
  std::size_t memory_number = 0;
  std::optional<Error> error;
  PostAllocationEngine (MachineFunction &f, MachineSpillResult spill, MachineGraphAnalysis graph,
                        MachinePostOptions options)
    : f (f),
      rw (f),
      options (std::move (options)),
      result (std::move (spill)),
      input_graph (std::move (graph)),
      refs (f.target ().registers ().size ()) {}
  std::size_t live_index (MachineRegister r) {
    if (auto p = std::get_if<target::PhysicalRegister> (detail::NodeAccess::storage (&r)))
      return p->encoding ();
    // Most functions have only physical effects at this stage. Allocate this
    // map only when copied-block dependencies actually survive allocation.
    if (virtual_indices.empty ()) {
      auto n = f.target ().registers ().size ();
      for (auto &v : f.registers ()) virtual_indices.emplace (&v, n++);
    }
    return virtual_indices.at (
      std::get<const MachineVirtualRegister *> (detail::NodeAccess::storage (r)));
  }
  auto &statistics () { return result.statistics_; }
  template <class T>
  bool checked (const Result<T> &r) {
    if (!r.has_value () && !error) error = invalid (f, r.error ().message);
    return r.has_value ();
  }
  Result<void> observe (MachinePostStage stage) {
    if (!options.observer) return {};
    MachinePostView view (f, input_graph.cfg (), boundary, deaths, statistics ());
    auto observed = options.observer (stage, view);
    if (!(observed).has_value ()) {
      auto e = invalid (f, observed.error ().message, stage_name (stage));
      e.code = observed.error ().code;
      return e;
    }
    return {};
  }
  MachineInstruction *next (MachineInstruction *i) {
    auto r = rw.next (*i);
    if (!checked (r)) return nullptr;
    return r.value ().get ();
  }
  void erase (MachineInstruction *i) {
    deaths.erase (i);
    checked (rw.erase (*i));
  }
  void replace (MachineInstruction *i, MachineOpcode code, std::vector<MachineOperand> ops) {
    checked (rw.replace (*i, code, std::move (ops)));
  }
  void properties (MachineInstruction *i) {
    ++statistics ().properties;
    auto value = i->operands ().size () > 2
                   ? detail::operand_access (i->operands ()[2]).get<std::int64_t> ()
                   : nullptr;
    if (is (i->opcode (), Opcode::PrSet)
        || (is (i->opcode (), Opcode::PrBeq) && (!value || *value != 0))
        || (is (i->opcode (), Opcode::PrBne) && (!value || *value == 0)))
      erase (i);
    else
      replace (i, Opcode::Jmp, {i->operands ()[0]});
  }
  bool safe (target::PhysicalRegister r, MachineInstruction *i) {
    auto &ref = refs[r.encoding ()];
    auto &dead = deaths[i];
    return ref.instruction && ref.definition
           && std::find (dead.begin (), dead.end (), detail::NodeAccess::make_MachineRegister (r))
                != dead.end ();
  }
  bool obsolete (target::PhysicalRegister r, std::size_t number) {
    auto &ref = refs[r.encoding ()];
    return ref.instruction && ref.number > number;
  }
  bool obsolete (const MachineOperand &o, std::size_t number) {
    if (auto r = physical (o)) return obsolete (*r, number);
    auto m = detail::operand_access (o).get<MachineMemory> ();
    if (!m) return false;
    auto changed = [&] (auto r) {
      return r
             && obsolete (std::get<target::PhysicalRegister> (detail::NodeAccess::storage (*r)),
                          number);
    };
    return changed (m->base) || changed (m->index) || memory_number > number;
  }
  MachineInstruction *definition (target::PhysicalRegister r) {
    auto &ref = refs[r.encoding ()];
    if (!ref.definition) return nullptr;
    if (ref.deleted) {
      if (!error) error = invalid (f, "deleted definition required by death metadata");
      return nullptr;
    }
    auto i = ref.instruction;
    auto ops = i->operands ();
    if ((ops.size () > 1 && obsolete (ops[1], ref.number))
        || (ops.size () > 2 && obsolete (ops[2], ref.number)))
      return nullptr;
    return i;
  }
  void erase_definition (MachineInstruction *def, MachineInstruction *at) {
    auto r = *physical (def->operands ()[0]);
    auto &ref = refs[r.encoding ()];
    if (!ref.instruction || ref.deleted) return;
    auto &dead = deaths[at];
    std::erase (dead, detail::NodeAccess::make_MachineRegister (r));
    auto inputs = machine_register_effects (f, *at);
    if (!checked (inputs)) return;
    for (auto value : deaths[def])
      if (std::find (inputs.value ().uses.begin (), inputs.value ().uses.end (), value)
          != inputs.value ().uses.end ())
        dead.push_back (value);
    erase (def);
    ref.deleted = true;
    ++statistics ().deleted;
  }
  bool extensions (MachineInstruction *i) {
    auto [width, sign] = extension (i->opcode ());
    if (!width) return false;
    auto r = physical (i->operands ()[1]);
    if (!r || !safe (*r, i)) return false;
    auto &ref = refs[r->encoding ()];
    if (ref.deleted) {
      error = invalid (f, "deleted extension definition");
      return false;
    }
    auto def = ref.instruction;
    auto [inner, inner_sign] = extension (def->opcode ());
    if (!inner || obsolete (def->operands ()[1], ref.number)
        || (width > inner && !sign && inner_sign))
      return false;
    auto ops = std::vector<MachineOperand> (i->operands ().begin (), i->operands ().end ());
    ops[1] = def->operands ()[1];
    auto code = inner < width ? def->opcode () : i->opcode ();
    if (!instruction_pattern_matches (f.target (), code, ops)) return false;
    replace (i, code, std::move (ops));
    erase_definition (def, i);
    ++statistics ().extensions;
    return true;
  }
  bool substitute (MachineInstruction *&i, MachineOpcode candidate_code,
                   std::vector<MachineOperand> ops) {
    if (ops.empty ()) return false;
    auto saved_ops = ops;
    std::vector<target::PhysicalRegister> vars;
    SparseBitSet seen;
    // Candidate swaps never alter output positions for the supported commutative codes.
    for (std::size_t p = 0; p < ops.size (); ++p) {
      if (operand_is_output (*i, p) || detail::operand_access (ops[p]).get<MachineMemory> ())
        continue;
      if (auto r = physical (ops[p]); r && safe (*r, i) && !seen.contains (r->encoding ())) {
        seen.insert (r->encoding ());
        vars.push_back (*r);
      }
    }
    if (move (candidate_code) && physical (ops[1]) && !vars.empty ()
        && vars.back () == *physical (ops[1])) {
      auto r = *physical (ops[1]);
      auto def = definition (r);
      if (!def || is_call (def->opcode ())) return false;
      auto early = early_clobbers (f.target (), def->opcode ());
      auto m = detail::operand_access (ops[0]).get<MachineMemory> ();
      if (!move (def->opcode ()) && !early[0] && !early[1]
          && (!m
              || (m->base != detail::NodeAccess::make_MachineRegister (r)
                  && m->index != detail::NodeAccess::make_MachineRegister (r)))) {
        auto changed
          = std::vector<MachineOperand> (def->operands ().begin (), def->operands ().end ());
        changed[0] = ops[0];
        if (!instruction_pattern_matches (f.target (), def->opcode (), changed)) return false;
        checked (rw.move_before (*def, *i));
        replace (def, def->opcode (), std::move (changed));
        erase (i);
        i = def;
        ++statistics ().deleted;
        ++statistics ().moves;
        return true;
      }
    }
    bool changed = false, success = true;
    while (!vars.empty ()) {
      auto r = vars.back ();
      vars.pop_back ();
      auto def = definition (r);
      if (!def || !move (def->opcode ())) continue;
      std::vector<std::size_t> changed_ops;
      for (std::size_t p = 0; p < ops.size (); ++p) {
        auto &o = ops[p];
        bool change = false;
        if (!operand_is_output (*i, p) && physical (o) == r) {
          o = def->operands ()[1];
          change = true;
        } else if (auto m = detail::operand_access (o).get<MachineMemory> ();
                   m
                   && (m->base == detail::NodeAccess::make_MachineRegister (r)
                       || m->index == detail::NodeAccess::make_MachineRegister (r))) {
          auto src = physical (def->operands ()[1]);
          if (!src)
            success = false;
          else {
            auto value = *m;
            if (value.base == detail::NodeAccess::make_MachineRegister (r)) value.base = *src;
            if (value.index == detail::NodeAccess::make_MachineRegister (r)) value.index = *src;
            o = detail::NodeAccess::make_MachineOperand (std::move (value));
            change = true;
          }
        }
        if (change) changed_ops.push_back (p);
      }
      if (changed_ops.empty ()) continue;
      if (success) success = instruction_pattern_matches (f.target (), candidate_code, ops);
      for (auto p : changed_ops) {
        if (success)
          saved_ops[p] = ops[p];
        else
          ops[p] = saved_ops[p];
      }
      if (success) {
        replace (i, candidate_code, ops);
        changed = true;
        erase_definition (def, i);
        ++statistics ().substitutions;
      }
      // A failed match remains failed for the rest of this candidate list.
    }
    return changed;
  }
  void reference (target::PhysicalRegister r, MachineInstruction *i, std::size_t n, bool def) {
    refs[r.encoding ()] = {i, n, def, false};
  }
  void combine () {
    // Incoming ABI copies precede the generator's synthetic entry anchor in
    // flat MIR, within the same allocation block. MIR combine removes an
    // anchor in that position before scanning the remaining body.
    std::vector<MachineLabel *> internal_entries;
    for (auto &label : f.labels ())
      if (label.is_function_entry ()) {
        for (auto &i : label.block ().instructions ())
          if (i.is_entry_setup ()) {
            internal_entries.push_back (const_cast<MachineLabel *> (&label));
            break;
          }
      }
    for (auto label : internal_entries) checked (rw.erase_label (*label));
    if (error) return;
    for (auto &block : f.blocks ()) {
      bool changed;
      do {
        ++statistics ().scans;
        changed = false;
        std::fill (refs.begin (), refs.end (), Reference{});
        memory_number = 0;
        std::size_t number = ir::detail::ReferenceAccess::pointers (block.labels ()).size () + 1;
        auto body = block.instructions ();
        for (auto i = body.begin () == body.end () ? nullptr : &*body.begin (); i;) {
          auto following = next (i);
          if (property (i->opcode ())) {
            if (options.remove_properties) properties (i);
            i = following;
            ++number;
            continue;
          }
          for (auto r : early_clobbers (f.target (), i->opcode ()))
            if (r) reference (*r, i, number, true);
          auto original = i->opcode ();
          if (is_call (original)) {
            for (auto &r : f.target ().registers ())
              if (f.target ().call_clobbers (r.reg, ir::Type (ir::Type::Kind::Undef)))
                reference (r.reg, i, number, true);
            memory_number = number;
          } else if (is (original, Opcode::VaBlockArg))
            memory_number = number;
          else if (!is (original, Opcode::Ret)) {
            if (extensions (i))
              changed = true;
            else {
              auto ops
                = std::vector<MachineOperand> (i->operands ().begin (), i->operands ().end ());
              bool replaced = substitute (i, i->opcode (), ops);
              auto opcode = std::get_if<Opcode> (&original);
              auto commute = opcode ? commutative (*opcode) : std::nullopt;
              if (!replaced && commute) {
                std::swap (ops[1], ops[2]);
                if (substitute (i, *commute, std::move (ops))) {
                  replaced = true;
                  ++statistics ().swaps;
                }
              }
              changed |= replaced;
              if (is (original, Opcode::BStart) || is (original, Opcode::BEnd))
                memory_number = number;
            }
          }
          if (error) return;
          for (unsigned pass = 0; pass < 2; ++pass)
            for (std::size_t p = 0; p < i->operands ().size (); ++p) {
              auto &o = i->operands ()[p];
              bool out = operand_is_output (*i, p);
              if (auto r = physical (o); r && (pass != 0) == out)
                reference (*r, i, number, pass != 0);
              else if (auto m = detail::operand_access (o).get<MachineMemory> ()) {
                if (out && pass == 1)
                  memory_number = number;
                else if (pass == 0) {
                  if (m->base)
                    reference (std::get<target::PhysicalRegister> (
                                 detail::NodeAccess::storage (*m->base)),
                               i, number, false);
                  if (m->index)
                    reference (std::get<target::PhysicalRegister> (
                                 detail::NodeAccess::storage (*m->index)),
                               i, number, false);
                }
              }
            }
          i = following;
          ++number;
        }
      } while (changed);
    }
  }
  void dce () {
    for (std::size_t b = 2; b < detail::NodeAccess::blocks (input_graph.cfg ()).size (); ++b) {
      auto &block
        = *const_cast<MachineBasicBlock *> (detail::NodeAccess::blocks (input_graph.cfg ())[b]);
      auto live = boundary[b].out;
      auto body = block.instructions ();
      bool reachable_overflow = false;
      for (auto at = body.end (); at != body.begin ();) {
        auto &i = *--at;
        auto previous = at;
        bool first = at == body.begin ();
        if (!first) --previous;
        bool reg_def = false, dead = true;
        visit_registers (i, true, [&] (auto reg) {
          auto r = live_index (reg);
          reg_def = true;
          if (live.contains (r)) dead = false;
          live.erase (r);
        });
        auto first_reg = i.operands ().empty () ? std::nullopt : physical (i.operands ()[0]);
        if (reg_def && dead && !observable (i.opcode ())
            && !(overflow (i.opcode ()) && reachable_overflow)
            && !(first_reg
                 && (f.target ().architecture () == target::Architecture::X86_64
                       ? first_reg->encoding () == 5 || first_reg->encoding () == 4
                       : first_reg->encoding () == 29 || first_reg->encoding () == 31))) {
          if (first && std::next (at) == body.end ()
              && ir::detail::ReferenceAccess::pointers (block.labels ()).empty ()) {
            checked (rw.insert_after (i, MachinePseudoOpcode::Use, {}));
            ++statistics ().no_op_anchors;
            reachable_overflow = false;
          }
          erase (&i);
          ++statistics ().dead_instructions;
          if (first) break;
          at = std::next (previous);
          continue;
        }
        if (is_call (i.opcode ())) {
          for (auto &r : f.target ().registers ())
            if (f.target ().call_clobbers (r.reg, ir::Type (ir::Type::Kind::Undef)))
              live.erase (r.reg.encoding ());
          for (auto r : result.allocation ().global_registers ()) live.insert (r.encoding ());
        }
        visit_registers (i, false, [&] (auto reg) { live.insert (live_index (reg)); });
        for (auto r : early_clobbers (f.target (), i.opcode ()))
          if (r) live.erase (r->encoding ());
        if (is_call (i.opcode ()))
          for (auto r : i.call_arguments ()) live.insert (r.encoding ());
        // The reverse frontier tracks the forward MOV/EXT32/UEXT32 search after each
        // deletion, without rescanning the same surviving suffix.
        if (overflow_branch (i.opcode ()))
          reachable_overflow = true;
        else if (!is (i.opcode (), Opcode::Mov) && !is (i.opcode (), Opcode::Ext32)
                 && !is (i.opcode (), Opcode::UExt32))
          reachable_overflow = false;
      }
    }
  }
  std::optional<MachinePostStage> next_stage () const {
    auto level = result.allocation ().optimization_level ();
    if (next_index == 0 && level < 2 && options.remove_properties)
      return MachinePostStage::StripProperties;
    if (next_index <= 1 && level) return MachinePostStage::Liveness;
    if (next_index == 2 && level) return MachinePostStage::Combine;
    if (next_index == 3 && level) return MachinePostStage::DeadCode;
    return {};
  }
  unsigned next_index = 0;
  bool finished = false;
  Result<void> step () {
    if (error) return *error;
    if (finished || !next_stage ()) return invalid (f, "post-allocation stages already finished");
    auto stage = *next_stage ();
    auto level = result.allocation ().optimization_level ();
    switch (stage) {
    case MachinePostStage::StripProperties:
      for (auto &b : f.blocks ()) {
        auto body = b.instructions ();
        for (auto i = body.begin () == body.end () ? nullptr : &*body.begin (); i;) {
          auto following = next (i);
          if (property (i->opcode ())) properties (i);
          i = following;
        }
      }
      if (error) return *error;
      break;
    case MachinePostStage::Liveness: {
      auto live
        = MachineLiveness::analyze_retained_topology (f, input_graph,
                                                      {.optimization_level = level,
                                                       .calculate_frequencies = false,
                                                       .global_registers
                                                       = result.allocation ().global_registers ()});
      if (!(live).has_value ()) return live.error ();
      boundary.assign (live.value ().blocks ().begin (), live.value ().blocks ().end ());
      for (auto &b : f.blocks ())
        for (auto &i : b.instructions ()) {
          auto dead = live.value ().deaths (i);
          if (!(dead).has_value ()) return dead.error ();
          if (!dead.value ().empty ())
            deaths.emplace (&i, std::vector<MachineRegister> (dead.value ().begin (),
                                                              dead.value ().end ()));
        }
      break;
    }
    case MachinePostStage::Combine: combine (); break;
    case MachinePostStage::DeadCode: dce (); break;
    }
    if (error) return *error;
    auto observed = observe (stage);
    if (!(observed).has_value ()) {
      error = observed.error ();
      return *error;
    }
    next_index = static_cast<unsigned> (stage) + 1;
    return {};
  }
  Result<MachinePostResult> finish () {
    if (error) return *error;
    if (finished) return invalid (f, "post-allocation result already consumed");
    while (next_stage ()) {
      auto checked = step ();
      if (!(checked).has_value ()) return checked.error ();
    }
    finished = true;
    auto state = f.state ();
    state.post_allocation_optimized = true;
    auto committed = rw.set_state (state);
    if (!(committed).has_value ()) return committed.error ();
    auto cfg = MachineControlFlow::build (f);
    if (!(cfg).has_value ()) return cfg.error ();
    result.spill_.cfg_ = std::move ((cfg).value ());
    result.spill_.rewrite_.allocation_.revision_ = f.revision ();
    return std::move (result);
  }
};
}  // namespace detail
namespace detail {
PostAllocationState::PostAllocationState (std::unique_ptr<PostAllocationEngine> state)
  : state_ (std::move (state)) {}
PostAllocationState::PostAllocationState (PostAllocationState &&) noexcept = default;
PostAllocationState &PostAllocationState::operator= (PostAllocationState &&) noexcept = default;
PostAllocationState::~PostAllocationState () = default;
Result<PostAllocationState> PostAllocationState::create (MachineFunction &f,
                                                         MachineSpillResult spill,
                                                         MachinePostOptions options) {
  if (f.target ().architecture () != target::Architecture::AArch64
      && f.target ().architecture () != target::Architecture::X86_64)
    return invalid (f, "unsupported target");
  if (!spill.current (f) || !f.state ().edge_spills_inserted
      || f.state ().post_allocation_optimized)
    return invalid (f, "stale/foreign input or invalid phase");
  auto graph = MachineGraphAnalysis::build (f, spill.cfg ());
  if (!(graph).has_value ()) return graph.error ();
  return PostAllocationState (std::make_unique<PostAllocationEngine> (f, std::move (spill),
                                                                      std::move ((graph).value ()),
                                                                      std::move (options)));
}
std::optional<MachinePostStage> PostAllocationState::next_stage () const {
  return state_ ? state_->next_stage () : std::nullopt;
}
Result<void> PostAllocationState::step () {
  if (!state_) return Error{ErrorCode::InvalidArgument, "moved-from post-allocation state"};
  return state_->step ();
}
Result<MachinePostResult> PostAllocationState::finish () {
  if (!state_) return Error{ErrorCode::InvalidArgument, "moved-from post-allocation state"};
  return state_->finish ();
}
}  // namespace detail
Result<MachinePostResult> optimize_allocated_machine (MachineFunction &f, MachineSpillResult spill,
                                                      MachinePostOptions options) {
  auto state = detail::PostAllocationState::create (f, std::move (spill), std::move (options));
  if (!(state).has_value ()) return state.error ();
  return state.value ().finish ();
}
}  // namespace mirnext::codegen
