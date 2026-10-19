/* Licensed under the MIT License. Derived from MIR, Copyright 2018-2024 Vladimir Makarov. */
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"

namespace mirnext::transforms {
namespace {
using namespace ir;
using namespace analysis;
Error invalid(const Function &function, std::string reason) {
  return {ErrorCode::InvalidArgument, std::string(function.symbol().name()) + ": SSA: " + std::move(reason)};
}
PreservedAnalyses preserved_graph() {
  auto preserved = PreservedAnalyses::none();
  preserved.preserve<ControlFlowAnalysis>(); preserved.preserve<FunctionGraphAnalysis>();
  preserved.preserve<DominatorAnalysis>(); preserved.preserve<LoopAnalysis>(); return preserved;
}
Opcode move(Type type) {
  return type.kind() == Type::Kind::F32 ? Opcode::FMov : type.kind() == Type::Kind::F64 ? Opcode::DMov
    : type.kind() == Type::Kind::LongDouble ? Opcode::LDMov : Opcode::Mov;
}
struct SSABuilder {
  Function &function;
  const FunctionGraph &graph;
  SSAForm &form;
  SSARewriter rewriter;
  std::vector<std::unordered_map<const Register *, SSAValue *>> definitions;
  std::unordered_map<const Register *, SSAValue *> entry, undefined;
  std::vector<SSAPhi *> phis;
  std::vector<std::uint64_t> seen;
  std::uint64_t epoch = 0;
  std::size_t transformations = 0;
  bool cache_reads = false;
  SSABuilder(Function &function, const FunctionGraph &graph, SSAForm &form)
    : function(function), graph(graph), form(form), rewriter(form), definitions(graph.blocks.size()), seen(graph.blocks.size()) {}
  Result<SSAValue *> initial(const Register &reg, bool argument) {
    auto &map = argument ? entry : undefined;
    if (auto found = map.find(&reg); found != map.end()) return found->second;
    auto value = rewriter.initial(reg, argument ? SSAValue::Kind::Entry : SSAValue::Kind::Undefined);
    if (!value) return value.error(); map.emplace(&reg, *value); return *value;
  }
  Result<SSAValue *> read(std::size_t block, const Register &reg) {
    if (++epoch == 0) { std::ranges::fill(seen, 0); ++epoch; }
    std::vector<std::size_t> path; Result<SSAValue *> value = invalid(function, "missing reaching definition");
    for (auto b = block;;) {
      if (auto found = definitions[b].find(&reg); found != definitions[b].end()) { value = found->second; break; }
      // The C recursion cannot terminate on a closed single-predecessor cycle.
      // Preserve its uninitialized value semantics using an explicit initial def.
      if (seen[b] == epoch || b == FunctionGraph::entry) { value = initial(reg, false); break; }
      seen[b] = epoch; path.push_back(b);
      const auto &incoming = graph.incoming[b];
      if (incoming.size() == 1) {
        auto pred = graph.edges[incoming[0]].source;
        if (pred == FunctionGraph::entry) { value = initial(reg, true); break; }
        b = pred; continue;
      }
      auto body = graph.blocks[b].ir;
      if (!body) { value = initial(reg, false); break; }
      std::vector<const BasicBlock *> predecessors;
      for (auto id : incoming) predecessors.push_back(graph.blocks[graph.edges[id].source].ir);
      auto phi = rewriter.phi(*body, reg, predecessors); if (!phi) return phi.error();
      phis.push_back(*phi); value = const_cast<SSAValue *>(&(*phi)->output()); definitions[b].emplace(&reg, *value); ++transformations; break;
    }
    if (!value) return value.error();
    // Ordinary definitions are final before filling PHIs. Memoize forwarding
    // paths only then, so later instructions cannot change a cached local def.
    if (cache_reads) for (auto b : path) definitions[b].try_emplace(&reg, *value);
    return *value;
  }
  Result<void> build(bool rename) {
    std::vector<const Register *> hardware;
    for (auto &reg : function.registers()) if (!reg.hardware_binding().empty() && !reg.is_memory_backed()) hardware.push_back(&reg);
    for (auto b : graph.order) if (auto body = graph.blocks[b].ir) for (auto &ordinary : body->instructions()) {
      auto &instruction = const_cast<Instruction &>(ordinary);
      if (instruction.opcode() == Opcode::Ret) for (auto reg : hardware) {
        auto value = read(b, *reg); if (!value) return value.error();
        auto use = rewriter.hardware_input(instruction, *reg, **value); if (!use) return use.error();
      }
      for (std::size_t p = 0; p < instruction.operands().size(); ++p) {
        auto &operand = instruction.operands()[p];
        auto input = [&](const Register *reg, SSAOperandPart part) -> Result<void> {
          if (!reg) return {};
          auto value = read(b, *reg); if (!value) return value.error();
          auto use = rewriter.input(instruction, p, part, **value); if (!use) return use.error(); return {};
        };
        if (auto reg = operand.get<const Register *>(); reg && !ssa_output_operand(function, instruction, p)) {
          auto checked = input(*reg, SSAOperandPart::Register); if (!checked) return checked;
        } else if (auto memory = operand.get<Memory>()) {
          auto checked = input(memory->base, SSAOperandPart::MemoryBase); if (!checked) return checked;
          checked = input(memory->index, SSAOperandPart::MemoryIndex); if (!checked) return checked;
        }
      }
      std::unordered_map<const Register *, SSAValue *> outputs;
      for (std::size_t p = 0; p < instruction.operands().size(); ++p)
        if (auto reg = instruction.operands()[p].get<const Register *>(); reg && ssa_output_operand(function, instruction, p)) {
          auto value = rewriter.definition(instruction, p); if (!value) return value.error(); outputs.try_emplace(*reg, *value);
        }
      for (auto [reg, value] : outputs) definitions[b].insert_or_assign(reg, value);
    }
    cache_reads = true;
    for (std::size_t p = 0; p < phis.size(); ++p) {
      auto phi = phis[p]; auto b = graph.indices.at(&phi->block());
      for (auto id : graph.incoming[b]) {
        auto value = read(graph.edges[id].source, phi->output().origin()); if (!value) return value.error();
        auto use = rewriter.phi_input(*phi, **value); if (!use) return use.error();
      }
    }
    bool changed;
    do {
      changed = false;
      for (auto phi : phis) if (phi->alive()) {
        SSAValue *same = nullptr; bool different = false;
        for (auto use : phi->inputs()) {
          auto value = const_cast<SSAValue *>(&use->value());
          if (value == &phi->output() || value == same) continue;
          if (same) { different = true; break; } same = value;
        }
        if (different) continue;
        if (!same) { auto value = initial(phi->output().origin(), false); if (!value) return value.error(); same = *value; }
        auto checked = rewriter.erase_phi(*phi, *same); if (!checked) return checked; changed = true; ++transformations;
      }
    } while (changed);
    if (!rename) return form.verify();
    std::unordered_map<const Register *, std::size_t> counts;
    for (auto &reg : function.registers()) if (entry.contains(&reg)) ++counts[&reg];
    auto rename_value = [&](SSAValue &value) -> Result<void> {
      if (counts[&value.origin()]++) {
        auto reg = rewriter.fresh_register(value.origin()); if (!reg) return reg.error();
        auto checked = rewriter.rename(value, **reg); if (!checked) return checked; ++transformations;
      }
      return {};
    };
    for (auto &block : function.blocks()) {
      for (auto phi : form.phis(block)) { auto checked = rename_value(const_cast<SSAValue &>(phi->output())); if (!checked) return checked; }
      for (auto &instruction : block.instructions()) for (auto value : form.definitions(instruction)) {
        auto checked = rename_value(*value); if (!checked) return checked;
      }
    }
    auto checked = rewriter.mark_renamed(); if (!checked) return checked; return form.verify();
  }
};
}
Result<PassResult> BuildSSAPass::run(ir::Function &function, analysis::FunctionAnalysisManager &analyses) {
  if (&analyses.function() != &function) return invalid(function, "analysis owner mismatch");
  auto phase = function.require_register_ir(); if (!phase) return phase.error();
  if (function.needs_materialization()) return invalid(function, "stack scopes must be materialized before SSA");
  for (auto &block : function.blocks()) if (block.labels().size() > 1) return invalid(function, "CFG label anchors must be separated before SSA");
  auto graph = analyses.get<FunctionGraphAnalysis>(); if (!graph) return graph.error();
  std::size_t prologues = 0;
  if (std::ranges::any_of((*graph)->blocks, [](auto &b) { return b.kind == FunctionGraph::Kind::Prologue; })) {
    auto materialized = **graph;
    auto created = function.create_block_before(*function.blocks().begin()); if (!created) return created.error(); ++prologues;
    // Replacing a synthetic prologue must retain edge insertion order from
    // earlier transforms: PHI input order follows these edges.
    for (std::size_t b = 0; b < materialized.blocks.size(); ++b) if (materialized.blocks[b].kind == FunctionGraph::Kind::Prologue) {
      materialized.blocks[b] = {FunctionGraph::Kind::Body, *created}; materialized.indices.emplace(*created, b); break;
    }
    auto stored = analyses.set<FunctionGraphAnalysis>(std::move(materialized)); if (!stored) return stored.error();
    graph = analyses.get<FunctionGraphAnalysis>(); if (!graph) return graph.error();
  }
  auto form = SSARewriter::create(function); if (!form) return form.error();
  SSABuilder builder(function, **graph, **form); auto built = builder.build(rename_); if (!built) return built.error();
  auto installed = SSARewriter::install(std::move(*form)); if (!installed) return installed.error();
  return PassResult{builder.transformations + prologues, preserved_graph()};
}
Result<PassResult> ConventionalSSAPass::run(ir::Function &function, analysis::FunctionAnalysisManager &analyses) {
  if (&analyses.function() != &function) return invalid(function, "analysis owner mismatch");
  auto rewriter = SSARewriter::get(function); if (!rewriter) return rewriter.error(); auto form = function.ssa_form();
  if (form->conventional()) return PassResult{0, PreservedAnalyses::all()};
  std::size_t changes = 0;
  for (auto &block : function.blocks()) {
    auto ordered = form->phis(block);
    for (auto phi : ordered) {
      auto &output = const_cast<SSAValue &>(phi->output());
      auto &original = output.reg(); auto edge_reg = rewriter->fresh_register(original); if (!edge_reg) return edge_reg.error();
      for (std::size_t p = 0; p < phi->inputs().size(); ++p) {
        auto predecessor = const_cast<BasicBlock *>(phi->predecessors()[p]);
        if (!predecessor) return invalid(function, "PHI copy requires a materialized predecessor");
        auto &source = const_cast<SSAValue &>(phi->inputs()[p]->value());
        auto tail = predecessor->terminator();
        if (tail && tail->opcode() != Opcode::Jmp && tail->opcode() != Opcode::JmpIndirect && tail->opcode() != Opcode::Switch
            && instruction_descriptor(tail->opcode())->flow != ControlFlow::Branch) tail = nullptr;
        const Operand operands[]{Operand(**edge_reg), Operand(source.reg())}; SSAValue *inputs[]{&source};
        auto inserted = rewriter->insert(*predecessor, tail, move((*edge_reg)->type()), operands, inputs); if (!inserted) return inserted.error();
        auto value = form->definitions(**inserted)[0]; auto checked = rewriter->retarget(*phi->inputs()[p], *value); if (!checked) return checked.error(); ++changes;
      }
      bool local = true;
      for (auto use : output.uses()) if (form->use_block(*use) != &block) { local = false; break; }
      std::vector<SSAUse *> users(output.uses().begin(), output.uses().end());
      auto checked = rewriter->rename(output, **edge_reg); if (!checked) return checked.error();
      if (!local) {
        const Operand operands[]{Operand(original), Operand(**edge_reg)}; SSAValue *inputs[]{&output};
        auto inserted = rewriter->insert(block, phi->before(), move(original.type()), operands, inputs, phi); if (!inserted) return inserted.error();
        auto value = form->definitions(**inserted)[0]; checked = rewriter->replace_uses(users, *value); if (!checked) return checked.error(); ++changes;
      }
    }
  }
  auto checked = rewriter->mark_conventional(); if (!checked) return checked.error();
  return PassResult{changes, preserved_graph()};
}
Result<PassResult> DestroySSAPass::run(ir::Function &function, analysis::FunctionAnalysisManager &analyses) {
  if (&analyses.function() != &function) return invalid(function, "analysis owner mismatch");
  auto rewriter = SSARewriter::get(function); if (!rewriter) return rewriter.error();
  std::size_t phis = 0; for (auto &phi : function.ssa_form()->phis()) phis += phi.alive();
  bool storage = false; for (auto &reg : function.registers()) storage |= reg.is_memory_backed();
  auto checked = rewriter->complete(); if (!checked) return checked.error();
  if (storage) {
    std::vector<const ir::Register *> registers;
    for (auto &block : function.blocks()) for (auto &instruction : block.instructions())
      if (instruction.opcode() == ir::Opcode::Addr || instruction.opcode() == ir::Opcode::Addr8
          || instruction.opcode() == ir::Opcode::Addr16 || instruction.opcode() == ir::Opcode::Addr32)
        registers.push_back(*instruction.operands()[1].get<const ir::Register *>());
    checked = ir::IRRewriter(function).materialize_register_storage(registers); if (!checked) return checked.error();
  }
  return PassResult{phis, preserved_graph()};
}
} // namespace mirnext::transforms
