/* Licensed under the MIT License. Derived from MIR copy_prop and transform_mul_div. */
#include "mirnext/Transforms/AnalysisAccess.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
#include "mirnext/Transforms/CopyPropagation.hpp"
#include "mirnext/Transforms/GVN.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include <bit>
namespace mirnext::transforms {
namespace {
using namespace ir;
bool address (Opcode op) {
  return op == Opcode::Addr || op == Opcode::Addr8 || op == Opcode::Addr16 || op == Opcode::Addr32;
}
bool move (Opcode op) {
  return op == Opcode::Mov || op == Opcode::FMov || op == Opcode::DMov || op == Opcode::LDMov
         || (sizeof (long double) == sizeof (double) && (op == Opcode::D2LD || op == Opcode::LD2D));
}
bool copy (const Instruction &i) {
  return move (i.opcode ())
         && ir::detail::operand_access (i.operands ()[0]).get<const Register *> ()
         && ir::detail::operand_access (i.operands ()[1]).get<const Register *> ();
}
std::pair<unsigned, bool> extension (Opcode op) {
  switch (op) {
  case Opcode::Ext8: return {8, true};
  case Opcode::Ext16: return {16, true};
  case Opcode::Ext32: return {32, true};
  case Opcode::UExt8: return {8, false};
  case Opcode::UExt16: return {16, false};
  case Opcode::UExt32: return {32, false};
  default: return {0, false};
  }
}
bool full_comparison (Opcode op) {
  switch (op) {
#define GROUP(a, b, c, d) \
  case Opcode::a:         \
  case Opcode::b:         \
  case Opcode::c:         \
  case Opcode::d:
    GROUP (Eq, FEq, DEq, LDEq)
    GROUP (Ne, FNe, DNe, LDNe)
    GROUP (Lt, FLt, DLt, LDLt)
  case Opcode::ULt: GROUP (Le, FLe, DLe, LDLe)
  case Opcode::ULe: GROUP (Gt, FGt, DGt, LDGt)
  case Opcode::UGt: GROUP (Ge, FGe, DGe, LDGe)
  case Opcode::UGe: return true;
#undef GROUP
  default: return false;
  }
}
struct State {
  Function &function;
  SSAForm &form;
  SSAUpdater rewriter;
  std::optional<GVNResult> info;
  std::unordered_map<const SSAValue *, std::vector<std::size_t>> bases;
  std::size_t changes = 0;
  State (Function &f, SSAForm &s, std::optional<GVNResult> metadata)
    : function (f), form (s), rewriter (s), info (std::move (metadata)) {
    if (info)
      for (std::size_t p = 1; p < info->memory.size (); ++p)
        if ((info->memory[p].base).has_value ()) bases[info->memory[p].base.get ()].push_back (p);
  }
  SSAValue *input (const Instruction &i, std::size_t p) {
    for (auto use : ir::detail::ReferenceAccess::pointers (form.inputs (i)))
      if (auto site = std::get_if<SSAInstructionUse> (&use->site ()); site && site->operand == p)
        return const_cast<SSAValue *> (&use->value ());
    return nullptr;
  }
  SSAValue *output (const Instruction &i) {
    auto values = ir::detail::ReferenceAccess::pointers (form.definitions (i));
    return values.empty () ? nullptr : values[0];
  }
  std::optional<std::uint64_t> literal (const SSAValue *value) {
    auto i = value ? value->instruction ().get () : nullptr;
    if (!i || i->opcode () != Opcode::Mov) return {};
    if (auto s = ir::detail::operand_access (i->operands ()[1]).get<std::int64_t> ())
      return static_cast<std::uint64_t> (*s);
    if (auto u = ir::detail::operand_access (i->operands ()[1]).get<std::uint64_t> ()) return *u;
    return {};
  }
  void replaced (SSAValue &from, SSAValue &to) {
    if (!info || &from == &to) return;
    if (auto found = bases.find (&from); found != bases.end ()) {
      auto slots = std::move (found->second);
      bases.erase (found);
      for (auto p : slots) info->memory[p].base = to;
      auto &target = bases[&to];
      target.insert (target.end (), slots.begin (), slots.end ());
    }
  }
  Result<void> erase (Instruction &i) {
    if (info) {
      for (auto v : ir::detail::ReferenceAccess::pointers (form.definitions (i)))
        detail::AnalysisAccess::numbers (*(info)).erase (v);
      detail::AnalysisAccess::locations (*(info)).erase (&i);
    }
    auto result = rewriter.erase_instruction (i);
    if (!(result).has_value ()) return result;
    ++changes;
    return {};
  }
  Result<Instruction &> insert (Instruction &before, Opcode op,
                                std::initializer_list<Operand> operands,
                                std::initializer_list<SSAValue *> inputs) {
    auto result
      = rewriter.insert (const_cast<BasicBlock &> (before.block ()), &before, op,
                         {operands.begin (), operands.size ()}, {inputs.begin (), inputs.size ()});
    if (!(result).has_value ()) return result.error ();
    ++changes;
    if (info)
      for (auto v : ir::detail::ReferenceAccess::pointers (form.definitions ((result).value ())))
        detail::AnalysisAccess::numbers (*(info)).emplace (v, GVNResult::Number{});
    return (result).value ();
  }
  Result<const Register &> temp () { return rewriter.fresh_register (Type::i64 ()); }
  Result<void> substitute (Instruction &i, Opcode op, std::initializer_list<Operand> operands,
                           std::initializer_list<SSAValue *> inputs) {
    auto result = rewriter.replace (i, op, {operands.begin (), operands.size ()},
                                    {inputs.begin (), inputs.size ()});
    if (!(result).has_value ()) return result;
    ++changes;
    return {};
  }
  Result<void> propagate (SSAUse &use) {
    for (unsigned n = 0; n < 30; ++n) {
      auto value = &use.value ();
      auto def = value->instruction ().get ();
      if (!def || !copy (*def)) break;
      auto src = input (*def, 1);
      if (&value->reg () == &src->reg ()
          || value->reg ().hardware_binding () != src->reg ().hardware_binding ())
        break;
      auto checked = rewriter.retarget (use, *src);
      if (!(checked).has_value ()) return checked;
      ++changes;
    }
    return {};
  }
  Result<bool> coalesce (Instruction &i, const SSAPhi *previous_phi, const SSAPhi *following_phi) {
    if (!copy (i) || ir::detail::ReferenceAccess::pointers (output (i)->uses ()).empty ())
      return false;
    auto source = input (i, 1);
    auto producer = source->instruction ().get ();
    auto phi = source->phi ().get ();
    bool adjacent = producer
                      ? !previous_phi
                          && InstructionRewriter (function).next_instruction (*producer) == &i
                          && &producer->block () == &i.block ()
                      : phi && phi == previous_phi;
    auto following = InstructionRewriter (function).next_instruction (i);
    if (!adjacent || ir::detail::ReferenceAccess::pointers (source->uses ()).size () != 2
        || (!following && !following_phi))
      return false;
    bool follows = false;
    for (auto use : ir::detail::ReferenceAccess::pointers (source->uses ())) {
      if (auto site = std::get_if<SSAInstructionUse> (&use->site ());
          site && !following_phi && ir::detail::NodeAccess::instruction (*(site)) == following)
        follows = true;
      if (auto site = std::get_if<SSAPhiUse> (&use->site ());
          site && ir::detail::NodeAccess::phi (*(site)) == following_phi)
        follows = true;
    }
    if (!follows) return false;
    auto destination = output (i);
    auto identity = &i;
    auto merged = rewriter.coalesce_copy (i);
    if (!(merged).has_value ()) return merged.error ();
    replaced (*destination, (merged).value ());
    if (info) {
      detail::AnalysisAccess::numbers (*(info)).erase (destination);
      detail::AnalysisAccess::locations (*(info)).erase (identity);
    }
    ++changes;
    return true;
  }
  Result<Instruction &> strength_reduce (Instruction &i) {
    auto op = i.opcode ();
    bool multiply = op == Opcode::Mul || op == Opcode::Muls;
    bool signed_div = op == Opcode::Div || op == Opcode::Divs;
    if (!multiply && !signed_div && op != Opcode::UDiv && op != Opcode::UDivs) return i;
    auto power = [&] (std::size_t p) {
      auto value = literal (input (i, p));
      return value && *value && *value <= INT64_MAX && std::has_single_bit (*value)
               ? static_cast<int> (std::countr_zero (*value))
               : -1;
    };
    auto shift = power (2);
    if (shift < 0 && multiply && (shift = power (1)) >= 0) {
      auto checked = substitute (i, op, {i.operands ()[0], i.operands ()[2], i.operands ()[1]},
                                 {input (i, 2), input (i, 1)});
      if (!(checked).has_value ()) return checked.error ();
    }
    if (shift < 0) return i;
    auto original = output (i), source = input (i, 1);
    Instruction *last = nullptr;
    if (!shift) {
      auto added = insert (i, Opcode::Mov, {i.operands ()[0], Operand (source->reg ())}, {source});
      if (!(added).has_value ()) return added.error ();
      last = &(added).value ();
    } else {
      bool narrow = op == Opcode::Muls || op == Opcode::Divs || op == Opcode::UDivs;
      SSAValue *numerator = source;
      if (signed_div) {
        auto sign_reg = temp (), signed_reg = temp (), mask_reg = temp (), bias_reg = temp (),
             sum_reg = temp ();
        if (!(sign_reg).has_value ()) return sign_reg.error ();
        if (!(signed_reg).has_value ()) return signed_reg.error ();
        if (!(mask_reg).has_value ()) return mask_reg.error ();
        if (!(bias_reg).has_value ()) return bias_reg.error ();
        if (!(sum_reg).has_value ()) return sum_reg.error ();
        auto sign = insert (i, Opcode::Mov,
                            {Operand ((sign_reg).value ()), Operand (narrow ? 31 : 63)}, {});
        if (!(sign).has_value ()) return sign.error ();
        auto sign_value = insert (i, narrow ? Opcode::Rshs : Opcode::Rsh,
                                  {Operand ((signed_reg).value ()), Operand (source->reg ()),
                                   Operand ((sign_reg).value ())},
                                  {source, output ((sign).value ())});
        if (!(sign_value).has_value ()) return sign_value.error ();
        auto mask = insert (i, Opcode::Mov,
                            {Operand ((mask_reg).value ()),
                             Operand (std::bit_cast<std::int64_t> (*literal (input (i, 2)) - 1))},
                            {});
        if (!(mask).has_value ()) return mask.error ();
        auto bias = insert (i, narrow ? Opcode::Ands : Opcode::And,
                            {Operand ((bias_reg).value ()), Operand ((signed_reg).value ()),
                             Operand ((mask_reg).value ())},
                            {output ((sign_value).value ()), output ((mask).value ())});
        if (!(bias).has_value ()) return bias.error ();
        auto sum = insert (i, narrow ? Opcode::Adds : Opcode::Add,
                           {Operand ((sum_reg).value ()), Operand ((bias_reg).value ()),
                            Operand (source->reg ())},
                           {output ((bias).value ()), source});
        if (!(sum).has_value ()) return sum.error ();
        numerator = output ((sum).value ());
      }
      auto shift_reg = temp ();
      if (!(shift_reg).has_value ()) return shift_reg.error ();
      auto literal_insn
        = insert (i, Opcode::Mov, {Operand ((shift_reg).value ()), Operand (shift)}, {});
      if (!(literal_insn).has_value ()) return literal_insn.error ();
      auto code = multiply     ? (narrow ? Opcode::Lshs : Opcode::Lsh)
                  : signed_div ? (narrow ? Opcode::Rshs : Opcode::Rsh)
                               : (narrow ? Opcode::URshs : Opcode::URsh);
      auto added
        = insert (i, code,
                  {i.operands ()[0], Operand (numerator->reg ()), Operand ((shift_reg).value ())},
                  {numerator, output ((literal_insn).value ())});
      if (!(added).has_value ()) return added.error ();
      last = &(added).value ();
    }
    auto checked = rewriter.replace_all_uses (*original, *output (*last));
    if (!(checked).has_value ()) return checked.error ();
    replaced (*original, *output (*last));
    checked = erase (i);
    if (!(checked).has_value ()) return checked.error ();
    return *(last);
  }
  Result<bool> extend (Instruction &i) {
    auto [width, sign] = extension (i.opcode ());
    if (!width) return false;
    auto source = input (i, 1);
    auto def = source ? source->instruction ().get () : nullptr;
    if (!def) return false;
    if (full_comparison (def->opcode ())) {
      auto checked = substitute (i, Opcode::Mov, {i.operands ()[0], i.operands ()[1]}, {source});
      if (!(checked).has_value ()) return checked.error ();
      return true;
    }
    auto [previous_width, previous_sign] = extension (def->opcode ());
    if (previous_width
        && (width <= previous_width || (previous_width < width && (sign || !previous_sign)))) {
      auto original = input (*def, 1);
      auto code = width <= previous_width ? i.opcode () : def->opcode ();
      auto checked
        = substitute (i, code, {i.operands ()[0], Operand (original->reg ())}, {original});
      if (!(checked).has_value ()) return checked.error ();
      return true;
    }
    if (sign || (def->opcode () != Opcode::And && def->opcode () != Opcode::Ands)) return false;
    if (literal (input (*def, 1))) {
      auto checked = substitute (*const_cast<Instruction *> (def), def->opcode (),
                                 {def->operands ()[0], def->operands ()[2], def->operands ()[1]},
                                 {input (*def, 2), input (*def, 1)});
      if (!(checked).has_value ()) return checked.error ();
    }
    auto mask = literal (input (*def, 2));
    if (!mask) return false;
    auto reg = temp ();
    if (!(reg).has_value ()) return reg.error ();
    auto constant
      = insert (i, Opcode::Mov,
                {Operand ((reg).value ()),
                 Operand (std::bit_cast<std::int64_t> (*mask & ((std::uint64_t{1} << width) - 1)))},
                {});
    if (!(constant).has_value ()) return constant.error ();
    auto input_value = input (*def, 1);
    auto added
      = insert (i, Opcode::And,
                {i.operands ()[0], Operand (input_value->reg ()), Operand ((reg).value ())},
                {input_value, output ((constant).value ())});
    if (!(added).has_value ()) return added.error ();
    auto old = output (i);
    auto checked = rewriter.replace_all_uses (*old, *output ((added).value ()));
    if (!(checked).has_value ()) return checked.error ();
    replaced (*old, *output ((added).value ()));
    checked = erase (i);
    if (!(checked).has_value ()) return checked.error ();
    return false;
  }
  Result<void> run () {
    for (auto &block : function.blocks ()) {
      auto phis = ir::detail::ReferenceAccess::pointers (form.phis (block));
      std::size_t p = 0;
      auto process_phis = [&] (const Instruction *before) -> Result<void> {
        while (p < phis.size () && phis[p]->before ().get () == before) {
          for (auto use : ir::detail::ReferenceAccess::pointers (phis[p]->inputs ())) {
            auto checked = propagate (*use);
            if (!(checked).has_value ()) return checked;
          }
          ++p;
        }
        return {};
      };
      auto i
        = block.size () ? const_cast<Instruction *> (&*block.instructions ().begin ()) : nullptr;
      while (i) {
        auto checked = process_phis (i);
        if (!(checked).has_value ()) return checked;
        auto following = InstructionRewriter (function).next_instruction (*i);
        if (!address (i->opcode ())) {
          for (auto use : ir::detail::ReferenceAccess::pointers (form.inputs (*i))) {
            checked = propagate (*use);
            if (!(checked).has_value ()) return checked;
          }
          auto previous_phi = p && phis[p - 1]->before ().get () == i ? phis[p - 1] : nullptr;
          auto following_phi
            = p < phis.size () && phis[p]->before ().get () == following ? phis[p] : nullptr;
          auto merged = coalesce (*i, previous_phi, following_phi);
          if (!(merged).has_value ()) return merged.error ();
          if (!(merged).value ()) {
            auto result = strength_reduce (*i);
            if (!(result).has_value ()) return result.error ();
            i = &(result).value ();
            auto again = extend (*i);
            if (!(again).has_value ()) return again.error ();
            if ((again).value ()) following = i;
          }
        }
        i = following;
      }
      auto checked = process_phis (nullptr);
      if (!(checked).has_value ()) return checked;
    }
    return form.verify ();
  }
};
}  // namespace
Result<PassResult> CopyPropagationPass::run (ir::MIRBuilder &edit,
                                             analysis::FunctionAnalysisManager &analyses) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  auto invalid = [&] (std::string reason) {
    return Error{ErrorCode::InvalidArgument,
                 std::string (function.symbol ().name ()) + ": copy-propagation: " + reason};
  };
  if (&analyses.function () != &function) return invalid ("analysis owner mismatch");
  auto checked = function.verify ();
  if (!(checked).has_value ()) return checked.error ();
  auto form = function.ssa_form ().get ();
  if (!form || !form->renamed () || form->conventional ())
    return invalid ("requires renamed nonconventional SSA");
  auto metadata = analyses.take_cached<GVNAnalysis> ();
  if (!(metadata).has_value ()) return metadata.error ();
  auto graph = analyses.take_cached<analysis::FunctionGraphAnalysis> ();
  if (!(graph).has_value ()) return graph.error ();
  State state (function, *const_cast<ir::SSAForm *> (form), std::move ((metadata).value ()));
  checked = state.run ();
  if (!(checked).has_value ()) return checked.error ();
  auto preserved = analysis::PreservedAnalyses::none ();
  preserved.preserve<analysis::FunctionGraphAnalysis> ();
  if ((graph).value ()) {
    checked = analyses.set<analysis::FunctionGraphAnalysis> (std::move (*(graph).value ()));
    if (!(checked).has_value ()) return checked.error ();
  }
  if (state.info) {
    checked = analyses.set<GVNAnalysis> (std::move (*state.info));
    if (!(checked).has_value ()) return checked.error ();
    preserved.preserve<GVNAnalysis> ();
  }
  return PassResult{state.changes, preserved};
}
}  // namespace mirnext::transforms
