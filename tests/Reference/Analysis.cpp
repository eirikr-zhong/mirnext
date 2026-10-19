/* Licensed under the MIT License. Only this independent adapter reads original C analysis state. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "COracle.h"
#include "mir-gen.h"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "../Analysis/Fixtures.hpp"
#include "../IR/PublicInstructions.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
namespace {
struct Snapshot {
  std::vector<std::vector<std::string>> instructions;
  std::vector<std::vector<std::size_t>> successors;
  std::map<std::pair<unsigned, std::size_t>, std::vector<std::size_t>> facts;
  std::string str() const {
    std::ostringstream out;
    for (std::size_t b = 0; b < instructions.size(); ++b) {
      out << "block " << b << ':'; for (auto &i : instructions[b]) out << '[' << i << ']'; out << " edges:";
      for (auto d : successors[b]) out << ' ' << d; out << '\n';
    }
    for (auto &[key, values] : facts) {
      out << key.first << '/' << key.second << ':';
      auto ordered = values;
      if (key.first >= 2 && key.first <= 7) std::ranges::sort(ordered);
      for (auto value : ordered) out << ' ' << value; out << '\n';
    }
    return out.str();
  }
};
Snapshot native(Function &function, FunctionAnalysisManager &manager) {
  auto graph = manager.get<FunctionGraphAnalysis>(); REQUIRE(graph); auto &g = **graph;
  auto dom = manager.get<DominatorAnalysis>(); REQUIRE(dom); auto live = manager.get<LivenessAnalysis>(); REQUIRE(live);
  auto loops = manager.get<LoopAnalysis>(); REQUIRE(loops); Snapshot result;
  result.instructions.resize(g.blocks.size()); result.successors.resize(g.blocks.size());
  std::map<const Register *, std::size_t> regs;
  auto reg = [&](const Register *r) { if (!r) return std::size_t{0}; auto [i, added] = regs.emplace(r, regs.size() + 1); return i->second; };
  for (auto argument : function.arguments()) reg(argument);
  for (std::size_t b = 0; b < g.blocks.size(); ++b) {
    if (auto block = g.blocks[b].ir) for (auto &i : block->instructions()) {
      if (i.opcode() == Opcode::Ret) {
        std::string use = "use";
        for (auto &r : function.registers()) if (!r.hardware_binding().empty()) use += " r" + std::to_string(reg(&r));
        if (use != "use") result.instructions[b].push_back(use);
      }
      auto shape = std::string(instruction_descriptor(i.opcode())->name);
      if (sizeof(long double) == sizeof(double)) {
        if (shape == "ld2d" || shape == "d2ld") shape = "dmov";
        else if (auto pos = shape.find("ld"); pos != shape.npos) shape.replace(pos, 2, "d");
      }
      for (auto &o : i.operands()) {
        if (auto r = o.get<const Register *>()) shape += " r" + std::to_string(reg(*r));
        else if (auto m = o.get<Memory>()) { auto base = reg(m->base), index = reg(m->index); shape += " m" + std::to_string(base) + '/' + std::to_string(index); }
      }
      result.instructions[b].push_back(shape);
    }
    for (auto id : g.outgoing[b]) result.successors[b].push_back(g.edges[id].destination);
  }
  for (std::size_t b = 0; b < g.blocks.size(); ++b) {
    result.facts[{0, b}].push_back(g.preorder[b]); result.facts[{1, b}].push_back(g.rpost[b]);
    for (std::size_t a = 0; a < g.blocks.size(); ++a) if ((*dom)->dominates(a, b)) {
      result.facts[{3, b}].push_back(a);
      if (a != b || !g.rpost[b]) result.facts[{2, b}].push_back(a);
    }
    const SparseBitSet *sets[] = {&(*live)->in[b], &(*live)->out[b], &(*live)->generated[b], &(*live)->killed[b]};
    for (unsigned k = 0; k < 4; ++k) sets[k]->for_each([&](auto id) { result.facts[{k + 4, b}].push_back(reg((*live)->registers[id])); });
  }
  for (std::size_t n = 0; n < (*loops)->nodes.size(); ++n) {
    auto &node = (*loops)->nodes[n]; result.facts[{8, n}].push_back(node.block);
    result.facts[{9, n}].push_back(node.entry); result.facts[{10, n}].push_back(node.parent);
    for (auto child : node.children) result.facts[{11, n}].push_back(child);
  }
  return result;
}
struct CReference {
  MIR_context_t context;
  MIR_item_t item;
  Snapshot result;
  std::map<MIR_reg_t, std::size_t> regs;
  std::size_t reg(MIR_reg_t r) { if (!r) return 0; auto [i, added] = regs.emplace(r, regs.size() + 1); return i->second; }
  static void cfg(std::size_t b, MIR_insn_t insn, std::size_t target, void *data) {
    auto &self = *static_cast<CReference *>(data);
    if (!insn && target == SIZE_MAX) { self.result.instructions.emplace_back(); self.result.successors.emplace_back(); return; }
    if (!insn) { self.result.successors[b].push_back(target); return; }
    if (insn->code == MIR_LABEL) return;
    auto shape = std::string(MIR_insn_name(self.context, insn->code));
    for (std::size_t n = 0; n < insn->nops; ++n) {
      auto &o = insn->ops[n];
      if (o.mode == MIR_OP_VAR) shape += " r" + std::to_string(self.reg(_MIR_opt_test_var_register(o.u.var)));
      else if (o.mode == MIR_OP_VAR_MEM) {
        auto base = self.reg(_MIR_opt_test_var_register(o.u.var_mem.base)), index = self.reg(_MIR_opt_test_var_register(o.u.var_mem.index));
        shape += " m" + std::to_string(base) + '/' + std::to_string(index);
      }
    }
    self.result.instructions[b].push_back(shape);
  }
  static void fact(unsigned kind, std::size_t b, std::size_t value, void *data) {
    auto &self = *static_cast<CReference *>(data);
    if (kind >= 4 && kind <= 7) value = self.reg(value);
    self.result.facts[{kind, b}].push_back(value);
  }
  static void observe(MIR_context_t context, MIR_item_t item, void *data) {
    _MIR_opt_test_visit_cfg(context, item, cfg, data); _MIR_opt_test_visit_analysis(context, fact, data);
  }
};
void compare(std::string_view text, unsigned level) {
  auto source = serialization::parse_text(text); INFO((source ? "" : source.error().message)); REQUIRE(source);
  auto encoded = serialization::write_text(**source); REQUIRE(encoded);
  auto context = MIR_init(); MIR_scan_string(context, encoded->c_str()); MIR_gen_init(context);
  auto module = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(context)); REQUIRE(module); _MIR_opt_test_simplify_module(context, module);
  transforms::FunctionPassManager passes; passes.add<transforms::SimplifyPass>(); passes.add<transforms::PrepareCFGPass>(level != 0);
  auto status = passes.run(**source); INFO((status ? "" : status.error().message)); REQUIRE(status);
  for (auto &function : (*source)->modules().begin()->functions()) {
    MIR_item_t item = NULL;
    for (auto i = DLIST_HEAD(MIR_item_t, module->items); i; i = DLIST_NEXT(MIR_item_t, i))
      if (i->item_type == MIR_func_item && std::string_view(MIR_item_name(context, i)) == function.symbol().name()) item = i;
    REQUIRE(item); CReference reference{context, item};
    for (std::size_t n = 0; n < item->u.func->nargs; ++n)
      reference.reg(MIR_reg(context, VARR_GET(MIR_var_t, item->u.func->vars, n).name, item->u.func));
    REQUIRE(_MIR_opt_test_analyze_function(context, item, level, CReference::observe, &reference));
    FunctionAnalysisManager manager(function); auto actual = native(function, manager).str(), expected = reference.result.str();
    INFO("input:\n", *encoded, "\nC:\n", expected, "\nC++:\n", actual); CHECK(actual == expected);
  }
  MIR_gen_finish(context); MIR_finish(context);
}
}
TEST_CASE("native graph dominance liveness and loops match isolated C analysis stages") {
  for (auto &text : test::analysis_fixtures()) for (unsigned level : {0, 1, 2, 3}) { CAPTURE(level); compare(text, level); }
}
TEST_CASE("MIR optimization regression inputs retain original analysis results") {
  for (unsigned n = 1; n <= 16; ++n) {
    CAPTURE(n); std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"mir-tests"/("test"+std::to_string(n)+".mir")); REQUIRE(input);
    std::string text((std::istreambuf_iterator<char>(input)), {}); compare(text, 0); compare(text, 2);
  }
}
TEST_CASE("fixed seed valid CFGs match original analyses at every optimization level") {
  unsigned sample = 0;
  for (auto &text : test::random_analysis_fixtures()) {
    CAPTURE(sample); for (unsigned level : {0, 1, 2, 3}) { CAPTURE(level); compare(text, level); } ++sample;
  }
}
TEST_CASE("label difference data preserves both otherwise unreachable roots") {
  auto text = test::analysis_fixtures()[6]; auto position = text.find("endmodule");
  text.insert(position, "difference: lref dead1, dead2, 7\n");
  for (unsigned level : {0, 1, 2, 3}) { CAPTURE(level); compare(text, level); }
}
TEST_CASE("all public opcodes retain original pre-SSA analysis decisions") {
  for (auto &descriptor : instruction_descriptors()) {
    CAPTURE(descriptor.name); auto context = test::public_instruction_example(descriptor);
    if (descriptor.opcode == Opcode::VaStart || descriptor.opcode == Opcode::VaEnd || descriptor.opcode == Opcode::VaArg || descriptor.opcode == Opcode::VaBlockArg)
      for (auto &f : context->modules().begin()->functions()) for (auto &b : f.blocks()) for (auto &i : b.instructions())
        for (std::size_t n = 0; n < i.operands().size(); ++n) if (auto m = i.operands()[n].get<Memory>(); m && m->type.kind() == Type::Kind::Undef) {
          auto memory = *m; memory.displacement = 128; REQUIRE(IRRewriter(f).replace_operand(i, n, Operand(memory)));
        }
    auto text = serialization::write_text(*context); REQUIRE(text); compare(*text, 0); compare(*text, 2);
  }
}
