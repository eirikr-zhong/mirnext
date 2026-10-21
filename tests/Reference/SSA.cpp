/* Licensed under the MIT License. Independent C SSA stage oracle. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "COracle.h"
#include "mir-gen.h"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Transforms/Address.hpp"
#include "mirnext/Transforms/BlockCloning.hpp"
#include "mirnext/Transforms/GVN.hpp"
#include "mirnext/Transforms/CopyPropagation.hpp"
#include "mirnext/Transforms/DeadStores.hpp"
#include "mirnext/Transforms/DeadCode.hpp"
#include "mirnext/Transforms/Motion.hpp"
#include "mirnext/Transforms/SSACombine.hpp"
#include "mirnext/Transforms/JumpOptimization.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Target/X86ABI.hpp"
#include "../Execution/NativeTarget.hpp"
#include "mirnext/Execution/Interpreter.hpp"
#include "../Analysis/Fixtures.hpp"
#include "../IR/PublicInstructions.hpp"
#include "../Transforms/AddressFixtures.hpp"
#include "../Transforms/CloningFixtures.hpp"
#include "../Transforms/GVNFixtures.hpp"
#include "../Transforms/MemoryFixtures.hpp"
#include "../Transforms/MotionFixtures.hpp"
#include "../Transforms/CombineFixtures.hpp"
#include "../Transforms/JumpFixtures.hpp"
#include "../Transforms/InlineFixtures.hpp"
#include <bit>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
namespace {
int type_code(Type type, bool reg = false) {
  if (reg && (type.is_integer() || type.is_block())) return MIR_T_I64;
  switch (type.kind()) {
  case Type::Kind::Bool: case Type::Kind::I64: return MIR_T_I64; case Type::Kind::U64: return MIR_T_U64;
  case Type::Kind::I8: return MIR_T_I8; case Type::Kind::U8: return MIR_T_U8;
  case Type::Kind::I16: return MIR_T_I16; case Type::Kind::U16: return MIR_T_U16;
  case Type::Kind::I32: return MIR_T_I32; case Type::Kind::U32: return MIR_T_U32;
  case Type::Kind::F32: return MIR_T_F; case Type::Kind::F64: return MIR_T_D;
  case Type::Kind::LongDouble: return sizeof(long double) == sizeof(double) ? MIR_T_D : MIR_T_LD;
  case Type::Kind::Pointer: return MIR_T_P; case Type::Kind::Undef: return MIR_T_UNDEF;
  case Type::Kind::Block: return MIR_T_BLK + static_cast<int>(type.block_class()); case Type::Kind::ReturnBlock: return MIR_T_RBLK;
  }
  return -1;
}
template<class T> std::string bits(T value) {
  std::ostringstream out; out << std::hex << std::setfill('0');
  if constexpr (std::is_integral_v<T>) out << std::setw(sizeof(T) * 2) << static_cast<std::make_unsigned_t<T>>(value);
  else if constexpr (sizeof(T) == 4) out << std::setw(8) << std::bit_cast<std::uint32_t>(value);
  else if constexpr (sizeof(T) == 8) out << std::setw(16) << std::bit_cast<std::uint64_t>(value);
  else {
    auto bytes = reinterpret_cast<const unsigned char *>(&value);
    auto count = sizeof(T) == 16 && sizeof(long double) == 16 ? 10u : sizeof(T);
    for (std::size_t n = 0; n < count; ++n) out << std::setw(2) << static_cast<unsigned>(bytes[n]);
  }
  return out.str();
}
std::string target_opcode(std::string name) {
  if (sizeof(long double) != sizeof(double)) return name;
  if (name == "ld2d" || name == "d2ld") return "dmov";
  auto at = name.find("ld"); if (at != name.npos) name.replace(at, 2, "d"); return name;
}
std::string location(std::size_t block, std::size_t instruction) { return std::to_string(block) + '/' + std::to_string(instruction); }
struct Snapshot {
  std::vector<std::vector<std::string>> bodies;
  std::vector<std::vector<std::size_t>> successors;
  std::vector<std::vector<std::size_t>> predecessors;
  std::vector<bool> address_roots;
  std::map<std::string, std::string> edges;
  std::string str() const {
    std::ostringstream out;
    for (std::size_t b = 0; b < bodies.size(); ++b) {
      out << "bb" << b << '\n';
      for (std::size_t i = 0; i < bodies[b].size(); ++i) out << location(b, i) << ':' << bodies[b][i] << '\n';
      out << " successors:"; for (auto successor : successors[b]) out << ' ' << successor; out << '\n';
      if (!predecessors.empty()) {
        out << " predecessors:"; for (auto pred : predecessors[b]) out << ' ' << pred;
        out << " address-root:" << address_roots[b] << '\n';
      }
    }
    for (auto &[use, definition] : edges) out << use << " <- " << definition << '\n'; return out.str();
  }
};
struct NativeSnapshot {
  Function &function;
  const FunctionGraph &graph;
  std::map<const Register *, std::size_t> registers;
  std::map<const Instruction *, std::string> instructions, returns;
  std::map<const SSAPhi *, std::string> phis;
  std::map<const Symbol *, std::string> pools;
  bool jump_state = false;
  std::size_t reg(const Register *r) {
    if (!r) return 0; auto [found, fresh] = registers.emplace(r, registers.size() + 1); return found->second;
  }
  std::string operand(const Operand &o) {
    if (auto r = o.get<const Register *>()) return "r" + std::to_string(reg(*r)) + ':' + std::to_string(type_code((*r)->type(), true)) + ':' + std::string((*r)->hardware_binding());
    if (auto l = o.get<const Label *>()) return "l" + std::to_string(graph.indices.at(&(*l)->block()));
    if (auto s = o.get<SymbolReference>()) {
      auto symbol = s->symbol;
      if (symbol->kind() == Symbol::Kind::Import) for (auto &m : function.module().context().modules()) for (auto &candidate : m.symbols())
        if (candidate.exported() && candidate.name() == symbol->name()) symbol = &candidate;
      auto found = pools.find(symbol); auto name = found == pools.end() ? std::string(symbol->name()) : found->second;
      return "s" + name + ':' + std::to_string(s->displacement);
    }
    if (auto m = o.get<Memory>()) {
      auto base = reg(m->base), index = reg(m->index);
      return "m" + std::to_string(type_code(m->type)) + ':' + (m->type.is_block() ? std::to_string(m->type.block_size()) : std::to_string(m->displacement))
        + ':' + std::to_string(base) + ':' + std::to_string(index) + ':' + std::to_string(m->index ? m->scale : 1) + ':' + m->alias + ':' + m->nonalias;
    }
    if (auto v = o.get<std::int64_t>()) return "i" + bits(*v);
    if (auto v = o.get<std::uint64_t>()) return "u" + bits(*v);
    if (auto v = o.get<float>()) return "f" + bits(*v);
    if (auto v = o.get<double>()) return "d" + bits(*v);
    if (auto v = o.get<long double>()) return (sizeof(long double) == 8 ? "d" : "e") + bits(*v);
    return "string:" + *o.get<std::string>();
  }
  Snapshot run() {
    Snapshot result; result.bodies.resize(graph.blocks.size()); result.successors.resize(graph.blocks.size());
    if (jump_state) { result.predecessors.resize(graph.blocks.size()); result.address_roots.resize(graph.blocks.size()); }
    for (auto argument : function.arguments()) reg(argument);
    std::size_t pool = 0;
    for (auto &data : function.module().data()) if (data.is_literal_pool()) pools.emplace(&data.symbol(), "pool" + std::to_string(pool++));
    auto form = function.ssa_form();
    for (std::size_t b = 0; b < graph.blocks.size(); ++b) {
      if (auto block = graph.blocks[b].ir) {
        auto insert_phis = [&](const Instruction *before) {
          if (form) for (auto phi : form->phis(*block)) if (phi->before() == before) {
            phis.emplace(phi, location(b, result.bodies[b].size())); auto shape = "phi " + operand(Operand(phi->output().reg()));
            for (auto use : phi->inputs()) shape += ' ' + operand(Operand(use->value().reg())); result.bodies[b].push_back(shape);
          }
        };
        for (auto &i : block->instructions()) {
          insert_phis(&i);
          if (i.opcode() == Opcode::Ret) {
            std::string use = "use";
            for (auto &r : function.registers()) if (!r.hardware_binding().empty() && !r.is_memory_backed()) use += ' ' + operand(Operand(r));
            if (use != "use") { returns.emplace(&i, location(b, result.bodies[b].size())); result.bodies[b].push_back(use); }
          }
          instructions.emplace(&i, location(b, result.bodies[b].size())); auto shape = target_opcode(std::string(instruction_descriptor(i.opcode())->name));
          for (std::size_t p = 0; p < i.operands().size(); ++p) {
            if (!p && instruction_descriptor(i.opcode())->count == OperandCount::Call) {
              auto sig = i.operands()[0].get<SymbolReference>()->symbol->signature(); shape += " signature:" + std::to_string(sig->variadic);
              for (auto t : sig->results) shape += ":r" + std::to_string(type_code(t));
              for (auto t : sig->parameters) shape += ":a" + std::to_string(type_code(t)) + ':' + std::to_string(t.block_size());
            } else shape += ' ' + operand(i.operands()[p]);
          }
          result.bodies[b].push_back(shape);
        }
        insert_phis(nullptr);
      }
      for (auto id : graph.outgoing[b]) result.successors[b].push_back(graph.edges[id].destination);
      if (jump_state) {
        for (auto id : graph.incoming[b]) result.predecessors[b].push_back(graph.edges[id].source);
        result.address_roots[b] = graph.blocks[b].address_taken;
      }
    }
    if (form) for (auto &use : form->uses()) if (use.alive()) {
      auto input = std::visit([&](const auto &site) {
        using T = std::decay_t<decltype(site)>;
        if constexpr (std::is_same_v<T, SSAInstructionUse>) return instructions.at(site.instruction) + ':' + std::to_string(site.operand) + '/' + std::to_string(static_cast<unsigned>(site.part));
        else if constexpr (std::is_same_v<T, SSAPhiUse>) return phis.at(site.phi) + ':' + std::to_string(site.input + 1) + "/0";
        else {
          std::size_t p = 0; for (auto &reg : function.registers()) if (!reg.hardware_binding().empty() && !reg.is_memory_backed()) { if (&reg == site.hardware) break; ++p; }
          return returns.at(site.ret) + ':' + std::to_string(p) + "/0";
        }
      }, use.site());
      auto &value = use.value(); std::string definition;
      if (value.instruction()) definition = instructions.at(value.instruction()) + ':' + std::to_string(value.operand());
      else if (value.phi()) definition = phis.at(value.phi()) + ":0";
      else definition = (value.kind() == SSAValue::Kind::Entry ? "entry:" : "undefined:") + std::to_string(reg(&value.reg()));
      result.edges.emplace(std::move(input), std::move(definition));
    }
    return result;
  }
};
struct CReference {
  MIR_context_t context;
  MIR_item_t item;
  std::map<MIR_item_t, std::string> pools;
  std::map<MIR_reg_t, std::size_t> registers;
  std::map<MIR_insn_t, std::string> instructions;
  std::map<MIR_insn_t, std::size_t> labels;
  std::vector<std::vector<MIR_insn_t>> bodies;
  Snapshot snapshot;
  std::map<std::string, Snapshot> stages;
  std::vector<std::pair<std::string, Snapshot>> sequence;
  bool sequence_only = false;
  std::size_t reg(MIR_reg_t r) { if (!r) return 0; auto [found, fresh] = registers.emplace(r, registers.size() + 1); return found->second; }
  static void cfg(std::size_t b, MIR_insn_t i, std::size_t successor, void *data) {
    auto &self = *static_cast<CReference *>(data);
    if (!i && successor == SIZE_MAX) { self.bodies.emplace_back(); self.snapshot.bodies.emplace_back(); self.snapshot.successors.emplace_back(); }
    else if (!i) self.snapshot.successors[b].push_back(successor);
    else if (i->code == MIR_LABEL) self.labels.emplace(i, b);
    else if (i->code == MIR_USE && i->nops == 0) return;
    else { self.instructions.emplace(i, location(b, self.bodies[b].size())); self.bodies[b].push_back(i); }
  }
  std::string operand(MIR_op_t o) {
    auto function = item->u.func;
    switch (o.mode) {
    case MIR_OP_VAR: {
      auto r = _MIR_opt_test_var_register(o.u.var); auto binding = MIR_reg_hard_reg_name(context, r, function);
      return "r" + std::to_string(reg(r)) + ':' + std::to_string(MIR_reg_type(context, r, function)) + ':' + (binding ? binding : "");
    }
    case MIR_OP_VAR_MEM: {
      auto base = reg(_MIR_opt_test_var_register(o.u.var_mem.base)), index = reg(_MIR_opt_test_var_register(o.u.var_mem.index));
      return "m" + std::to_string(o.u.var_mem.type) + ':' + std::to_string(o.u.var_mem.disp) + ':' + std::to_string(base) + ':' + std::to_string(index)
        + ':' + std::to_string(index ? o.u.var_mem.scale : 1) + ':' + (o.u.var_mem.alias ? MIR_alias_name(context, o.u.var_mem.alias) : "")
        + ':' + (o.u.var_mem.nonalias ? MIR_alias_name(context, o.u.var_mem.nonalias) : "");
    }
    case MIR_OP_LABEL: {
      auto found = labels.find(o.u.label); CHECK(found != labels.end());
      return found == labels.end() ? "missing-label" : "l" + std::to_string(found->second);
    }
    case MIR_OP_REF: {
      while (o.u.ref->ref_def) o.u.ref = o.u.ref->ref_def;
      auto found = pools.find(o.u.ref); return "s" + (found == pools.end() ? std::string(MIR_item_name(context, o.u.ref)) : found->second) + ":0";
    }
    case MIR_OP_INT: return "i" + bits(o.u.i); case MIR_OP_UINT: return "u" + bits(o.u.u);
    case MIR_OP_FLOAT: return "f" + bits(o.u.f); case MIR_OP_DOUBLE: return "d" + bits(o.u.d); case MIR_OP_LDOUBLE: return "e" + bits(o.u.ld);
    case MIR_OP_STR: return "string:" + std::string(o.u.str.s, o.u.str.len);
    default: return "invalid:" + std::to_string(o.mode);
    }
  }
  static void edge(MIR_insn_t use, std::size_t up, MIR_insn_t def, std::size_t dp, unsigned part, unsigned fake, MIR_reg_t r, void *data) {
    auto &self = *static_cast<CReference *>(data);
    auto definition = fake ? std::string(fake == 1 ? "entry:" : "undefined:") + std::to_string(self.reg(r))
      : self.instructions.at(def) + ':' + std::to_string(dp);
    self.snapshot.edges.emplace(self.instructions.at(use) + ':' + std::to_string(up) + '/' + std::to_string(part), definition);
  }
  static void jump_state(unsigned kind, std::size_t block, std::size_t value, void *data) {
    auto &snapshot = static_cast<CReference *>(data)->snapshot;
    if (kind == 0) snapshot.predecessors[block].push_back(value); else snapshot.address_roots[block] = value;
  }
  static void observe(const char *stage, MIR_context_t context, MIR_item_t item, void *data) {
    auto &self = *static_cast<CReference *>(data); self.bodies.clear(); self.instructions.clear(); self.labels.clear(); self.registers.clear(); self.snapshot = {};
    for (std::size_t n = 0; n < item->u.func->nargs; ++n) self.reg(MIR_reg(context, VARR_GET(MIR_var_t, item->u.func->vars, n).name, item->u.func));
    _MIR_opt_test_visit_cfg(context, item, cfg, data);
    if (std::string_view(stage) == "jump-opt") {
      self.snapshot.predecessors.resize(self.bodies.size()); self.snapshot.address_roots.resize(self.bodies.size());
      _MIR_opt_test_visit_jump_state(context, item, jump_state, data);
    }
    for (std::size_t b = 0; b < self.bodies.size(); ++b) for (auto i : self.bodies[b]) {
      auto shape = std::string(MIR_insn_name(context, i->code));
      for (std::size_t p = 0; p < i->nops; ++p) {
        if (!p && MIR_call_code_p(i->code)) {
          // Copied prototypes have independent names; signatures are interned
          // structural types. Preserve every ABI classification and block size.
          auto sig = i->ops[0].u.ref->u.proto; shape += " signature:" + std::to_string(sig->vararg_p);
          for (std::size_t r = 0; r < sig->nres; ++r) shape += ":r" + std::to_string(sig->res_types[r]);
          for (std::size_t a = 0; a < VARR_LENGTH(MIR_var_t, sig->args); ++a) {
            auto arg = VARR_GET(MIR_var_t, sig->args, a);
            shape += ":a" + std::to_string(arg.type) + ':' + std::to_string(MIR_all_blk_type_p(arg.type) ? arg.size : 0);
          }
        } else shape += ' ' + self.operand(i->ops[p]);
      }
      self.snapshot.bodies[b].push_back(shape);
    }
    if (std::string_view(stage) != "destroy-ssa" && std::string_view(stage) != "transform-addresses"
        && std::string_view(stage) != "clone-blocks" && std::string_view(stage) != "prepare-cfg"
        && std::string_view(stage) != "build-cfg" && std::string_view(stage) != "jump-opt") _MIR_opt_test_visit_ssa(context, item, edge, data);
    if (std::string_view(stage) == "ssa-combine") {
      // C follows a fake self move and drops its address edge. Canonical SSA
      // retains the typed initial value; reconstruct only those known boundaries.
      for (auto &body : self.bodies) for (auto i : body) for (std::size_t p = 0; p < i->nops; ++p) if (i->ops[p].mode == MIR_OP_VAR_MEM) {
        auto &m = i->ops[p].u.var_mem;
        for (auto [variable, part] : {std::pair{m.base, 1u}, std::pair{m.index, 2u}}) {
          if (variable == MIR_NON_VAR) continue;
          auto site = self.instructions.at(i) + ':' + std::to_string(p) + '/' + std::to_string(part);
          if (self.snapshot.edges.contains(site)) continue;
          auto reg = _MIR_opt_test_var_register(variable), kind = _MIR_opt_test_initial_register_kind(context, reg);
          REQUIRE(kind != 0);
          self.snapshot.edges.emplace(site, std::string(kind == 1 ? "entry:" : "undefined:") + std::to_string(self.reg(reg)));
        }
      }
    }
    if (self.sequence_only) self.sequence.emplace_back(stage, std::move(self.snapshot));
    else self.stages.emplace(stage, self.snapshot);
  }
};
void compare(std::string_view text, bool rename, unsigned level, bool addresses = false, bool cloning = false, bool gvn = false, bool memory = false, bool dce = false, bool motion = false, bool combine = false, bool jumps = false) {
  auto source = serialization::parse_text(text); INFO((source ? "" : source.error().message)); REQUIRE(source);
  auto encoded = serialization::write_text(**source); REQUIRE(encoded);
  auto context = MIR_init();
  // The original scanner rejects signed inf/nan tokens. Its binary reader is
  // the independent oracle for nonfinite values and preserves their bits.
  if (text.find("+nan") != text.npos || text.find("+inf") != text.npos) {
    auto binary = serialization::write_binary(**source); REQUIRE(binary); auto file = std::tmpfile(); REQUIRE(file);
    REQUIRE(std::fwrite(binary->data(), 1, binary->size(), file) == binary->size()); std::rewind(file); MIR_read(context, file); std::fclose(file);
  } else MIR_scan_string(context, encoded->c_str());
  MIR_gen_init(context);
  auto module = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(context)); REQUIRE(module);
  std::set<MIR_item_t> old_data;
  for (auto i = DLIST_HEAD(MIR_item_t, module->items); i; i = DLIST_NEXT(MIR_item_t, i)) if (i->item_type == MIR_data_item) old_data.insert(i);
  _MIR_opt_test_simplify_module(context, module);
  FunctionPassManager prepare; prepare.add<SimplifyPass>();
  auto status = prepare.run(**source); INFO((status ? "" : status.error().message)); REQUIRE(status);
  for (auto &function : (*source)->modules().begin()->functions()) {
    FunctionAnalysisManager analyses(function); FunctionPassManager cfg; cfg.add<PrepareCFGPass>(level != 0);
    status = cfg.run(function, analyses); INFO((status ? "" : status.error().message)); REQUIRE(status);
    MIR_item_t item = nullptr;
    for (auto i = DLIST_HEAD(MIR_item_t, module->items); i; i = DLIST_NEXT(MIR_item_t, i))
      if (i->item_type == MIR_func_item && std::string_view(MIR_item_name(context, i)) == function.symbol().name()) item = i;
    REQUIRE(item); CReference reference{context, item}; std::size_t pool = 0;
    for (auto i = DLIST_HEAD(MIR_item_t, module->items); i; i = DLIST_NEXT(MIR_item_t, i))
      if (i->item_type == MIR_data_item && !old_data.contains(i)) reference.pools.emplace(i, "pool" + std::to_string(pool++));
    if (jumps) REQUIRE(_MIR_opt_test_jump_function(context, item, level, gvn, CReference::observe, &reference));
    else if (combine) REQUIRE(_MIR_opt_test_combine_function(context, item, level, gvn, CReference::observe, &reference));
    else if (motion) REQUIRE(_MIR_opt_test_motion_function(context, item, level, CReference::observe, &reference));
    else if (dce) REQUIRE(_MIR_opt_test_dce_function(context, item, level, CReference::observe, &reference));
    else if (memory) REQUIRE(_MIR_opt_test_memory_function(context, item, level, CReference::observe, &reference));
    else if (gvn) REQUIRE(_MIR_opt_test_gvn_function(context, item, level, CReference::observe, &reference));
    else if (cloning) REQUIRE(_MIR_opt_test_clone_function(context, item, level, CReference::observe, &reference));
    else if (addresses) REQUIRE(_MIR_opt_test_address_function(context, item, level, CReference::observe, &reference));
    else REQUIRE(_MIR_opt_test_ssa_function(context, item, level, rename, CReference::observe, &reference));
    FunctionPassManager passes;
    if (!jumps || gvn) {
    if (cloning) passes.add<BlockCloningPass>();
    passes.add<BuildSSAPass>(addresses ? false : rename);
    if (gvn) passes.add<GVNPass>();
    if (memory) { passes.add<CopyPropagationPass>(); passes.add<DeadStoresPass>(); }
    if (dce) passes.add<SSADeadCodePass>();
    if (motion) { passes.add<LICMPass>(); passes.add<PressureReliefPass>(); }
    if (addresses) { passes.add<AddressTransformPass>(); passes.add<BuildSSAPass>(); }
    if (rename || addresses) passes.add<ConventionalSSAPass>();
    if (combine) passes.add<SSACombinePass>(); passes.add<DestroySSAPass>();
    }
    if (jumps) passes.add<JumpOptimizationPass>();
    status = passes.run(function, analyses, [&](const PassEvent &event) -> Result<void> {
      if (event.phase == PassEvent::Phase::Before) return {};
      auto graph = analyses.get<FunctionGraphAnalysis>(); REQUIRE(graph);
      auto phase = std::string(event.pass);
      if (addresses && phase == "build-ssa" && function.ssa_form()->renamed()) phase = "rebuild-ssa";
      NativeSnapshot native{function, **graph}; native.jump_state = phase == "jump-opt";
      auto actual = native.run().str(), expected = reference.stages.at(phase).str();
      INFO("input:\n", *encoded, "\nphase:", event.pass, "\nC:\n", expected, "\nC++:\n", actual); CHECK(actual == expected); return {};
    });
    INFO((status ? "" : status.error().message)); REQUIRE(status); REQUIRE(function.verify());
  }
  MIR_gen_finish(context); MIR_finish(context);
}
// Address fixtures bind r19/r12 globally, so MIR deliberately changes that
// register across returns. Preserve the host caller's value in this boundary.
#if defined(_MSC_VER) && defined(_M_X64)
extern "C" std::int64_t mirnext_reference_invoke(std::int64_t (*)(), std::int64_t *);
std::int64_t invoke(std::int64_t (*code)(), std::int64_t *hardware = nullptr) {
  return mirnext_reference_invoke(code, hardware);
}
#else
[[gnu::noinline]] std::int64_t invoke(std::int64_t (*code)(), std::int64_t *hardware = nullptr) {
#if defined(__aarch64__)
  if (hardware) asm volatile("mov x19, %0" : : "r"(std::int64_t{7}) : "x19");
#elif defined(__x86_64__)
  if (hardware) asm volatile("mov %0, %%r12" : : "r"(std::int64_t{7}) : "r12");
#endif
  auto result = code();
#if defined(__aarch64__)
  if (hardware) asm volatile("mov %0, x19" : "=r"(*hardware));
  asm volatile("" : : : "x19");
#elif defined(__x86_64__)
  if (hardware) asm volatile("mov %%r12, %0" : "=r"(*hardware));
  asm volatile("" : : : "r12");
#endif
  return result;
}
#endif
std::int64_t execute(std::string_view text, bool jit, unsigned level, std::span<const std::byte> binary = {}, std::vector<std::byte> *state = nullptr, std::int64_t *hardware = nullptr) {
  auto context = MIR_init(); std::string input(text);
  std::vector<std::byte> nonfinite;
  if (binary.empty() && (text.find("+nan") != text.npos || text.find("+inf") != text.npos)) {
    auto source = serialization::parse_text(text); REQUIRE(source); auto encoded = serialization::write_binary(**source); REQUIRE(encoded);
    nonfinite = std::move(*encoded); binary = nonfinite;
  }
  if (binary.empty()) MIR_scan_string(context, input.c_str());
  else {
    auto file = std::tmpfile(); REQUIRE(file);
    REQUIRE(std::fwrite(binary.data(), 1, binary.size(), file) == binary.size()); std::rewind(file);
    MIR_read(context, file); std::fclose(file);
  }
  MIR_item_t entry = nullptr;
  for (auto module = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(context)); module; module = DLIST_NEXT(MIR_module_t, module)) {
    MIR_load_module(context, module);
    for (auto item = DLIST_HEAD(MIR_item_t, module->items); item; item = DLIST_NEXT(MIR_item_t, item))
      if (item->item_type == MIR_func_item && std::string_view(MIR_item_name(context, item)) == "entry") entry = item;
  }
  REQUIRE(entry); REQUIRE(entry->u.func->nargs == 0); REQUIRE(entry->u.func->nres == 1);
  std::int64_t result;
  if (jit) {
    MIR_gen_init(context); MIR_gen_set_optimize_level(context, level); MIR_link(context, MIR_set_gen_interface, nullptr);
    result = invoke(reinterpret_cast<std::int64_t (*)()>(MIR_gen(context, entry)), hardware); MIR_gen_finish(context);
  } else {
    MIR_link(context, MIR_set_interp_interface, nullptr); MIR_val_t value; MIR_interp_arr(context, entry, &value, 0, nullptr); result = value.i;
  }
  if (state) {
    state->clear();
    for (auto module = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(context)); module; module = DLIST_NEXT(MIR_module_t, module))
      for (auto item = DLIST_HEAD(MIR_item_t, module->items); item; item = DLIST_NEXT(MIR_item_t, item))
        if (item->item_type == MIR_data_item && item->u.data->name && std::string_view(item->u.data->name) == "state") {
          auto bytes = static_cast<const std::byte *>(item->addr); auto size = _MIR_type_size(context, item->u.data->el_type) * item->u.data->nel;
          state->insert(state->end(), bytes, bytes + size);
        }
  }
  MIR_finish(context); return result;
}
std::int64_t interpreted_result(const Context &context) {
  auto program = execution::InterpretedProgram::create(context); REQUIRE(program);
  auto entry = program->find_function("m","entry"); REQUIRE(entry);
  auto result = program->call_i64(*entry); REQUIRE(result); return *result;
}
void compare_limited_results(const Context &original,const Context &transformed,std::int64_t expected) {
  CHECK(interpreted_result(transformed) == expected);
  for (unsigned level : {0, 1, 2, 3}) {
    CAPTURE(level);
    for (auto context : {&original,&transformed}) {
      auto optimized = optimize(*context,{.level=static_cast<OptimizationLevel>(level)}); REQUIRE(optimized);
      CHECK(interpreted_result(optimized->context()) == expected);
      CHECK(test::check_native_result_limit(*context,{.optimization=OptimizationOptions{.level=static_cast<OptimizationLevel>(level)}}));
    }
  }
}
}
namespace {
void compare_pipeline(std::string_view text, unsigned level, bool identities = false) {
  INFO("input:\n", text);
  auto source = serialization::parse_text(text); INFO((source ? "" : source.error().message)); REQUIRE(source);
  auto encoded = serialization::write_text(**source); REQUIRE(encoded); auto context = MIR_init();
  if (text.find("+nan") != text.npos || text.find("+inf") != text.npos) {
    auto binary = serialization::write_binary(**source); REQUIRE(binary); auto file = std::tmpfile(); REQUIRE(file);
    REQUIRE(std::fwrite(binary->data(), 1, binary->size(), file) == binary->size()); std::rewind(file); MIR_read(context, file); std::fclose(file);
  } else MIR_scan_string(context, encoded->c_str());
  MIR_gen_init(context);
  std::map<std::pair<std::string, std::string>, std::vector<std::pair<std::string, Snapshot>>> expected;
  std::map<MIR_module_t, std::set<MIR_item_t>> old_data;
  std::map<MIR_item_t, std::string> symbols;
  std::map<MIR_item_t, std::pair<std::string, std::string>> functions;
  for (auto m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(context)); m; m = DLIST_NEXT(MIR_module_t, m)) {
    for (auto i = DLIST_HEAD(MIR_item_t, m->items); i; i = DLIST_NEXT(MIR_item_t, i)) {
      if (i->item_type == MIR_data_item) old_data[m].insert(i);
      if (i->item_type == MIR_func_item) functions.emplace(i, std::pair{std::string(m->name), std::string(MIR_item_name(context, i))});
      if (identities && i->item_type != MIR_import_item && i->item_type != MIR_export_item && i->item_type != MIR_forward_item)
        if (auto name = MIR_item_name(context, i)) symbols.emplace(i, std::string(m->name) + '/' + name);
    }
    _MIR_opt_test_simplify_module(context, m);
  }
  _MIR_opt_test_inline_context(context);
  for (auto m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(context)); m; m = DLIST_NEXT(MIR_module_t, m))
    for (auto i = DLIST_HEAD(MIR_item_t, m->items); i; i = DLIST_NEXT(MIR_item_t, i)) if (i->item_type == MIR_func_item) {
      CReference reference{context, i}; reference.sequence_only = true; std::size_t pool = 0;
      reference.pools = symbols;
      for (auto d = DLIST_HEAD(MIR_item_t, m->items); d; d = DLIST_NEXT(MIR_item_t, d))
        if (d->item_type == MIR_data_item && !old_data[m].contains(d)) reference.pools.emplace(d, "pool" + std::to_string(pool++));
      REQUIRE(_MIR_opt_test_pipeline_function(context, i, level, CReference::observe, &reference));
      expected.emplace(functions.at(i), std::move(reference.sequence));
    }
  std::map<std::pair<std::string, std::string>, std::size_t> positions;
  std::map<const Symbol *, std::string> native_symbols;
  std::map<const Function *, std::pair<std::string, std::string>> native_functions;
  OptimizationOptions options; options.level = static_cast<OptimizationLevel>(level);
  options.observer = [&](const OptimizationSnapshot &s) -> Result<void> {
    if (native_functions.empty()) {
      auto &c = s.function.module().context();
      for (auto &m : c.modules()) {
        for (auto &f : m.functions()) native_functions.emplace(&f, std::pair{std::string(m.name()), std::string(f.symbol().name())});
        if (identities) for (auto &symbol : m.symbols()) {
          auto definition = &symbol;
          if (symbol.kind() == Symbol::Kind::Import) for (auto &owner : c.modules()) for (auto &candidate : owner.symbols())
            if (candidate.exported() && candidate.name() == symbol.name()) definition = &candidate;
          native_symbols.emplace(&symbol, std::string(definition->module().name()) + '/' + std::string(definition->name()));
        }
      }
    }
    if (s.phase != PassEvent::Phase::After || s.statistics.pass < PassId::BuildCFG) return {};
    auto key = native_functions.at(&s.function); auto found = expected.find(key); REQUIRE(found != expected.end());
    auto &position = positions[key]; REQUIRE(position < found->second.size()); auto &[phase, snapshot] = found->second[position++];
    INFO("phase:", phase, " function:", s.statistics.function, " input:\n", text);
    CHECK(pass_name(s.statistics.pass) == phase); REQUIRE(s.graph); if (!s.graph) return Error{ErrorCode::InvalidArgument, "missing observed CFG"};
    NativeSnapshot native{const_cast<Function &>(s.function), *s.graph}; native.pools = native_symbols; native.jump_state = s.statistics.pass == PassId::JumpOptimization;
    auto actual = native.run().str(), wanted = snapshot.str();
    INFO("input:\n", text, "\nfunction:", s.statistics.function, "\nphase:", phase, "\nC:\n", wanted, "\nC++:\n", actual);
    CHECK(actual == wanted); return {};
  };
  auto result = optimize(**source, options); INFO((result ? "" : result.error().message)); REQUIRE(result); REQUIRE(result->verify());
  for (auto &[key, sequence] : expected) CHECK(positions[key] == sequence.size());
  MIR_gen_finish(context); MIR_finish(context);
}
}
TEST_CASE("complete canonical pipeline preserves independent C phase order CFG and SSA at O0 through O3") {
  auto inputs = test::analysis_fixtures();
  for (auto group : {test::address_fixtures(), test::cloning_fixtures(), test::gvn_fixtures(), test::memory_fixtures(),
                    test::motion_fixtures(), test::combine_fixtures(), test::jump_fixtures()}) inputs.insert(inputs.end(), group.begin(), group.end());
  for (auto &text : inputs) for (unsigned level : {0, 1, 2, 3}) { CAPTURE(level); compare_pipeline(text, level); }
}
TEST_CASE("complete pipeline compares every opcode original corpus and fixed seed programs") {
  for (auto &d : instruction_descriptors()) {
    CAPTURE(d.name); auto source = test::public_instruction_example(d); auto text = serialization::write_text(*source); REQUIRE(text);
    for (unsigned level : {0, 1, 2, 3}) { CAPTURE(level); compare_pipeline(*text, level); }
  }
  for (unsigned n = 1; n <= 16; ++n) {
    CAPTURE(n); std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"mir-tests"/("test"+std::to_string(n)+".mir")); REQUIRE(input);
    std::string text((std::istreambuf_iterator<char>(input)), {}); for (unsigned level : {0, 1, 2, 3}) { CAPTURE(level); compare_pipeline(text, level); }
  }
  for (auto group : {test::random_jump_fixtures(), test::random_address_fixtures(), test::random_memory_fixtures(), test::random_motion_fixtures()})
    for (auto &text : group) compare_pipeline(text, 2);
}
TEST_CASE("owning full pipeline preserves cross module inline identities data order and execution") {
  auto fixtures = test::inline_fixtures();
  for (auto n : {fixtures.size() - 2, fixtures.size() - 1}) for (unsigned level : {0, 1, 2, 3}) {
    CAPTURE(n); CAPTURE(level); auto &text = fixtures[n]; compare_pipeline(text, level, true);
    auto source = serialization::parse_text(text); REQUIRE(source); OptimizationOptions options; options.level = static_cast<OptimizationLevel>(level);
    auto result = optimize(**source, options); REQUIRE(result); auto binary = result->encode_binary(); REQUIRE(binary);
    auto expected = execute(text, false, 0); CHECK(execute({}, false, 0, *binary) == expected);
    CHECK(execute(text, true, level) == expected); CHECK(execute({}, true, level, *binary) == expected);
  }
}
TEST_CASE("owning optimization preserves returns stores and original interpreter and JIT at all levels") {
  auto inputs = test::random_jump_fixtures();
  for (auto group : {test::random_memory_fixtures(), test::random_motion_fixtures(), test::random_combine_fixtures()}) inputs.insert(inputs.end(), group.begin(), group.end());
  for (auto &text : inputs) {
    CAPTURE(text); std::vector<std::byte> before, after; auto expected = execute(text, false, 0, {}, &before);
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level : {0, 1, 2, 3}) {
      CAPTURE(level); OptimizationOptions options; options.level = static_cast<OptimizationLevel>(level);
      auto result = optimize(**source, options); INFO((result ? "" : result.error().message)); REQUIRE(result);
      auto binary = result->encode_binary(); REQUIRE(binary);
      CHECK(execute({}, false, 0, *binary, &after) == expected); CHECK(after == before);
      CHECK(execute(text, true, level, {}, &after) == expected); CHECK(after == before);
      CHECK(execute({}, true, level, *binary, &after) == expected); CHECK(after == before);
    }
  }
}
TEST_CASE("closed PHI deletion and hardware copy coalescing retain defined edges and observable writes") {
  constexpr auto closed = "m:module\nentry:func i64\nlocal i64:v, i64:c\nmov v, 0\nmov c, 1\nbt cold, c\nhot:\nblt hot, v, 14\nret v\ncold:\nmov v, 14\njmp hot\nendfunc\nendmodule\n";
#if defined(__aarch64__)
  constexpr auto binding = "r19";
#else
  constexpr auto binding = "r12";
#endif
  auto bound = "m:module\nentry:func i64\nglobal i64:h:" + std::string(binding) + "\nlocal i64:a\nmov h, 42\nmov a, h\nret a\nendfunc\nendmodule\n";
  for (std::string_view text : {std::string_view(closed), std::string_view(bound)}) {
    for (unsigned level : {0, 1, 2, 3}) {
      CAPTURE(level); compare_pipeline(text, level); CHECK(execute(text, false, 0) == (text == closed ? 14 : 42));
      auto source = serialization::parse_text(text); REQUIRE(source); OptimizationOptions options; options.level = static_cast<OptimizationLevel>(level);
      auto result = optimize(**source, options); REQUIRE(result); auto binary = result->encode_binary(); REQUIRE(binary);
      std::int64_t hardware = 0;
      CHECK(execute(text, true, level, {}, nullptr, &hardware) == (text == closed ? 14 : 42)); if (text != closed) CHECK(hardware == 42);
      CHECK(execute({}, true, level, *binary, nullptr, &hardware) == (text == closed ? 14 : 42)); if (text != closed) CHECK(hardware == 42);
    }
  }
}
TEST_CASE("SSA construction rename conventionalization and exit match isolated original C stages") {
  for (auto &text : test::analysis_fixtures()) for (bool rename : {false, true}) for (unsigned level : {2, 3}) { CAPTURE(rename); CAPTURE(level); compare(text, rename, level); }
}
TEST_CASE("fixed seed valid CFGs retain original SSA choices and ordered copies") {
  unsigned n = 0;
  for (auto &text : test::random_analysis_fixtures()) { CAPTURE(n); compare(text, false, 2); compare(text, true, 2); ++n; }
}
TEST_CASE("original MIR regression corpus retains complete SSA stage behavior") {
  for (unsigned n = 1; n <= 16; ++n) {
    CAPTURE(n); std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"mir-tests"/("test"+std::to_string(n)+".mir")); REQUIRE(input);
    std::string text((std::istreambuf_iterator<char>(input)), {}); compare(text, false, 2); compare(text, true, 2);
  }
}
TEST_CASE("every public opcode participates in the isolated SSA lifecycle") {
  for (auto &descriptor : instruction_descriptors()) {
    CAPTURE(descriptor.name); auto context = test::public_instruction_example(descriptor);
    if (descriptor.opcode == Opcode::VaStart || descriptor.opcode == Opcode::VaEnd || descriptor.opcode == Opcode::VaArg || descriptor.opcode == Opcode::VaBlockArg)
      for (auto &f : context->modules().begin()->functions()) for (auto &b : f.blocks()) for (auto &i : b.instructions())
        for (std::size_t n = 0; n < i.operands().size(); ++n) if (auto m = i.operands()[n].get<Memory>(); m && m->type.kind() == Type::Kind::Undef) {
          auto memory = *m; memory.displacement = 128; REQUIRE(IRRewriter(f).replace_operand(i, n, Operand(memory)));
        }
    auto text = serialization::write_text(*context); REQUIRE(text); compare(*text, false, 2); compare(*text, true, 2);
  }
}
TEST_CASE("native SSA exit preserves execution through loops PHI cycles and memory effects") {
  const std::string_view sources[]{
R"(m: module
entry: func i64
local i64:i, i64:sum
mov i, 0
mov sum, 0
loop:
add i, i, 1
add sum, sum, i
blt loop, i, 10
ret sum
endfunc
endmodule
)",
R"(m: module
entry: func i64
local i64:a, i64:b, i64:c, i64:i
mov a, 1
mov b, 2
mov i, 0
loop:
mov c, a
mov a, b
add b, b, c
add i, i, 1
blt loop, i, 12
ret a
endfunc
endmodule
)",
R"(m: module
state: i64 0
entry: func i64
local i64:p, i64:i, i64:value
mov p, state
mov i, 0
loop:
mov value, i64:0(p)
add value, value, 3
mov i64:0(p), value
add i, i, 1
blt loop, i, 14
mov value, i64:0(p)
ret value
endfunc
endmodule
)"
  };
  for (auto source : sources) {
    auto expected = execute(source, false, 0);
    for (bool rename : {false, true}) {
      auto parsed = serialization::parse_text(source); REQUIRE(parsed);
      FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>(rename);
      if (rename) passes.add<ConventionalSSAPass>(); passes.add<DestroySSAPass>();
      auto result = passes.run(**parsed); INFO((result ? "" : result.error().message)); REQUIRE(result);
      auto text = serialization::write_text(**parsed); REQUIRE(text); CHECK(execute(*text, false, 0) == expected);
      for (unsigned level : {0, 1, 2, 3}) { CAPTURE(level); CHECK(execute(*text, true, level) == expected); }
    }
  }
}
TEST_CASE("address conversion exit and rebuilt SSA match isolated original C stages") {
  auto inputs = test::address_fixtures(); auto random = test::random_address_fixtures();
  inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) for (unsigned level : {2, 3}) { CAPTURE(level); compare(text, true, level, true); }
}
TEST_CASE("hot block cloning and subsequent SSA match isolated original C stages") {
  auto inputs = test::cloning_fixtures(); auto random = test::random_cloning_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) for (unsigned level : {2, 3}) { CAPTURE(level); compare(text, true, level, false, true); }
}
TEST_CASE("GVN transformed IR CFG and SSA match isolated original C stages") {
  for (auto &text : test::gvn_fixtures()) for (unsigned level : {2, 3}) { CAPTURE(text); CAPTURE(level); compare(text, true, level, false, false, true); }
}
TEST_CASE("copy propagation and dead stores match original C stages") {
  auto inputs = test::gvn_fixtures(); auto rules = test::gvn_rule_fixtures(); auto random = test::random_gvn_fixtures();
  inputs.insert(inputs.end(), rules.begin(), rules.end()); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) for (unsigned level : {2, 3}) { CAPTURE(text); CAPTURE(level); compare(text, true, level, false, false, true, true); }
}
TEST_CASE("copy propagation and dead stores cover the original MIR corpus") {
  for (unsigned n = 1; n <= 16; ++n) {
    CAPTURE(n); std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"mir-tests"/("test"+std::to_string(n)+".mir")); REQUIRE(input);
    std::string text((std::istreambuf_iterator<char>(input)), {}); compare(text, true, 2, false, false, true, true);
  }
}
TEST_CASE("copy propagation and dead stores cover rule boundaries and fixed seed loops") {
  auto inputs = test::memory_fixtures(); auto random = test::random_memory_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) for (unsigned level : {2, 3}) { CAPTURE(text); CAPTURE(level); compare(text, true, level, false, false, true, true); }
}
TEST_CASE("copy propagation and dead stores retain CFG and PHI boundary behavior") {
  auto inputs = test::analysis_fixtures(); auto random = test::random_analysis_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) { CAPTURE(text); compare(text, true, 2, false, false, true, true); }
}
TEST_CASE("SSA dead code matches independent C across rules loops and original corpus") {
  auto inputs = test::memory_fixtures(); auto random = test::random_memory_fixtures();
  auto rules = test::gvn_rule_fixtures(); auto graph = test::analysis_fixtures();
  inputs.insert(inputs.end(), random.begin(), random.end()); inputs.insert(inputs.end(), rules.begin(), rules.end()); inputs.insert(inputs.end(), graph.begin(), graph.end());
  for (auto &text : inputs) for (unsigned level : {2, 3}) { CAPTURE(text); CAPTURE(level); compare(text, true, level, false, false, true, true, true); }
  for (unsigned n = 1; n <= 16; ++n) {
    CAPTURE(n); std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"mir-tests"/("test"+std::to_string(n)+".mir")); REQUIRE(input);
    std::string text((std::istreambuf_iterator<char>(input)), {}); compare(text, true, 2, false, false, true, true, true);
  }
}
TEST_CASE("LICM and pressure relief match independent C stages") {
  auto inputs = test::motion_fixtures(); auto random = test::random_motion_fixtures();
  inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) for (unsigned level : {2, 3}) { CAPTURE(text); CAPTURE(level); compare(text, true, level, false, false, true, true, true, true); }
}
TEST_CASE("SSA cleanup and motion cover memory CFG and original regression programs") {
  auto inputs = test::memory_fixtures(); auto graph = test::analysis_fixtures(); auto random = test::random_analysis_fixtures();
  inputs.insert(inputs.end(), graph.begin(), graph.end()); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) { CAPTURE(text); compare(text, true, 2, false, false, true, true, true, true); }
  for (unsigned n = 1; n <= 16; ++n) {
    CAPTURE(n); std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"mir-tests"/("test"+std::to_string(n)+".mir")); REQUIRE(input);
    std::string text((std::istreambuf_iterator<char>(input)), {}); compare(text, true, 2, false, false, true, true, true, true);
  }
}
TEST_CASE("SSA cleanup and motion preserve interpretation and all original JIT levels") {
  auto inputs = test::motion_fixtures(); auto random = test::random_motion_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &source : inputs) {
    CAPTURE(source); auto parsed = serialization::parse_text(source); REQUIRE(parsed);
    if (!(*parsed)->modules().begin()->find_symbol("entry")->function()->arguments().empty()) continue;
    auto expected = execute(source, false, 0);
    FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>();
    passes.add<GVNPass>(); passes.add<CopyPropagationPass>(); passes.add<DeadStoresPass>(); passes.add<SSADeadCodePass>();
    passes.add<LICMPass>(); passes.add<PressureReliefPass>(); passes.add<ConventionalSSAPass>(); passes.add<DestroySSAPass>();
    auto status = passes.run(**parsed); INFO((status ? "" : status.error().message)); REQUIRE(status);
    auto binary = serialization::write_binary(**parsed); REQUIRE(binary); CHECK(execute({}, false, 0, *binary) == expected);
    for (unsigned level : {0, 1, 2, 3}) { CAPTURE(level); CHECK(execute(source, true, level) == expected); CHECK(execute({}, true, level, *binary) == expected); }
  }
}
TEST_CASE("original C hardware return boundary preserves block instruction order at every JIT level") {
  for (auto &source : test::motion_fixtures()) if (source.find("mul h,") != std::string::npos) {
    compare(source, true, 2, false, false, true, true, true, true);
    CHECK(execute(source, false, 0) == 308);
    for (unsigned level : {0, 1, 2, 3}) CHECK(execute(source, true, level) == 308);
  }
}
TEST_CASE("LICM never speculates property control flow out of a loop") {
  for (auto branch : {"prbeq", "prbne"}) {
    auto text = "m: module\nentry: func i64, i64:x, i64:y\nlocal i64:i, i64:a, i64:sum\nmov i, 0\nmov sum, 0\nprset x, 1\n"
      + std::string(branch) + " done, x, 0\nloop:\nmul a, x, y\nadd sum, sum, a\n"
      + branch + " done, x, 1\nadd i, i, 1\nblt loop, i, 4\ndone:\nret sum\nendfunc\nendmodule\n";
    compare(text, true, 2, false, false, true, true, true, true);
  }
}
TEST_CASE("jump optimization matches isolated original C stages at its original enabling levels") {
  auto inputs = test::jump_fixtures(); auto graph = test::analysis_fixtures(); auto random = test::random_analysis_fixtures();
  inputs.insert(inputs.end(), graph.begin(), graph.end()); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) for (unsigned level : {2, 3}) {
    CAPTURE(text); CAPTURE(level); compare(text, true, level, false, false, false, false, false, false, false, true);
  }
}
TEST_CASE("jump optimization matches C after the complete value memory and motion pipeline") {
  auto inputs = test::jump_fixtures(); auto random = test::random_jump_fixtures(); auto memory = test::memory_fixtures(); auto loops = test::motion_fixtures();
  inputs.insert(inputs.end(), random.begin(), random.end()); inputs.insert(inputs.end(), memory.begin(), memory.end()); inputs.insert(inputs.end(), loops.begin(), loops.end());
  for (auto &text : inputs) for (unsigned level : {2, 3}) {
    CAPTURE(text); CAPTURE(level); compare(text, true, level, false, false, true, true, true, true, true, true);
  }
  for (unsigned n = 1; n <= 16; ++n) {
    CAPTURE(n); std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"mir-tests"/("test"+std::to_string(n)+".mir")); REQUIRE(input);
    std::string text((std::istreambuf_iterator<char>(input)), {}); compare(text, true, 2, false, false, true, true, true, true, true, true);
  }
  for (auto &descriptor : instruction_descriptors()) {
    CAPTURE(descriptor.name); auto source = test::public_instruction_example(descriptor); auto text = serialization::write_text(*source); REQUIRE(text);
    compare(*text, true, 2, false, false, true, true, true, true, true, true);
  }
}
TEST_CASE("jump optimized seeded programs preserve returns global stores and all JIT levels") {
  for (auto &text : test::random_jump_fixtures()) for (bool full : {false, true}) {
    CAPTURE(text); CAPTURE(full); std::vector<std::byte> before, after; auto expected = execute(text, false, 0, {}, &before);
    auto c = serialization::parse_text(text); REQUIRE(c); FunctionPassManager p; p.add<SimplifyPass>(); p.add<PrepareCFGPass>();
    if (full) {
      p.add<BuildSSAPass>(); p.add<GVNPass>(); p.add<CopyPropagationPass>(); p.add<DeadStoresPass>(); p.add<SSADeadCodePass>();
      p.add<LICMPass>(); p.add<PressureReliefPass>(); p.add<ConventionalSSAPass>(); p.add<SSACombinePass>(); p.add<DestroySSAPass>();
    }
    p.add<JumpOptimizationPass>(); REQUIRE(p.run(**c)); auto binary = serialization::write_binary(**c); REQUIRE(binary);
    CHECK(execute({}, false, 0, *binary, &after) == expected); CHECK(after == before);
    for (unsigned level : {0, 1, 2, 3}) {
      CAPTURE(level); CHECK(execute(text, true, level, {}, &after) == expected); CHECK(after == before);
      CHECK(execute({}, true, level, *binary, &after) == expected); CHECK(after == before);
    }
  }
}
TEST_CASE("SSA combine matches independent conventional SSA branch and address stages") {
  auto inputs = test::combine_fixtures(); auto random = test::random_combine_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) for (unsigned level : {2, 3}) {
    CAPTURE(text); compare(text, true, level, false, false, true, true, true, true, true);
    compare(text, true, level, false, false, false, false, false, false, true);
  }
}
TEST_CASE("SSA combine covers every public opcode and original memory and loop corpus") {
  for (auto &descriptor : instruction_descriptors()) {
    CAPTURE(descriptor.name); auto source = test::public_instruction_example(descriptor); auto text = serialization::write_text(*source); REQUIRE(text);
    compare(*text, true, 2, false, false, true, true, true, true, true);
  }
  auto inputs = test::memory_fixtures(); auto loops = test::motion_fixtures(); auto graph = test::analysis_fixtures();
  inputs.insert(inputs.end(), loops.begin(), loops.end()); inputs.insert(inputs.end(), graph.begin(), graph.end());
  for (auto &text : inputs) { CAPTURE(text); compare(text, true, 2, false, false, true, true, true, true, true); }
  for (unsigned n = 1; n <= 16; ++n) {
    CAPTURE(n); std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"mir-tests"/("test"+std::to_string(n)+".mir")); REQUIRE(input);
    std::string text((std::istreambuf_iterator<char>(input)), {}); compare(text, true, 2, false, false, true, true, true, true, true);
  }
}
TEST_CASE("SSA combine preserves execution and stores at every original JIT level") {
  auto inputs = test::random_combine_fixtures(); auto memory = test::memory_fixtures(); inputs.insert(inputs.end(), memory.begin(), memory.end());
  for (auto &text : inputs) {
    CAPTURE(text); auto expected = execute(text, false, 0); auto c = serialization::parse_text(text); REQUIRE(c);
    FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>();
    passes.add<GVNPass>(); passes.add<CopyPropagationPass>(); passes.add<DeadStoresPass>(); passes.add<SSADeadCodePass>();
    passes.add<LICMPass>(); passes.add<PressureReliefPass>(); passes.add<ConventionalSSAPass>(); passes.add<SSACombinePass>(); passes.add<DestroySSAPass>();
    auto status = passes.run(**c); INFO((status ? "" : status.error().message)); REQUIRE(status); auto binary = serialization::write_binary(**c); REQUIRE(binary);
    CHECK(execute({}, false, 0, *binary) == expected);
    for (unsigned level : {0, 1, 2, 3}) { CAPTURE(level); CHECK(execute(text, true, level) == expected); CHECK(execute({}, true, level, *binary) == expected); }
  }
}
TEST_CASE("SSA combined stores preserve global bytes and NaN branches preserve unordered semantics") {
  std::vector<std::string> inputs;
  for (unsigned n = 0; n < 8; ++n)
    inputs.push_back("m: module\nstate: i64 7, 11, 13, 17, 19, 23, 29, 31\nentry: func i64\nlocal i64:p, i64:q, i64:x, i64:y\nmov p, state\nmov x, i64:0(p)\nadd q, p, "
      + std::to_string(n * 8) + "\nmul y, x, 3\nmov i64:0(q), y\nret 0\nendfunc\nendmodule\n");
  for (auto branch : {"bt", "bf"}) for (auto op : {"deq", "dne", "dlt", "dle", "dgt", "dge"})
    inputs.push_back("m: module\nentry: func i64\nlocal d:x, d:y, i64:c\ndmov x, +nan\ndmov y, 1.0\n" + std::string(op) + " c, x, y\n"
      + branch + " yes, c\nret 0\nyes:\nret 1\nendfunc\nendmodule\n");
  for (auto &text : inputs) {
    CAPTURE(text); std::vector<std::byte> before, after; auto expected = execute(text, false, 0, {}, &before);
    auto c = serialization::parse_text(text); REQUIRE(c); FunctionPassManager passes;
    passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>(); passes.add<ConventionalSSAPass>(); passes.add<SSACombinePass>(); passes.add<DestroySSAPass>();
    REQUIRE(passes.run(**c)); auto binary = serialization::write_binary(**c); REQUIRE(binary);
    CHECK(execute({}, false, 0, *binary, &after) == expected); CHECK(after == before);
    for (unsigned level : {0, 1, 2, 3}) {
      CAPTURE(level); CHECK(execute(text, true, level, {}, &after) == expected); CHECK(after == before);
      CHECK(execute({}, true, level, *binary, &after) == expected); CHECK(after == before);
    }
  }
}
TEST_CASE("original C combine rejects byte displacement candidates without dividing by zero") {
  for (auto displacement : {-1, 1, 255}) {
    auto text = "m: module\nentry: func i64, i64:p\nlocal i64:q, i64:r\nadd q, p, " + std::to_string(displacement)
      + "\nmov r, u8:0(q)\nret r\nendfunc\nendmodule\n";
    compare(text, true, 2, false, false, false, false, false, false, true);
    compare(text, true, 2, false, false, true, true, true, true, true);
  }
}
TEST_CASE("original C constant AND commutation updates inverse SSA edges before DCE") {
  for (auto op : {"and", "ands"}) {
    auto text = "m: module\nstate: i64 -12345\nentry: func i64\nlocal i64:p, i64:x, i64:a, i64:b\nmov p, state\nmov x, i64:0(p)\n"
      + std::string(op) + " a, 131071, x\nuext8 b, a\nret b\nendfunc\nendmodule\n";
    compare(text, true, 2, false, false, true, true);
    CHECK(execute(text, false, 0) == 199);
    for (unsigned level : {0, 1, 2, 3}) CHECK(execute(text, true, level) == 199);
  }
}
TEST_CASE("copy propagation and dead stores preserve interpreter and all JIT levels") {
  auto inputs = test::memory_fixtures(); auto random = test::random_memory_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &source : inputs) {
    CAPTURE(source); auto expected = execute(source, false, 0); auto parsed = serialization::parse_text(source); REQUIRE(parsed);
    FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>();
    passes.add<GVNPass>(); passes.add<CopyPropagationPass>(); passes.add<DeadStoresPass>(); passes.add<ConventionalSSAPass>(); passes.add<DestroySSAPass>();
    auto status = passes.run(**parsed); INFO((status ? "" : status.error().message)); REQUIRE(status);
    auto binary = serialization::write_binary(**parsed); REQUIRE(binary);
    CHECK(execute({}, false, 0, *binary) == expected);
    for (unsigned level : {0, 1, 2, 3}) { CAPTURE(level); CHECK(execute(source, true, level) == expected); CHECK(execute({}, true, level, *binary) == expected); }
  }
}
TEST_CASE("GVN retains original behavior across the MIR regression corpus") {
  for (unsigned n = 1; n <= 16; ++n) {
    CAPTURE(n); std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"mir-tests"/("test"+std::to_string(n)+".mir")); REQUIRE(input);
    std::string text((std::istreambuf_iterator<char>(input)), {}); compare(text, true, 2, false, false, true);
  }
}
TEST_CASE("GVN rules and fixed seed programs match independent original C stages") {
  auto inputs = test::gvn_rule_fixtures(); auto random = test::random_gvn_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &source : inputs) for (unsigned level : {2, 3}) { CAPTURE(level); compare(source, true, level, false, false, true); }
}
TEST_CASE("native GVN preserves interpreter and O0 through O3 JIT results") {
  auto inputs = test::gvn_fixtures(); auto rules = test::gvn_rule_fixtures(); auto random = test::random_gvn_fixtures();
  inputs.insert(inputs.end(), rules.begin(), rules.end()); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &source : inputs) {
    CAPTURE(source); auto parsed = serialization::parse_text(source); REQUIRE(parsed);
    if (!(*parsed)->modules().begin()->find_symbol("entry")->function()->arguments().empty()) continue;
    // The C interpreter also uses native ABI shims for internal calls. Its
    // Windows implementation cannot execute a multiple-result callee.
    const bool limited = test::check_native_result_limit(**parsed);
    auto expected = limited ? interpreted_result(**parsed) : execute(source, false, 0);
    if (limited) CHECK(expected == 43);
    FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>();
    passes.add<GVNPass>(); passes.add<ConventionalSSAPass>(); passes.add<DestroySSAPass>();
    auto status = passes.run(**parsed); INFO((status ? "" : status.error().message)); REQUIRE(status);
    auto text = serialization::write_text(**parsed); REQUIRE(text); auto binary = serialization::write_binary(**parsed); REQUIRE(binary);
    if (limited) {
      auto original = serialization::parse_text(source); REQUIRE(original);
      auto decoded = serialization::read_binary(*binary); REQUIRE(decoded);
      compare_limited_results(**original,**decoded,expected);
    } else {
      CHECK(execute(*text, false, 0, *binary) == expected);
      for (unsigned level : {0, 1, 2, 3}) { CAPTURE(level); CHECK(execute(source, true, level) == expected); CHECK(execute(*text, true, level, *binary) == expected); }
    }
  }
}
TEST_CASE("native GVN folding respects narrow negation without assuming unspecified high bits") {
  std::string source = "m: module\nentry: func i64\nlocal i64:a, i64:b\nmov a, 42\nnegs b, a\nret b\nendfunc\nendmodule\n";
  CHECK(static_cast<std::uint32_t>(execute(source, false, 0)) == static_cast<std::uint32_t>(-42));
  auto parsed = serialization::parse_text(source); REQUIRE(parsed);
  FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>();
  passes.add<GVNPass>(); passes.add<ConventionalSSAPass>(); passes.add<DestroySSAPass>(); REQUIRE(passes.run(**parsed));
  auto binary = serialization::write_binary(**parsed); REQUIRE(binary);
  for (unsigned level : {0, 1, 2, 3}) {
    CAPTURE(level); auto original = execute(source, true, level);
    CHECK(static_cast<std::uint32_t>(original) == static_cast<std::uint32_t>(-42));
    CHECK(execute({}, true, level, *binary) == -42);
  }
}
TEST_CASE("GVN handles every public opcode with the independent original C oracle") {
  for (auto &descriptor : instruction_descriptors()) {
    CAPTURE(descriptor.name); auto context = test::public_instruction_example(descriptor);
    if (descriptor.opcode == Opcode::VaStart || descriptor.opcode == Opcode::VaEnd || descriptor.opcode == Opcode::VaArg || descriptor.opcode == Opcode::VaBlockArg)
      for (auto &f : context->modules().begin()->functions()) for (auto &b : f.blocks()) for (auto &i : b.instructions())
        for (std::size_t n = 0; n < i.operands().size(); ++n) if (auto m = i.operands()[n].get<Memory>(); m && m->type.kind() == Type::Kind::Undef) {
          auto memory = *m; memory.displacement = 128; REQUIRE(IRRewriter(f).replace_operand(i, n, Operand(memory)));
        }
    auto text = serialization::write_text(*context); REQUIRE(text);
    compare(*text, true, 2, false, false, true); compare(*text, true, 2, false, false, true, true);
    compare(*text, true, 2, false, false, true, true, true, true);
  }
}
TEST_CASE("native hot block cloning preserves interpreter and O0 to O3 JIT results") {
  auto inputs = test::cloning_fixtures(); auto random = test::random_cloning_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  auto controls = test::cloning_control_fixtures(); inputs.insert(inputs.end(), controls.begin(), controls.end());
  for (auto &source : inputs) {
    CAPTURE(source); auto expected = execute(source, false, 0);
    bool exceptional_terminator = source.find("prbeq") != std::string::npos || source.find("prbne") != std::string::npos || source.find("jmpi") != std::string::npos;
    auto parsed = serialization::parse_text(source); REQUIRE(parsed);
    FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BlockCloningPass>();
    passes.add<BuildSSAPass>(); passes.add<ConventionalSSAPass>(); passes.add<DestroySSAPass>();
    auto status = passes.run(**parsed); INFO((status ? "" : status.error().message)); REQUIRE(status);
    auto text = serialization::write_text(**parsed); REQUIRE(text);
    auto binary = serialization::write_binary(**parsed); REQUIRE(binary);
    CHECK(execute(*text, false, 0, *binary) == expected);
    for (unsigned level : {0, 1, 2, 3}) {
      // C clone_bbs assumes the destination of a nonordinary branch has a
      // fallthrough label. Property branches assert there, and JMPI has no
      // such fallthrough. Native construction and all JIT levels still run.
      CAPTURE(level); if (!exceptional_terminator || level < 2) CHECK(execute(source, true, level) == expected);
      CHECK(execute(*text, true, level, *binary) == expected);
    }
  }
}
TEST_CASE("native address lowering preserves interpreter and all JIT optimization levels") {
  auto inputs = test::address_fixtures(); auto random = test::random_address_fixtures();
  inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &source : inputs) {
    auto parsed = serialization::parse_text(source); REQUIRE(parsed);
    auto entry = (*parsed)->modules().begin()->find_symbol("entry")->function();
    if (!entry->arguments().empty()) continue;
    const bool limited = test::check_native_result_limit(**parsed);
    auto expected = limited ? interpreted_result(**parsed) : execute(source, false, 0);
    FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>();
    passes.add<BuildSSAPass>(false); passes.add<AddressTransformPass>();
    passes.add<BuildSSAPass>(); passes.add<ConventionalSSAPass>(); passes.add<DestroySSAPass>();
    auto status = passes.run(**parsed); INFO((status ? "" : status.error().message)); REQUIRE(status);
    auto text = serialization::write_text(**parsed); REQUIRE(text);
    auto binary = serialization::write_binary(**parsed); REQUIRE(binary); REQUIRE(serialization::read_binary(*binary));
    if (limited) {
      auto original = serialization::parse_text(source); REQUIRE(original);
      auto decoded = serialization::read_binary(*binary); REQUIRE(decoded);
      compare_limited_results(**original,**decoded,expected);
    } else {
      CHECK(execute(*text, false, 0) == expected);
      for (unsigned level : {0, 1, 2, 3}) {
        CAPTURE(level); CAPTURE(source); CHECK(execute(source, true, level) == expected); CHECK(execute(*text, true, level) == expected);
      }
    }
  }
}
TEST_CASE("typed address memory consumers preserve bytes and block ABI with native lowering") {
  for (auto &source : test::address_memory_consumers()) {
    CAPTURE(source); auto expected = execute(source, false, 0);
    auto parsed = serialization::parse_text(source); REQUIRE(parsed);
    FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>();
    passes.add<BuildSSAPass>(false); passes.add<AddressTransformPass>();
    passes.add<BuildSSAPass>(); passes.add<ConventionalSSAPass>(); passes.add<DestroySSAPass>();
    REQUIRE(passes.run(**parsed)); auto text = serialization::write_text(**parsed); REQUIRE(text);
    auto binary = serialization::write_binary(**parsed); REQUIRE(binary); REQUIRE(serialization::read_binary(*binary));
    CHECK(execute(*text, false, 0, *binary) == expected);
    // Original C transform_addrs assumes all memory consumers are scalar moves
    // of the source's register class. O0/O1 do not run that unsafe conversion.
    auto host = target::TargetMachine::native(); REQUIRE(host);
    if (host->architecture() == target::Architecture::X86_64
        && (source.find("blk3:8") != std::string::npos || source.find("blk4:8") != std::string::npos)) {
      // Mixed integer/floating SysV classes need two eight-byte fields. The
      // interpreter can use these opaque blocks; the C JIT asserts on them.
      auto &read = *(*parsed)->modules().begin()->functions().begin();
      auto layout = target::layout_x86_arguments(host->abi(), read.signature().parameters);
      REQUIRE_FALSE(layout); CHECK(layout.error().message.find("invalid block classification") != std::string::npos);
    } else for (unsigned level : {0, 1}) { CAPTURE(level); CHECK(execute(source, true, level) == expected); CHECK(execute(*text, true, level, *binary) == expected); }
  }
}
TEST_CASE("native address lowering preserves stores into previously escaped pointer slots") {
  auto source = test::stored_pointer_address_fixture(); CHECK(execute(source, false, 0) == 42);
  auto probes = test::pointer_alias_address_fixtures(); probes.push_back(source); probes.push_back(test::stored_pointer_address_fixture(true));
  for (auto &probe : probes) {
  bool self = probe.find("addr p, p") != std::string::npos;
  auto legacy = MIR_init(); MIR_scan_string(legacy, probe.c_str()); MIR_gen_init(legacy);
  auto module = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(legacy)); REQUIRE(module);
  _MIR_opt_test_simplify_module(legacy, module); MIR_item_t entry = nullptr;
  for (auto item = DLIST_HEAD(MIR_item_t, module->items); item; item = DLIST_NEXT(MIR_item_t, item))
    if (item->item_type == MIR_func_item && std::string_view(MIR_item_name(legacy, item)) == "entry") entry = item;
  REQUIRE(entry);
  struct StoreObservation { MIR_reg_t pointer, source; bool present = false, before = false, after = false, rebuilt = false; } observation{
    MIR_reg(legacy, "p", entry->u.func), MIR_reg(legacy, self ? "p" : "v", entry->u.func)};
  auto observe = [](const char *stage, MIR_context_t context, MIR_item_t item, void *data) {
    auto &state = *static_cast<StoreObservation *>(data); state.present = false;
    _MIR_opt_test_visit_cfg(context, item, [](std::size_t, MIR_insn_t instruction, std::size_t, void *data) {
      auto &state = *static_cast<StoreObservation *>(data);
      if (instruction && instruction->code == MIR_ADDR && instruction->ops[0].mode == MIR_OP_VAR && instruction->ops[1].mode == MIR_OP_VAR)
        state.present |= _MIR_opt_test_var_register(instruction->ops[0].u.var) == state.pointer
          && _MIR_opt_test_var_register(instruction->ops[1].u.var) == state.source;
    }, data);
    if (std::string_view(stage) == "build-ssa") state.before = state.present;
    if (std::string_view(stage) == "transform-addresses") state.after = state.present;
    if (std::string_view(stage) == "rebuild-ssa") state.rebuilt = state.present;
  };
  REQUIRE(_MIR_opt_test_address_function(legacy, entry, 2, observe, &observation));
  CHECK(observation.before);
  CHECK(observation.after == (self || probe.find("sub r, p, p") != std::string::npos));
  CHECK_FALSE(observation.rebuilt);
  MIR_gen_finish(legacy); MIR_finish(legacy);
  }
  auto parsed = serialization::parse_text(source); REQUIRE(parsed);
  FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>(false);
  passes.add<AddressTransformPass>(); passes.add<BuildSSAPass>(); passes.add<ConventionalSSAPass>(); passes.add<DestroySSAPass>();
  auto result = passes.run(**parsed); INFO((result ? "" : result.error().message)); REQUIRE(result);
  auto text = serialization::write_text(**parsed); REQUIRE(text); auto binary = serialization::write_binary(**parsed); REQUIRE(binary);
  CHECK(execute(*text, false, 0, *binary) == 42);
  // C transform_addrs ignores an ADDR output whose pointer slot escaped
  // earlier. Its O2/O3 conversion erases the observable store. The interpreter
  // and O0/O1 preserve it. Native lowering makes the pointer store explicit,
  // so renaming and a subsequent C conversion retain it at every level.
  for (unsigned level : {0, 1, 2, 3}) {
    CAPTURE(level); if (level < 2) CHECK(execute(source, true, level) == 42);
    CHECK(execute(*text, true, level, *binary) == 42);
  }
}
TEST_CASE("native address lowering preserves pointer aliases with direct uses and self addresses") {
  auto inputs = test::pointer_alias_address_fixtures(); inputs.push_back(test::stored_pointer_address_fixture(true));
  for (auto &source : inputs) {
    auto expected = execute(source, false, 0); CAPTURE(source); CHECK(expected == (source.find("addr p, p") == std::string::npos ? 42 : 0));
    auto parsed = serialization::parse_text(source); REQUIRE(parsed);
    FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>(false);
    passes.add<AddressTransformPass>(); passes.add<BuildSSAPass>(); passes.add<ConventionalSSAPass>(); passes.add<DestroySSAPass>();
    auto result = passes.run(**parsed); INFO((result ? "" : result.error().message)); REQUIRE(result);
    auto text = serialization::write_text(**parsed); REQUIRE(text); auto binary = serialization::write_binary(**parsed); REQUIRE(binary);
    CHECK(execute(*text, false, 0, *binary) == expected);
    for (unsigned level : {0, 1, 2, 3}) {
      CAPTURE(level); if (level < 2) CHECK(execute(source, true, level) == expected);
      CHECK(execute(*text, true, level, *binary) == expected);
    }
  }
}
