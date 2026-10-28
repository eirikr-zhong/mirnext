/* Licensed under the MIT License. Derived from MIR, Copyright 2018-2024 Vladimir Makarov. */
#include "mirnext/Analysis/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"

namespace mirnext::transforms {
namespace {
using namespace ir;
using namespace analysis;
Error invalid (const Function &function, std::string reason) {
  return {ErrorCode::InvalidArgument,
          std::string (function.symbol ().name ()) + ": SSA: " + std::move (reason)};
}
PreservedAnalyses preserved_graph () {
  auto preserved = PreservedAnalyses::none ();
  preserved.preserve<ControlFlowAnalysis> ();
  preserved.preserve<FunctionGraphAnalysis> ();
  preserved.preserve<DominatorAnalysis> ();
  preserved.preserve<LoopAnalysis> ();
  return preserved;
}
Opcode move (Type type) {
  return type.kind () == Type::Kind::F32          ? Opcode::FMov
         : type.kind () == Type::Kind::F64        ? Opcode::DMov
         : type.kind () == Type::Kind::LongDouble ? Opcode::LDMov
                                                  : Opcode::Mov;
}
struct SSABuilder {
  Function &function;
  const FunctionGraph &graph;
  SSAForm &form;
  SSAUpdater rewriter;
  std::vector<std::unordered_map<const Register *, SSAValue *>> definitions;
  std::unordered_map<const Register *, SSAValue *> entry, undefined;
  std::vector<SSAPhi *> phis;
  std::vector<std::uint64_t> seen;
  std::uint64_t epoch = 0;
  std::size_t transformations = 0;
  bool cache_reads = false;
  SSABuilder (Function &function, const FunctionGraph &graph, SSAForm &form)
    : function (function),
      graph (graph),
      form (form),
      rewriter (form),
      definitions (graph.blocks.size ()),
      seen (graph.blocks.size ()) {}
  Result<SSAValue &> initial (const Register &reg, bool argument) {
    auto &map = argument ? entry : undefined;
    if (auto found = map.find (&reg); found != map.end ()) return *(found->second);
    auto value
      = rewriter.initial (reg, argument ? SSAValue::Kind::Entry : SSAValue::Kind::Undefined);
    if (!(value).has_value ()) return value.error ();
    map.emplace (&reg, &(value).value ());
    return (value).value ();
  }
  Result<SSAValue &> read (std::size_t block, const Register &reg) {
    if (++epoch == 0) {
      std::ranges::fill (seen, 0);
      ++epoch;
    }
    std::vector<std::size_t> path;
    Result<SSAValue &> value = invalid (function, "missing reaching definition");
    for (auto b = block;;) {
      if (auto found = definitions[b].find (&reg); found != definitions[b].end ()) {
        value = *(found->second);
        break;
      }
      // A closed single-predecessor cycle has no reaching definition.
      // Represent its uninitialized value with an explicit initial definition.
      if (seen[b] == epoch || b == FunctionGraph::entry) {
        value = initial (reg, false);
        break;
      }
      seen[b] = epoch;
      path.push_back (b);
      const auto &incoming = graph.incoming[b];
      if (incoming.size () == 1) {
        auto pred = graph.edges[incoming[0]].source;
        if (pred == FunctionGraph::entry) {
          value = initial (reg, true);
          break;
        }
        b = pred;
        continue;
      }
      auto body = analysis::detail::NodeAccess::ir (graph.blocks[b]);
      if (!body) {
        value = initial (reg, false);
        break;
      }
      std::vector<const BasicBlock *> predecessors;
      for (auto id : incoming)
        predecessors.push_back (
          analysis::detail::NodeAccess::ir (graph.blocks[graph.edges[id].source]));
      auto phi = rewriter.phi (*body, reg, predecessors);
      if (!(phi).has_value ()) return phi.error ();
      phis.push_back (&(phi).value ());
      value = *(const_cast<SSAValue *> (&phi.value ().output ()));
      definitions[b].emplace (&reg, &(value).value ());
      ++transformations;
      break;
    }
    if (!(value).has_value ()) return value.error ();
    // Ordinary definitions are final before filling PHIs. Memoize forwarding
    // paths only then, so later instructions cannot change a cached local def.
    if (cache_reads)
      for (auto b : path) definitions[b].try_emplace (&reg, &(value).value ());
    return (value).value ();
  }
  Result<void> build (bool rename) {
    std::vector<const Register *> hardware;
    for (auto &reg : function.registers ())
      if (!reg.hardware_binding ().empty () && !reg.is_memory_backed ()) hardware.push_back (&reg);
    for (auto b : graph.order)
      if (auto body = analysis::detail::NodeAccess::ir (graph.blocks[b]))
        for (auto &ordinary : body->instructions ()) {
          auto &instruction = const_cast<Instruction &> (ordinary);
          if (instruction.opcode () == Opcode::Ret)
            for (auto reg : hardware) {
              auto value = read (b, *reg);
              if (!(value).has_value ()) return value.error ();
              auto use = rewriter.hardware_input (instruction, *reg, (value).value ());
              if (!(use).has_value ()) return use.error ();
            }
          for (std::size_t p = 0; p < instruction.operands ().size (); ++p) {
            auto &operand = instruction.operands ()[p];
            auto input = [&] (const Register *reg, SSAOperandPart part) -> Result<void> {
              if (!reg) return {};
              auto value = read (b, *reg);
              if (!(value).has_value ()) return value.error ();
              auto use = rewriter.input (instruction, p, part, (value).value ());
              if (!(use).has_value ()) return use.error ();
              return {};
            };
            if (auto reg = ir::detail::operand_access (operand).get<const Register *> ();
                reg && !ssa_output_operand (function, instruction, p)) {
              auto checked = input (*reg, SSAOperandPart::Register);
              if (!(checked).has_value ()) return checked;
            } else if (auto memory = ir::detail::operand_access (operand).get<Memory> ()) {
              auto checked
                = input (ir::detail::NodeAccess::base (*(memory)), SSAOperandPart::MemoryBase);
              if (!(checked).has_value ()) return checked;
              checked
                = input (ir::detail::NodeAccess::index (*(memory)), SSAOperandPart::MemoryIndex);
              if (!(checked).has_value ()) return checked;
            }
          }
          std::unordered_map<const Register *, SSAValue *> outputs;
          for (std::size_t p = 0; p < instruction.operands ().size (); ++p)
            if (auto reg
                = ir::detail::operand_access (instruction.operands ()[p]).get<const Register *> ();
                reg && ssa_output_operand (function, instruction, p)) {
              auto value = rewriter.definition (instruction, p);
              if (!(value).has_value ()) return value.error ();
              outputs.try_emplace (*reg, &(value).value ());
            }
          for (auto [reg, value] : outputs) definitions[b].insert_or_assign (reg, value);
        }
    cache_reads = true;
    for (std::size_t p = 0; p < phis.size (); ++p) {
      auto phi = phis[p];
      auto b = analysis::detail::NodeAccess::indices (graph).at (&phi->block ());
      for (auto id : graph.incoming[b]) {
        auto value = read (graph.edges[id].source, phi->output ().origin ());
        if (!(value).has_value ()) return value.error ();
        auto use = rewriter.phi_input (*phi, (value).value ());
        if (!(use).has_value ()) return use.error ();
      }
    }
    bool changed;
    do {
      changed = false;
      for (auto phi : phis)
        if (phi->alive ()) {
          SSAValue *same = nullptr;
          bool different = false;
          for (auto use : ir::detail::ReferenceAccess::pointers (phi->inputs ())) {
            auto value = const_cast<SSAValue *> (&use->value ());
            if (value == &phi->output () || value == same) continue;
            if (same) {
              different = true;
              break;
            }
            same = value;
          }
          if (different) continue;
          if (!same) {
            auto value = initial (phi->output ().origin (), false);
            if (!(value).has_value ()) return value.error ();
            same = &(value).value ();
          }
          auto checked = rewriter.erase_phi (*phi, *same);
          if (!(checked).has_value ()) return checked;
          changed = true;
          ++transformations;
        }
    } while (changed);
    if (!rename) return form.verify ();
    std::unordered_map<const Register *, std::size_t> counts;
    for (auto &reg : function.registers ())
      if (entry.contains (&reg)) ++counts[&reg];
    auto rename_value = [&] (SSAValue &value) -> Result<void> {
      if (counts[&value.origin ()]++) {
        auto reg = rewriter.fresh_register (value.origin ());
        if (!(reg).has_value ()) return reg.error ();
        auto checked = rewriter.rename (value, (reg).value ());
        if (!(checked).has_value ()) return checked;
        ++transformations;
      }
      return {};
    };
    for (auto &block : function.blocks ()) {
      for (auto phi : ir::detail::ReferenceAccess::pointers (form.phis (block))) {
        auto checked = rename_value (const_cast<SSAValue &> (phi->output ()));
        if (!(checked).has_value ()) return checked;
      }
      for (auto &instruction : block.instructions ())
        for (auto value : ir::detail::ReferenceAccess::pointers (form.definitions (instruction))) {
          auto checked = rename_value (*value);
          if (!(checked).has_value ()) return checked;
        }
    }
    auto checked = rewriter.mark_renamed ();
    if (!(checked).has_value ()) return checked;
    return form.verify ();
  }
};
}  // namespace
Result<PassResult> BuildSSAPass::run (ir::MIRBuilder &edit,
                                      analysis::FunctionAnalysisManager &analyses) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  if (&analyses.function () != &function) return invalid (function, "analysis owner mismatch");
  auto phase = function.require_register_ir ();
  if (!(phase).has_value ()) return phase.error ();
  if (function.needs_materialization ())
    return invalid (function, "stack scopes must be materialized before SSA");
  for (auto &block : function.blocks ())
    if (ir::detail::ReferenceAccess::pointers (block.labels ()).size () > 1)
      return invalid (function, "CFG label anchors must be separated before SSA");
  auto graph = analyses.get<FunctionGraphAnalysis> ();
  if (!(graph).has_value ()) return graph.error ();
  std::size_t prologues = 0;
  if (std::ranges::any_of (graph.value ().blocks,
                           [] (auto &b) { return b.kind == FunctionGraph::Kind::Prologue; })) {
    auto materialized = (graph).value ();
    auto created = ir::detail::access (function).create_block_before (*function.blocks ().begin ());
    if (!(created).has_value ()) return created.error ();
    ++prologues;
    // Replacing a synthetic prologue must retain edge insertion order from
    // earlier transforms: PHI input order follows these edges.
    for (std::size_t b = 0; b < materialized.blocks.size (); ++b)
      if (materialized.blocks[b].kind == FunctionGraph::Kind::Prologue) {
        materialized.blocks[b] = {FunctionGraph::Kind::Body, (created).value ()};
        analysis::detail::NodeAccess::indices (materialized).emplace (&(created).value (), b);
        break;
      }
    auto stored = analyses.set<FunctionGraphAnalysis> (std::move (materialized));
    if (!(stored).has_value ()) return stored.error ();
    graph = analyses.get<FunctionGraphAnalysis> ();
    if (!(graph).has_value ()) return graph.error ();
  }
  auto form = SSAUpdater::create (function);
  if (!(form).has_value ()) return form.error ();
  SSABuilder builder (function, (graph).value (), (form).value ());
  auto built = builder.build (rename_);
  if (!(built).has_value ()) return built.error ();
  auto installed = SSAUpdater::install (std::move ((form).value ()));
  if (!(installed).has_value ()) return installed.error ();
  return PassResult{builder.transformations + prologues, preserved_graph ()};
}
Result<PassResult> ConventionalSSAPass::run (ir::MIRBuilder &edit,
                                             analysis::FunctionAnalysisManager &analyses) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  if (&analyses.function () != &function) return invalid (function, "analysis owner mismatch");
  auto rewriter = SSAUpdater::get (function);
  if (!(rewriter).has_value ()) return rewriter.error ();
  auto form = function.ssa_form ().get ();
  if (form->conventional ()) return PassResult{0, PreservedAnalyses::all ()};
  std::size_t changes = 0;
  for (auto &block : function.blocks ()) {
    auto ordered = ir::detail::ReferenceAccess::pointers (form->phis (block));
    for (auto phi : ordered) {
      auto &output = const_cast<SSAValue &> (phi->output ());
      auto &original = output.reg ();
      auto edge_reg = rewriter.value ().fresh_register (original);
      if (!(edge_reg).has_value ()) return edge_reg.error ();
      for (std::size_t p = 0; p < ir::detail::ReferenceAccess::pointers (phi->inputs ()).size ();
           ++p) {
        auto predecessor = const_cast<BasicBlock *> (
          ir::detail::ReferenceAccess::pointers (phi->predecessors ())[p]);
        if (!predecessor) return invalid (function, "PHI copy requires a materialized predecessor");
        auto &source = const_cast<SSAValue &> (
          ir::detail::ReferenceAccess::pointers (phi->inputs ())[p]->value ());
        auto tail = predecessor->terminator ().get ();
        if (tail && tail->opcode () != Opcode::Jmp && tail->opcode () != Opcode::JmpIndirect
            && tail->opcode () != Opcode::Switch
            && instruction_descriptor (tail->opcode ()).get ()->flow != ControlFlow::Branch)
          tail = nullptr;
        const Operand operands[]{Operand ((edge_reg).value ()), Operand (source.reg ())};
        SSAValue *inputs[]{&source};
        auto inserted
          = rewriter.value ().insert (*predecessor, tail, move (edge_reg.value ().type ()),
                                      operands, inputs);
        if (!(inserted).has_value ()) return inserted.error ();
        auto value
          = ir::detail::ReferenceAccess::pointers (form->definitions ((inserted).value ()))[0];
        auto checked
          = rewriter.value ().retarget (*ir::detail::ReferenceAccess::pointers (phi->inputs ())[p],
                                        *value);
        if (!(checked).has_value ()) return checked.error ();
        ++changes;
      }
      bool local = true;
      for (auto use : ir::detail::ReferenceAccess::pointers (output.uses ()))
        if (form->use_block (*use).get () != &block) {
          local = false;
          break;
        }
      std::vector<SSAUse *> users (ir::detail::ReferenceAccess::pointers (output.uses ()).begin (),
                                   ir::detail::ReferenceAccess::pointers (output.uses ()).end ());
      auto checked = rewriter.value ().rename (output, (edge_reg).value ());
      if (!(checked).has_value ()) return checked.error ();
      if (!local) {
        const Operand operands[]{Operand (original), Operand ((edge_reg).value ())};
        SSAValue *inputs[]{&output};
        auto inserted = rewriter.value ().insert (block, phi->before ().get (),
                                                  move (original.type ()), operands, inputs, phi);
        if (!(inserted).has_value ()) return inserted.error ();
        auto value
          = ir::detail::ReferenceAccess::pointers (form->definitions ((inserted).value ()))[0];
        checked = rewriter.value ().replace_uses (users, *value);
        if (!(checked).has_value ()) return checked.error ();
        ++changes;
      }
    }
  }
  auto checked = rewriter.value ().mark_conventional ();
  if (!(checked).has_value ()) return checked.error ();
  return PassResult{changes, preserved_graph ()};
}
Result<PassResult> DestroySSAPass::run (ir::MIRBuilder &edit,
                                        analysis::FunctionAnalysisManager &analyses) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  if (&analyses.function () != &function) return invalid (function, "analysis owner mismatch");
  auto rewriter = SSAUpdater::get (function);
  if (!(rewriter).has_value ()) return rewriter.error ();
  std::size_t phis = 0;
  for (auto &phi : function.ssa_form ().get ()->phis ()) phis += phi.alive ();
  bool storage = false;
  for (auto &reg : function.registers ()) storage |= reg.is_memory_backed ();
  auto checked = rewriter.value ().complete ();
  if (!(checked).has_value ()) return checked.error ();
  if (storage) {
    std::vector<const ir::Register *> registers;
    for (auto &block : function.blocks ())
      for (auto &instruction : block.instructions ())
        if (instruction.opcode () == ir::Opcode::Addr || instruction.opcode () == ir::Opcode::Addr8
            || instruction.opcode () == ir::Opcode::Addr16
            || instruction.opcode () == ir::Opcode::Addr32)
          registers.push_back (
            *ir::detail::operand_access (instruction.operands ()[1]).get<const ir::Register *> ());
    checked = ir::InstructionRewriter (function).materialize_register_storage (registers);
    if (!(checked).has_value ()) return checked.error ();
  }
  return PassResult{phis, preserved_graph ()};
}
}  // namespace mirnext::transforms
