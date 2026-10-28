/* Licensed under the MIT License. Derived from MIR's conventional SSA combine traversal. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
#include "mirnext/Transforms/SSACombine.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/IR/SSA.hpp"
#include <algorithm>
#include <bit>
#include <unordered_set>
namespace mirnext::transforms {
namespace {
using namespace ir;
std::optional<Opcode> combined_branch (Opcode compare, bool truth) {
  switch (compare) {
#define INTEGER(c, yes, no) \
  case Opcode::c: return truth ? Opcode::yes : Opcode::no;
#define FLOAT(c, yes) \
  case Opcode::c: return truth ? std::optional (Opcode::yes) : std::nullopt;
    INTEGER (Eq, Beq, Bne)
    INTEGER (Eqs, Beqs, Bnes)
    INTEGER (Ne, Bne, Beq)
    INTEGER (Nes, Bnes, Beqs)
    INTEGER (Lt, Blt, Bge)
    INTEGER (Lts, Blts, Bges)
    INTEGER (ULt, UBlt, UBge)
    INTEGER (ULts, UBlts, UBges)
    INTEGER (Le, Ble, Bgt)
    INTEGER (Les, Bles, Bgts)
    INTEGER (ULe, UBle, UBgt)
    INTEGER (ULes, UBles, UBgts)
    INTEGER (Gt, Bgt, Ble)
    INTEGER (Gts, Bgts, Bles)
    INTEGER (UGt, UBgt, UBle)
    INTEGER (UGts, UBgts, UBles)
    INTEGER (Ge, Bge, Blt)
    INTEGER (Ges, Bges, Blts)
    INTEGER (UGe, UBge, UBlt)
    INTEGER (UGes, UBges, UBlts)
    FLOAT (FEq, FBeq)
    FLOAT (DEq, DBeq)
    FLOAT (LDEq, LDBeq)
    FLOAT (FNe, FBne)
    FLOAT (DNe, DBne)
    FLOAT (LDNe, LDBne)
    FLOAT (FLt, FBlt)
    FLOAT (DLt, DBlt)
    FLOAT (LDLt, LDBlt)
    FLOAT (FLe, FBle)
    FLOAT (DLe, DBle)
    FLOAT (LDLe, LDBle)
    FLOAT (FGt, FBgt)
    FLOAT (DGt, DBgt)
    FLOAT (LDGt, LDBgt)
    FLOAT (FGe, FBge)
    FLOAT (DGe, DBge)
    FLOAT (LDGe, LDBge)
#undef INTEGER
#undef FLOAT
  default: return {};
  }
}
struct Term {
  Operand operand;
  SSAValue *value;
  const Instruction *origin;
  std::size_t position;
  bool operator== (const Term &other) const {
    return origin == other.origin && position == other.position
           && (!origin ? value == other.value : true);
  }
};
struct Address {
  std::optional<Term> base, index;
  std::uint64_t displacement;
  unsigned scale = 1;
  bool operator== (const Address &) const = default;
};
struct Expansion {
  std::optional<Term> base, index;
  unsigned scale;
  bool stop_base, stop_index;
  bool operator== (const Expansion &) const = default;
};
struct ExpansionHash {
  std::size_t operator() (const Expansion &key) const {
    auto hash = [] (const std::optional<Term> &term) {
      return !term ? std::size_t{0}
                   : std::hash<const Instruction *>{}(term->origin)
                       ^ std::hash<const SSAValue *>{}(term->origin ? nullptr : term->value)
                       ^ (term->position << 4);
    };
    return hash (key.base) ^ (hash (key.index) << 1) ^ (key.scale << 2) ^ (key.stop_base << 12)
           ^ (key.stop_index << 13);
  }
};
class Combiner {
  struct Node {
    Instruction *instruction;
    SSAPhi *phi;
    std::size_t next = SIZE_MAX;
    bool alive = true;
  };
  Function &function;
  SSAForm &form;
  SSAUpdater rewriter;
  target::AddressingRules target;
  std::vector<Node> nodes;
  std::unordered_map<const Instruction *, std::size_t> positions;
  std::size_t changes = 0;
  SSAValue *input (const Instruction &i, std::size_t p,
                   SSAOperandPart part = SSAOperandPart::Register) {
    for (auto use : ir::detail::ReferenceAccess::pointers (form.inputs (i)))
      if (auto site = std::get_if<SSAInstructionUse> (&use->site ());
          site && site->operand == p && site->part == part)
        return const_cast<SSAValue *> (&use->value ());
    return nullptr;
  }
  Term term (const Instruction &i, std::size_t p) {
    return {i.operands ()[p], input (i, p), &i, p};
  }
  std::optional<std::uint64_t> integer (Term t) {
    if (ir::detail::operand_access (t.operand).get<const Register *> ()) {
      auto definition = t.value ? t.value->instruction ().get () : nullptr;
      if (!definition || definition->opcode () != Opcode::Mov) return {};
      t.operand = definition->operands ()[1];
    }
    if (auto value = ir::detail::operand_access (t.operand).get<std::int64_t> ())
      return static_cast<std::uint64_t> (*value);
    if (auto value = ir::detail::operand_access (t.operand).get<std::uint64_t> ()) return *value;
    // No runtime addresses are guessed for unresolved symbol relocations.
    return {};
  }
  bool allowed (const Term &t, const BasicBlock &from) {
    if (!t.value || !t.value->phi ().get () || &t.value->phi ().get ()->block () == &from)
      return true;
    auto phi = t.value->phi ().get ();
    for (auto use : ir::detail::ReferenceAccess::pointers (phi->inputs ())) {
      auto &value = use->value ();
      if ((value.instruction ().get () && &value.instruction ().get ()->block () == &phi->block ())
          || (value.phi ().get () && &value.phi ().get ()->block () == &phi->block ()))
        return false;
    }
    return true;
  }
  bool bound (const Term &t) {
    return !ir::detail::operand_access (t.operand).get<const Register *> ()
           || !(*ir::detail::operand_access (t.operand).get<const Register *> ())
                 ->hardware_binding ()
                 .empty ();
  }
  bool plus_constant (Term &t, const BasicBlock &from, std::uint64_t &constant) {
    if (!t.value || bound (t)) return false;
    constant = 0;
    if (!t.value->instruction ().get () && !t.value->phi ().get () && t.origin) {
      t.origin = nullptr;
      t.position = 0;
      return true;
    }
    auto definition = t.value->instruction ().get ();
    if (!definition) return false;
    auto &i = *definition;
    std::optional<Term> result;
    if (i.opcode () == Opcode::Mov
        && ir::detail::operand_access (i.operands ()[1]).get<const Register *> ())
      result = term (i, 1);
    else if ((i.opcode () == Opcode::Add || i.opcode () == Opcode::Sub)
             && ir::detail::operand_access (i.operands ()[1]).get<const Register *> ()) {
      if (auto value = integer (term (i, 2))) {
        result = term (i, 1);
        constant = i.opcode () == Opcode::Sub ? 0 - *value : *value;
      }
    }
    if (!result && i.opcode () == Opcode::Add
        && ir::detail::operand_access (i.operands ()[2]).get<const Register *> ())
      if (auto value = integer (term (i, 1))) {
        result = term (i, 2);
        constant = *value;
      }
    if (!result || !allowed (*result, from)) return false;
    t = *result;
    return true;
  }
  bool multiply_constant (Term &t, const BasicBlock &from, std::uint64_t &constant) {
    if (!t.value || bound (t)) return false;
    auto definition = t.value->instruction ().get ();
    if (!definition) return false;
    auto &i = *definition;
    std::optional<Term> result;
    if ((i.opcode () == Opcode::Mul || i.opcode () == Opcode::Lsh)
        && ir::detail::operand_access (i.operands ()[1]).get<const Register *> ())
      if (auto value = integer (term (i, 2))) {
        result = term (i, 1);
        constant = *value;
        if (i.opcode () == Opcode::Lsh) {
          if (constant >= 64) return false;
          constant = std::uint64_t{1} << constant;
        }
      }
    if (!result && i.opcode () == Opcode::Mul
        && ir::detail::operand_access (i.operands ()[2]).get<const Register *> ())
      if (auto value = integer (term (i, 1))) {
        result = term (i, 2);
        constant = *value;
      }
    if (!result || constant > 255 || !allowed (*result, from)) return false;
    t = *result;
    return true;
  }
  bool plus_variable (Term &base, std::optional<Term> &index, const BasicBlock &from) {
    if (!base.value || bound (base)) return false;
    auto i = base.value->instruction ().get ();
    if (!i || i->opcode () != Opcode::Add
        || !ir::detail::operand_access (i->operands ()[1]).get<const Register *> ()
        || !ir::detail::operand_access (i->operands ()[2]).get<const Register *> ())
      return false;
    auto a = term (*i, 1), b = term (*i, 2);
    if (!allowed (a, from) || !allowed (b, from)) return false;
    base = a;
    index = b;
    return true;
  }
  bool dead (std::size_t p) {
    auto &node = nodes[p];
    if (!node.alive) return false;
    if (node.phi)
      return ir::detail::ReferenceAccess::pointers (node.phi->output ().uses ()).empty ();
    auto &i = *node.instruction;
    auto op = i.opcode ();
    if (instruction_descriptor (op).get ()->count == OperandCount::Call || op == Opcode::Alloca
        || op == Opcode::BStart || op == Opcode::VaStart || op == Opcode::VaArg)
      return false;
    auto definitions = ir::detail::ReferenceAccess::pointers (form.definitions (i));
    if (definitions.empty ()) return false;
    for (auto value : definitions)
      if (!ir::detail::ReferenceAccess::pointers (value->uses ()).empty ()) return false;
    if (op == Opcode::Addo || op == Opcode::Addos || op == Opcode::Subo || op == Opcode::Subos
        || op == Opcode::Mulo || op == Opcode::Mulos || op == Opcode::UMulo || op == Opcode::UMulos)
      for (auto n = node.next; n != SIZE_MAX; n = nodes[n].next)
        if (nodes[n].alive) {
          if (nodes[n].phi) break;
          auto following = nodes[n].instruction->opcode ();
          if (following == Opcode::Bo || following == Opcode::UBo || following == Opcode::Bno
              || following == Opcode::UBno)
            return false;
          if (following != Opcode::Mov && following != Opcode::Ext32 && following != Opcode::UExt32)
            break;
        }
    return true;
  }
  Result<void> erase (std::size_t p) {
    auto &n = nodes[p];
    auto result = n.phi ? rewriter.erase_phi (*n.phi) : rewriter.erase_instruction (*n.instruction);
    if (!(result).has_value ()) return result;
    n.alive = false;
    ++changes;
    return {};
  }
  Result<void> address (Instruction &i, std::size_t p) {
    auto memory = *ir::detail::operand_access (i.operands ()[p]).get<Memory> ();
    if (!ir::detail::NodeAccess::base (memory)) return {};
    Address a{Term{Operand (*ir::detail::NodeAccess::base (memory)),
                   input (i, p, SSAOperandPart::MemoryBase), &i, p},
              {},
              static_cast<std::uint64_t> (memory.displacement)};
    bool stop_base = false, stop_index = true, changed = false;
    std::unordered_set<Expansion, ExpansionHash> explored;
    for (;;) {
      if (!explored.insert ({a.base, a.index, a.scale, stop_base, stop_index}).second)
        return Error{ErrorCode::InvalidArgument, std::string (function.symbol ().name ())
                                                   + ": ssa-combine: cyclic address definition"};
      auto previous = a;
      auto previous_stop_index = stop_index;
      std::uint64_t constant;
      if (!stop_base && a.base) {
        if (plus_constant (*a.base, i.block (), constant))
          a.displacement += constant;
        else if (a.scale == 1 && multiply_constant (*a.base, i.block (), constant)) {
          if (constant != 1) {
            std::swap (a.base, a.index);
            std::swap (stop_base, stop_index);
            a.scale = constant;
          }
        } else if (!a.index && plus_variable (*a.base, a.index, i.block ()))
          stop_index = false;
      }
      auto legal = [&] {
        return target.accepts (memory.type, std::bit_cast<std::int64_t> (a.displacement),
                               a.index.has_value (), a.scale);
      };
      if (!(a == previous) && legal ()) {
        changed = true;
        continue;
      }
      a = previous;
      stop_index = previous_stop_index;
      stop_base = true;
      if (stop_index || !a.index) break;
      if (plus_constant (*a.index, i.block (), constant))
        a.displacement += constant * a.scale;
      else if (multiply_constant (*a.index, i.block (), constant))
        a.scale = static_cast<std::uint8_t> (a.scale * constant);
      if (!(a == previous) && legal ()) {
        changed = true;
        continue;
      }
      a = previous;
      break;
    }
    if (!changed) return {};
    ir::detail::NodeAccess::base (memory)
      = a.base ? *ir::detail::operand_access (a.base->operand).get<const Register *> () : nullptr;
    ir::detail::NodeAccess::index (memory)
      = a.index ? *ir::detail::operand_access (a.index->operand).get<const Register *> () : nullptr;
    memory.displacement = std::bit_cast<std::int64_t> (a.displacement);
    memory.scale = a.scale;
    auto result
      = rewriter.replace_memory (i, p, std::move (memory), a.base ? a.base->value : nullptr,
                                 a.index ? a.index->value : nullptr);
    if (!(result).has_value ()) return result;
    ++changes;
    return {};
  }

 public:
  Combiner (Function &f, target::AddressingRules rules)
    : function (f),
      form (*const_cast<SSAForm *> (f.ssa_form ().get ())),
      rewriter (form),
      target (rules) {
    nodes.reserve (f.instruction_count () + std::ranges::distance (form.phis ()));
    positions.reserve (f.instruction_count ());
    for (auto &block : f.blocks ()) {
      auto first = nodes.size ();
      std::unordered_map<const Instruction *, std::vector<SSAPhi *>> anchors;
      for (auto phi : ir::detail::ReferenceAccess::pointers (form.phis (block)))
        anchors[phi->before ().get ()].push_back (phi);
      auto phis = [&] (const Instruction *before) {
        if (auto found = anchors.find (before); found != anchors.end ())
          for (auto phi : found->second) nodes.push_back ({nullptr, phi});
      };
      for (auto &i : block.instructions ()) {
        phis (&i);
        positions.emplace (&i, nodes.size ());
        nodes.push_back ({&i, nullptr});
      }
      phis (nullptr);
      for (auto n = first; n + 1 < nodes.size (); ++n) nodes[n].next = n + 1;
    }
  }
  Result<std::size_t> run () {
    // Scan blocks forward and each block backward, including private PHIs.
    for (std::size_t first = 0; first < nodes.size ();) {
      auto end = first + 1;
      while (end < nodes.size () && nodes[end - 1].next != SIZE_MAX) ++end;
      for (auto n = end; n-- > first;) {
        auto &node = nodes[n];
        if (!node.alive) continue;
        if (dead (n)) {
          auto result = erase (n);
          if (!(result).has_value ()) return result.error ();
          continue;
        }
        if (node.phi) continue;
        auto &i = *node.instruction;
        if (instruction_descriptor (i.opcode ()).get ()->count == OperandCount::Call) continue;
        if (i.opcode () == Opcode::Bt || i.opcode () == Opcode::Bf || i.opcode () == Opcode::Bts
            || i.opcode () == Opcode::Bfs) {
          auto condition = input (i, 1);
          auto definition = condition ? condition->instruction ().get () : nullptr;
          if (definition)
            if (auto opcode
                = combined_branch (definition->opcode (),
                                   i.opcode () == Opcode::Bt || i.opcode () == Opcode::Bts)) {
              const Operand operands[]{i.operands ()[0], definition->operands ()[1],
                                       definition->operands ()[2]};
              std::vector<SSAValue *> inputs;
              for (auto p : {1u, 2u})
                if (auto value = input (*definition, p)) inputs.push_back (value);
              auto result = rewriter.replace (i, *opcode, operands, inputs);
              if (!(result).has_value ()) return result.error ();
              ++changes;
              auto at = positions.at (definition);
              if (dead (at)) {
                result = erase (at);
                if (!(result).has_value ()) return result.error ();
              }
            }
        }
        for (std::size_t p = 0; p < i.operands ().size (); ++p)
          if (ir::detail::operand_access (i.operands ()[p]).get<Memory> ()) {
            auto result = address (i, p);
            if (!(result).has_value ()) return result.error ();
          }
      }
      first = end;
    }
    auto checked = function.verify ();
    if (!(checked).has_value ()) return checked.error ();
    return changes;
  }
};
}  // namespace
Result<PassResult> SSACombinePass::run (ir::MIRBuilder &edit,
                                        analysis::FunctionAnalysisManager &analyses) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  auto invalid = [&] (std::string reason) {
    return Error{ErrorCode::InvalidArgument,
                 std::string (function.symbol ().name ()) + ": ssa-combine: " + reason};
  };
  if (&analyses.function () != &function) return invalid ("analysis owner mismatch");
  auto rules = target::AddressingRules::get (architecture_);
  if (!(rules).has_value ()) return invalid (rules.error ().message);
  auto checked = function.verify ();
  if (!(checked).has_value ()) return checked.error ();
  auto form = function.ssa_form ().get ();
  if (!form || !form->conventional ()) return invalid ("requires conventional SSA");
  for (auto &block : function.blocks ())
    for (auto &i : block.instructions ())
      for (auto &operand : i.operands ())
        if (auto memory = ir::detail::operand_access (operand).get<Memory> ();
            memory && ir::detail::NodeAccess::index (*(memory)))
          return invalid ("requires simplified memory addresses");
  auto graph = analyses.get<analysis::FunctionGraphAnalysis> ();
  if (!(graph).has_value ()) return graph.error ();
  auto retained = analyses.take_cached<analysis::FunctionGraphAnalysis> ();
  if (!(retained).has_value ()) return retained.error ();
  auto result = Combiner (function, (rules).value ()).run ();
  if (!(result).has_value ()) return result.error ();
  checked = analyses.set<analysis::FunctionGraphAnalysis> (std::move (*(retained).value ()));
  if (!(checked).has_value ()) return checked.error ();
  auto preserved = analysis::PreservedAnalyses::none ();
  preserved.preserve<analysis::FunctionGraphAnalysis> ();
  return PassResult{(result).value (), preserved};
}
}  // namespace mirnext::transforms
