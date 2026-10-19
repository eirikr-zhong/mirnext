/* Licensed under the MIT License. Independent C machine preparation oracle. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "COracle.h"
#include "mir-gen.h"
#include "mirnext/CodeGen/MachineIR.hpp"
#include "mirnext/CodeGen/Analysis.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/CodeGen/RegisterAllocation.hpp"
#include "mirnext/CodeGen/RegisterRewrite.hpp"
#include "mirnext/CodeGen/EdgeSpills.hpp"
#include "mirnext/CodeGen/PostAllocation.hpp"
#include "mirnext/CodeGen/Frame.hpp"
#include "mirnext/CodeGen/CompiledFunction.hpp"
#include "mirnext/CodeGen/BlockVersioning.hpp"
#include "mirnext/CodeGen/CompiledBlock.hpp"
#include "mirnext/Execution/detail/BBVRuntime.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "../IR/PublicInstructions.hpp"
#include "../Transforms/InlineFixtures.hpp"
#include "../Transforms/JumpFixtures.hpp"
#include "../Transforms/AddressFixtures.hpp"
#include "../CodeGen/AArch64Fixtures.hpp"
#include "../CodeGen/X86Fixtures.hpp"
#include "../CodeGen/EdgeSpillFixtures.hpp"
#include "../CodeGen/PostAllocationFixtures.hpp"
#include <bit>
#include <fstream>
#include <filesystem>
#include <iomanip>
#include <map>
#include <limits>
#include <sstream>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
// Bound declarations are assigned their fixed locations before priority
// allocation. C qsort leaves equal-priority bindings in an unspecified order;
// normalize only that prefix, preserving the decision order of free registers.
template<class Range, class Name, class Bound>
void allocation_order(std::ostream &out, const Range &range, Name name, Bound bound) {
  std::vector<std::string> fixed;
  for (auto r : range) if (bound(r)) fixed.push_back(name(r));
  std::sort(fixed.begin(), fixed.end());
  out << "allocation-order"; for (auto &r : fixed) out << ' ' << r;
  for (auto r : range) if (!bound(r)) out << ' ' << name(r);
  out << '\n';
}
template<class T> std::string bits(T value) {
  std::ostringstream out; out << std::hex << std::setfill('0');
  if constexpr (std::is_integral_v<T>) out << std::setw(sizeof(T) * 2) << static_cast<std::make_unsigned_t<T>>(value);
  else if constexpr (sizeof(T) == 4) out << std::setw(8) << std::bit_cast<std::uint32_t>(value);
  else if constexpr (sizeof(T) == 8) out << std::setw(16) << std::bit_cast<std::uint64_t>(value);
  else {
    constexpr auto bytes = std::numeric_limits<T>::digits == 64 ? 10 : sizeof(T);
    for (unsigned n = 0; n < bytes; ++n) out << std::setw(2) << static_cast<unsigned>(reinterpret_cast<const unsigned char *>(&value)[n]);
  }
  return out.str();
}
int type_code(Type t) {
  switch (t.kind()) {
  case Type::Kind::I8: return MIR_T_I8; case Type::Kind::U8: return MIR_T_U8;
  case Type::Kind::I16: return MIR_T_I16; case Type::Kind::U16: return MIR_T_U16;
  case Type::Kind::I32: return MIR_T_I32; case Type::Kind::U32: return MIR_T_U32;
  case Type::Kind::U64: return MIR_T_U64; case Type::Kind::F32: return MIR_T_F; case Type::Kind::F64: return MIR_T_D;
  case Type::Kind::LongDouble: return MIR_T_LD; case Type::Kind::Pointer: return MIR_T_P;
  case Type::Kind::Block: return MIR_T_BLK + static_cast<int>(t.block_class()); case Type::Kind::ReturnBlock: return MIR_T_RBLK;
  case Type::Kind::Undef: return MIR_T_UNDEF; default: return MIR_T_I64;
  }
}
struct Snapshot {
  std::vector<std::vector<std::string>> bodies;
  std::vector<std::vector<std::size_t>> incoming, outgoing;
  std::vector<std::string> analysis;
  std::map<std::string, long> frequencies;
  std::vector<std::array<std::vector<std::string>, 4>> live;
  std::vector<std::array<int, 2>> pressure;
  std::vector<std::array<std::vector<std::string>, 3>> effects;
  std::string str() const {
    std::ostringstream out;
    for (std::size_t b = 0; b < bodies.size(); ++b) {
      out << "bb" << b << '\n'; for (auto &i : bodies[b]) out << i << '\n';
      out << "in:"; for (auto v : incoming[b]) out << ' ' << v;
      out << " out:"; for (auto v : outgoing[b]) out << ' ' << v; out << '\n';
    }
    for (auto &a : analysis) out << a << '\n';
    for (auto &[r, n] : frequencies) if (n) out << "frequency " << r << ':' << n << '\n';
    for (std::size_t b = 0; b < live.size(); ++b) {
      for (std::size_t k = 0; k < 4; ++k) {
        auto values = live[b][k]; std::sort(values.begin(), values.end());
        out << "live " << b << ':' << k; for (auto &r : values) out << ' ' << r; out << '\n';
      }
      out << "pressure " << b << ':' << pressure[b][0] << ':' << pressure[b][1] << '\n';
    }
    for (std::size_t i = 0; i < effects.size(); ++i) for (std::size_t k = 0; k < 3; ++k) {
      out << "effect " << i << ':' << k; for (auto &r : effects[i][k]) out << ' ' << r; out << '\n';
    }
    return out.str();
  }
};
Snapshot native(const MachineFunction &f, unsigned level) {
  auto cfg = MachineControlFlow::build(f); REQUIRE(cfg); Snapshot s;
  s.bodies.resize(cfg->blocks.size()); s.incoming.resize(s.bodies.size()); s.outgoing.resize(s.bodies.size());
  std::map<const MachineVirtualRegister *, std::size_t> regs;
  auto reg = [&](const MachineVirtualRegister *r) {
    if (!r) return std::string("0"); auto [at, fresh] = regs.emplace(r, regs.size() + 1);
    auto bind = r->binding(); return std::to_string(at->second) + ':' + std::to_string(type_code(r->type())) + ':' + (bind ? std::string(f.target().register_info(*bind)->name) : "");
  };
  for (auto r : f.arguments()) reg(r);
  std::map<const MachineBasicBlock *, std::size_t> blocks;
  for (std::size_t n = 2; n < s.bodies.size(); ++n) blocks.emplace(cfg->blocks[n], n);
  auto operand = [&](const MachineOperand &o) {
    if (auto r = o.get<const MachineVirtualRegister *>()) return "r" + reg(*r);
    if (auto l = o.get<const MachineLabel *>()) return "l" + std::to_string(blocks.at(&(*l)->block()));
    if (auto v = o.get<std::int64_t>()) return "i" + bits(*v);
    if (auto v = o.get<std::uint64_t>()) return "u" + bits(*v);
    if (auto v = o.get<float>()) return "f" + bits(*v);
    if (auto v = o.get<double>()) return "d" + bits(*v);
    if (auto v = o.get<long double>()) return "e" + bits(*v);
    if (auto v = o.get<MachineSymbolReference>()) return "s" + v->symbol->module + '/' + v->symbol->name + ':' + std::to_string(v->displacement);
    auto m = o.get<MachineMemory>(); REQUIRE(m);
    auto base = m->base ? reg(std::get<const MachineVirtualRegister *>(*m->base)) : "0";
    auto index = m->index ? reg(std::get<const MachineVirtualRegister *>(*m->index)) : "0";
    return "m" + std::to_string(type_code(m->type)) + ':' + std::to_string(m->type.is_block() ? m->type.block_size() : m->displacement)
      + ':' + base + ':' + index + ':' + std::to_string(m->index ? m->scale : 1) + ':' + m->alias + ':' + m->nonalias;
  };
  for (std::size_t b = 2; b < s.bodies.size(); ++b) for (auto &i : cfg->blocks[b]->instructions()) {
    auto line = std::string(opcode_name(i.opcode()));
    for (std::size_t p = 0; p < i.operands().size(); ++p) {
      if (!p && (line == "call" || line == "inline" || line == "jcall")) {
        const auto &sig = *i.operands()[0].get<MachineSymbolReference>()->symbol->signature;
        line += " signature:" + std::to_string(sig.variadic);
        for (auto t : sig.results) line += ":r" + std::to_string(type_code(t));
        for (auto t : sig.parameters) line += ":a" + std::to_string(type_code(t)) + ':' + std::to_string(t.block_size());
      } else line += ' ' + operand(i.operands()[p]);
    } s.bodies[b].push_back(std::move(line));
    CHECK(i.call_arguments().empty());
  }
  for (std::size_t b = 0; b < s.bodies.size(); ++b) {
    for (auto e : cfg->outgoing[b]) s.outgoing[b].push_back(cfg->edges[e].destination);
    for (auto e : cfg->incoming[b]) s.incoming[b].push_back(cfg->edges[e].source);
  }
  auto graph = MachineGraphAnalysis::build(f); REQUIRE(graph);
  for (std::size_t b = 0; b < s.bodies.size(); ++b) {
    s.analysis.push_back("pre " + std::to_string(b) + ':' + std::to_string(graph->preorder()[b]));
    s.analysis.push_back("rpost " + std::to_string(b) + ':' + std::to_string(graph->reverse_postorder()[b]));
  }
  for (std::size_t n = 0; n < graph->loops().size(); ++n) {
    const auto &loop = graph->loops()[n]; auto text = "loop " + std::to_string(n) + ':' + std::to_string(loop.block)
      + ':' + std::to_string(loop.entry) + ':' + std::to_string(loop.parent);
    for (auto child : loop.children) text += ':' + std::to_string(child);
    s.analysis.push_back(std::move(text));
  }
  auto key = [&](MachineRegister r) {
    if (auto p = std::get_if<target::PhysicalRegister>(&r)) return "h" + std::to_string(p->encoding());
    return "r" + reg(std::get<const MachineVirtualRegister *>(r));
  };
  auto live = MachineLiveness::analyze(f, *graph, {.optimization_level = level}); REQUIRE(live);
  for (auto r : live->registers()) { auto n = live->frequency(r); REQUIRE(n); if (*n) s.frequencies[key(r)] = *n; }
  s.live.resize(live->blocks().size()); s.pressure.resize(s.live.size());
  for (std::size_t b = 0; b < s.live.size(); ++b) {
    const auto &bb = live->blocks()[b]; const SparseBitSet *sets[] = {&bb.in, &bb.out, &bb.gen, &bb.kill};
    for (unsigned k = 0; k < 4; ++k) sets[k]->for_each([&](auto p) { s.live[b][k].push_back(key(live->registers()[p])); });
    s.pressure[b] = {bb.max_integer_pressure, bb.max_floating_pressure};
  }
  for (auto &block : f.blocks()) for (auto &insn : block.instructions()) {
    auto effects = machine_register_effects(f, insn); REQUIRE(effects); s.effects.push_back({}); auto &e = s.effects.back();
    for (auto r : effects->definitions) e[0].push_back(key(r));
    for (auto r : effects->uses) e[1].push_back(key(r));
    if (level) { auto dead = live->deaths(insn); REQUIRE(dead); for (auto r : *dead) e[2].push_back(key(r)); }
  }
  return s;
}
struct CReference {
  MIR_context_t context; MIR_item_t item; Snapshot snapshot;
  std::vector<std::vector<MIR_insn_t>> bodies;
  std::map<MIR_insn_t, std::size_t> labels;
  std::map<MIR_reg_t, std::size_t> registers;
  std::string reg(MIR_reg_t var) {
    auto r = _MIR_opt_test_var_register(var); if (!r) return "0";
    auto [at, fresh] = registers.emplace(r, registers.size() + 1); auto binding = MIR_reg_hard_reg_name(context, r, item->u.func);
    return std::to_string(at->second) + ':' + std::to_string(MIR_reg_type(context, r, item->u.func)) + ':' + (binding ? binding : "");
  }
  std::string operand(MIR_op_t o) {
    switch (o.mode) {
    case MIR_OP_VAR: return "r" + reg(o.u.var); case MIR_OP_LABEL: return "l" + std::to_string(labels.at(o.u.label));
    case MIR_OP_INT: return "i" + bits(o.u.i); case MIR_OP_UINT: return "u" + bits(o.u.u);
    case MIR_OP_FLOAT: return "f" + bits(o.u.f); case MIR_OP_DOUBLE: return "d" + bits(o.u.d); case MIR_OP_LDOUBLE: return "e" + bits(o.u.ld);
    case MIR_OP_REF: return "s" + std::string(o.u.ref->module->name) + '/' + MIR_item_name(context, o.u.ref) + ":0";
    case MIR_OP_VAR_MEM: {
      auto &m = o.u.var_mem; auto base = reg(m.base), index = reg(m.index);
      return "m" + std::to_string(m.type) + ':' + std::to_string(m.disp) + ':' + base + ':' + index + ':' + std::to_string(index == "0" ? 1 : m.scale)
        + ':' + (m.alias ? MIR_alias_name(context, m.alias) : "") + ':' + (m.nonalias ? MIR_alias_name(context, m.nonalias) : "");
    }
    default: FAIL("unexpected operand mode ", static_cast<unsigned>(o.mode)); return "invalid";
    }
  }
  static void cfg(std::size_t b, MIR_insn_t insn, std::size_t to, void *data) {
    auto &self = *static_cast<CReference *>(data);
    if (self.bodies.size() <= b) self.bodies.resize(b + 1);
    if (self.snapshot.outgoing.size() <= b) self.snapshot.outgoing.resize(b + 1);
    if (insn) { self.bodies[b].push_back(insn); if (insn->code == MIR_LABEL) self.labels.emplace(insn, b); }
    else if (to != SIZE_MAX) self.snapshot.outgoing[b].push_back(to);
  }
  static void observe(MIR_context_t context, MIR_item_t item, void *data) {
    auto &self = *static_cast<CReference *>(data); self.context = context; self.item = item;
    _MIR_opt_test_visit_cfg(context, item, cfg, data); auto &s = self.snapshot; s.bodies.resize(self.bodies.size()); s.incoming.resize(s.bodies.size());
    // Incoming edges are reported by the original CFG visitor in their actual
    // insertion order; sorting would hide an observable preparation difference.
    struct Incoming {
      static void visit(unsigned kind, std::size_t b, std::size_t from, void *data) {
        if (kind == 0) static_cast<CReference *>(data)->snapshot.incoming[b].push_back(from);
      }
    };
    _MIR_opt_test_visit_jump_state(context, item, Incoming::visit, data);
    for (std::size_t p = 0; p < item->u.func->nargs; ++p) {
      auto r = MIR_reg(context, VARR_GET(MIR_var_t, item->u.func->vars, p).name, item->u.func);
      self.registers.emplace(r, self.registers.size() + 1);
    }
    for (std::size_t b = 0; b < self.bodies.size(); ++b) for (auto i : self.bodies[b]) if (i->code != MIR_LABEL) {
      auto line = std::string(MIR_insn_name(context, i->code));
      for (std::size_t p = 0; p < i->nops; ++p) {
        if (!p && MIR_call_code_p(i->code)) {
          auto sig = i->ops[0].u.ref->u.proto; line += " signature:" + std::to_string(sig->vararg_p);
          for (std::size_t r = 0; r < sig->nres; ++r) line += ":r" + std::to_string(sig->res_types[r]);
          for (std::size_t a = 0; a < VARR_LENGTH(MIR_var_t, sig->args); ++a) {
            auto arg = VARR_GET(MIR_var_t, sig->args, a); line += ":a" + std::to_string(arg.type) + ':' + std::to_string(MIR_all_blk_type_p(arg.type) ? arg.size : 0);
          }
        } else line += ' ' + self.operand(i->ops[p]);
      } s.bodies[b].push_back(std::move(line));
    }
    struct Graph {
      std::vector<std::size_t> pre, rpost;
      std::map<std::size_t, MachineLoopNode> loops;
      static void visit(unsigned kind, std::size_t a, std::size_t b, void *data) {
        auto &g = *static_cast<Graph *>(data);
        if (kind <= 1) {
          auto &v = kind ? g.rpost : g.pre; if (v.size() <= a) v.resize(a + 1); v[a] = b;
        } else if (kind == 8) g.loops[a].block = b;
        else if (kind == 9) g.loops[a].entry = b;
        else if (kind == 10) g.loops[a].parent = b;
        else if (kind == 11) g.loops[a].children.push_back(b);
      }
    } graph;
    _MIR_opt_test_visit_analysis(context, Graph::visit, &graph);
    for (std::size_t b = 0; b < graph.pre.size(); ++b) {
      s.analysis.push_back("pre " + std::to_string(b) + ':' + std::to_string(graph.pre[b]));
      s.analysis.push_back("rpost " + std::to_string(b) + ':' + std::to_string(graph.rpost[b]));
    }
    for (auto &[n, loop] : graph.loops) {
      auto text = "loop " + std::to_string(n) + ':' + std::to_string(loop.block) + ':' + std::to_string(loop.entry) + ':' + std::to_string(loop.parent);
      for (auto child : loop.children) text += ':' + std::to_string(child);
      s.analysis.push_back(std::move(text));
    }
    struct Live {
      CReference &self; std::map<MIR_insn_t, std::size_t> instructions; std::size_t physical_count;
      static void visit(unsigned kind, std::size_t b, MIR_insn_t insn, MIR_reg_t r, long n, void *data) {
        auto &probe = *static_cast<Live *>(data); auto &self = probe.self; auto &s = self.snapshot;
        if (kind == 5 || kind == 6) { s.pressure[b][kind - 5] = static_cast<int>(n); return; }
        if (!kind && !n) return;
        auto key = r < probe.physical_count ? "h" + std::to_string(r) : "r" + self.reg(r);
        if (!kind) { if (n) s.frequencies[key] = n; }
        else if (kind <= 4) s.live[b][kind - 1].push_back(key);
        else if (kind <= 9 && insn->code != MIR_LABEL) {
          auto at = probe.instructions.find(insn); REQUIRE(at != probe.instructions.end());
          s.effects[at->second][kind - 7].push_back(key);
        }
      }
    } live{self, {}, target::TargetMachine::native()->registers().size()};
    s.live.resize(s.bodies.size()); s.pressure.resize(s.bodies.size());
    for (auto insn = DLIST_HEAD(MIR_insn_t, item->u.func->insns); insn; insn = DLIST_NEXT(MIR_insn_t, insn)) if (insn->code != MIR_LABEL) {
      live.instructions.emplace(insn, s.effects.size()); s.effects.push_back({});
    }
    _MIR_opt_test_visit_machine_liveness(context, item, Live::visit, &live);
  }
};
std::string builtin_name(Builtin id) {
  switch (id) {
  case Builtin::CopyBlock: return "mir.blk_mov"; case Builtin::CopyArgument: return "mir.arg_memcpy";
  case Builtin::UnsignedToFloat: return "mir.ui2f"; case Builtin::UnsignedToDouble: return "mir.ui2d";
  case Builtin::VariadicScalar: return "mir.va_arg"; case Builtin::VariadicBlock: return "mir.va_block_arg";
  case Builtin::FloatToLongDouble: return "mir.f2ld"; case Builtin::DoubleToLongDouble: return "mir.d2ld";
  case Builtin::IntegerToLongDouble: return "mir.i2ld"; case Builtin::UnsignedToLongDouble: return "mir.ui2ld";
  case Builtin::LongDoubleToFloat: return "mir.ld2f"; case Builtin::LongDoubleToDouble: return "mir.ld2d"; case Builtin::LongDoubleToInteger: return "mir.ld2i";
  case Builtin::LongDoubleAdd: return "mir.ldadd"; case Builtin::LongDoubleSub: return "mir.ldsub"; case Builtin::LongDoubleMul: return "mir.ldmul"; case Builtin::LongDoubleDiv: return "mir.lddiv";
  case Builtin::LongDoubleNeg: return "mir.ldneg"; case Builtin::LongDoubleEq: return "mir.ldeq"; case Builtin::LongDoubleNe: return "mir.ldne";
  case Builtin::LongDoubleLt: return "mir.ldlt"; case Builtin::LongDoubleLe: return "mir.ldle"; case Builtin::LongDoubleGt: return "mir.ldgt"; case Builtin::LongDoubleGe: return "mir.ldge";
  default: FAIL("unexpected builtin"); return {};
  }
}
void record_cfg(std::ostream &out,const std::vector<std::vector<std::size_t>> &outgoing,
    const std::vector<std::vector<std::size_t>> &incoming,const std::vector<bool> &alias = {}) {
  // Co-located aliases are one owning block. The C CFG may represent an
  // earlier label as an empty fallthrough node; collapse only that identity,
  // preserving both edge multiplicity and incoming/outgoing order.
  std::vector<std::size_t> redirected(outgoing.size()),ordinals(outgoing.size()); std::size_t count = 0;
  for (std::size_t n = outgoing.size(); n-- > 0;) redirected[n] = !alias.empty() && alias[n] && outgoing[n].size() == 1 && outgoing[n][0] == n+1 ? redirected[n+1] : n;
  for (std::size_t n = 0; n < outgoing.size(); ++n) if (redirected[n] == n) ordinals[n] = count++;
  for (std::size_t n = 0; n < outgoing.size(); ++n) if (redirected[n] == n) {
    out << "cfg " << ordinals[n] << ':'; for (auto to : outgoing[n]) out << ' ' << ordinals[redirected[to]]; out << '\n';
    out << "incoming " << ordinals[n] << ':';
    auto append = [&](auto &&self,std::size_t from) -> void {
      if (redirected[from] == from) out << ' ' << ordinals[from]; else for (auto predecessor : incoming[from]) self(self,predecessor);
    };
    for (auto from : incoming[n]) append(append,from); out << '\n';
  }
}
std::string lowered_native(const MachineFunction &f, const MachineLiveRanges *ranges = nullptr, bool raw = false,
    const MachineAllocation *allocation = nullptr, const MachineRewriteResult *rewrite = nullptr, const MachineControlFlow *topology = nullptr,bool normalize_aliases = false) {
  std::ostringstream out; std::map<const MachineVirtualRegister *, std::size_t> regs;
  auto reg = [&](MachineRegister r) {
    if (auto h = std::get_if<target::PhysicalRegister>(&r)) return "h" + std::to_string(h->encoding());
    auto v = std::get<const MachineVirtualRegister *>(r); auto [at, fresh] = regs.emplace(v, regs.size() + 1);
    return "v" + std::to_string(at->second) + ':' + std::to_string(type_code(v->type())) + ':' + (v->binding() ? std::string(f.target().register_info(*v->binding())->name) : "");
  };
  for (auto a : f.arguments()) reg(a);
  std::map<const MachineLabel *, std::size_t> labels; std::size_t position = 0;
  for (auto &b : f.blocks()) { for (auto l : b.labels()) labels.emplace(l, position); position += std::distance(b.instructions().begin(), b.instructions().end()); }
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) {
    out << opcode_name(i.opcode());
    for (std::size_t p = 0; p < i.operands().size(); ++p) {
      auto &o = i.operands()[p]; out << ' ';
      if (!p && i.has_lowered_call()) {
        auto &s = *o.get<MachineSymbolReference>()->symbol->signature; out << "signature:" << s.variadic;
        for (auto t : s.results) out << ":r" << type_code(t);
        for (auto t : s.parameters) out << ":a" << type_code(t) << ':' << t.block_size();
      } else if (auto r = o.get<const MachineVirtualRegister *>()) out << reg(*r);
      else if (auto h = o.get<target::PhysicalRegister>()) out << reg(*h);
      else if (auto l = o.get<const MachineLabel *>()) out << 'l' << labels.at(*l);
      else if (auto n = o.get<std::int64_t>()) out << 'i' << bits(*n);
      else if (auto n = o.get<std::uint64_t>()) out << 'u' << bits(*n);
      else if (auto n = o.get<float>()) out << 'f' << bits(*n);
      else if (auto n = o.get<double>()) out << 'd' << bits(*n);
      else if (auto n = o.get<long double>()) out << 'e' << bits(*n);
      else if (auto s = o.get<MachineSymbolReference>()) out << 's' << s->symbol->module << '/' << s->symbol->name << ':' << s->displacement;
      else if (auto b = o.get<BuiltinReference>()) out << "builtin:" << builtin_name(b->builtin);
      else {
        auto m = o.get<MachineMemory>(); REQUIRE(m);
        auto base = m->base ? reg(*m->base) : "none", index = m->index ? reg(*m->index) : "none";
        for (auto d : i.call_dependencies()) if (d.operand == p) base = reg(d.value);
        out << 'm' << type_code(m->type) << ':' << m->displacement << ':' << base << ':' << index << ':' << (m->index ? m->scale : 1) << ':' << m->alias << ':' << m->nonalias;
      }
    }
    for (auto r : i.call_arguments()) out << " call-reg:" << r.encoding();
    auto matched = has_instruction_pattern(i); REQUIRE(matched); out << " pattern:" << *matched;
    out << '\n';
  }
  out << "state " << f.state().uses_alloca << ' ' << f.state().has_block_arguments << ' ' << f.state().is_leaf;
  if (auto arm = std::get_if<MachineFunctionState::AArch64>(&f.state().target)) out << ' ' << arm->aggregate_save_bytes;
  else out << ' ' << f.state().requires_frame_pointer << ' ' << std::get<MachineFunctionState::X86>(f.state().target).initial_sp_from_bp;
  out << '\n';
  auto cfg = MachineControlFlow::build(f); REQUIRE(cfg);
  if (topology) cfg = *topology;
  std::vector<std::vector<std::size_t>> outgoing(cfg->blocks.size()),incoming(cfg->blocks.size()); std::vector<bool> aliases(cfg->blocks.size());
  for (std::size_t b = 0; b < cfg->blocks.size(); ++b) {
    for (auto e : cfg->outgoing[b]) outgoing[b].push_back(cfg->edges[e].destination);
    for (auto e : cfg->incoming[b]) incoming[b].push_back(cfg->edges[e].source);
    auto block = cfg->blocks[b];
    aliases[b] = normalize_aliases && block && !block->labels().empty() && block->instructions().begin() == block->instructions().end()
      && b+1 < cfg->blocks.size() && cfg->blocks[b+1] && !cfg->blocks[b+1]->labels().empty();
  }
  record_cfg(out,outgoing,incoming,aliases);
  if (ranges) {
    out << "points " << (raw ? ranges->uncompressed_point_count() : ranges->point_count()) << '\n'; std::map<std::string, std::string> values;
    auto all = raw ? ranges->uncompressed_ranges() : ranges->all_ranges();
    for (std::size_t r = 0; r < ranges->registers().size(); ++r) if (!all[r].empty()) {
      std::ostringstream item;
      for (auto &range : all[r]) {
        item << ' ' << range.start << ':' << range.finish << ':' << range.reference_cost;
        for (auto block : range.blocks) item << 'b' << std::distance(cfg->blocks.begin(), std::find(cfg->blocks.begin(), cfg->blocks.end(), block));
      }
      values.emplace(reg(ranges->registers()[r]), item.str());
    }
    for (auto &[name, value] : values) out << "range " << name << value << '\n';
    if (!raw) { out << "point-map"; for (auto p : ranges->point_map()) out << ' ' << p; out << '\n'; }
    std::vector<std::string> crossed; ranges->call_crossed().for_each([&](auto r) { crossed.push_back(reg(ranges->registers()[r])); }); std::sort(crossed.begin(), crossed.end());
    out << "crossed"; for (auto &r : crossed) out << ' ' << r; out << '\n';
    auto spills = allocation ? allocation->spill_blocks() : ranges->spill_blocks();
    for (std::size_t b = 0; b < spills.size(); ++b) for (unsigned kind = 0; kind < 2; ++kind) {
      std::vector<std::string> bits; auto &spill = spills[b];
      (kind ? spill.kill : spill.gen).for_each([&](auto r) { bits.push_back(reg(ranges->scan_registers()[r])); }); std::sort(bits.begin(), bits.end());
      out << "spill " << b << ':' << kind; for (auto &r : bits) out << ' ' << r; out << '\n';
    }
  }
  if (allocation) {
    out << "allocation " << allocation->stack_bytes() / 8 << ':' << allocation->reusable_stack_offset() / 8 << '\n';
    std::map<std::string, std::string> values;
    for (auto &r : f.registers()) {
      auto loc = allocation->location(r); REQUIRE(loc); auto length = allocation->live_length(r); REQUIRE(length);
      std::string value = "none";
      if (auto p = std::get_if<target::PhysicalRegister>(&*loc)) value = "h" + std::to_string(p->encoding());
      else if (auto s = std::get_if<StackLocation>(&*loc)) value = "s" + std::to_string(s->offset / 8);
      // Empty unused declarations can be absent from C's normalized reader.
      if (value != "none" || *length) values.emplace(reg(&r), value + ':' + std::to_string(*length));
    }
    for (auto &[name, value] : values) out << "location " << name << ':' << value << '\n';
    allocation_order(out, allocation->order(), reg, [](auto r) { return r->binding().has_value(); });
    for (unsigned busy = 0; busy < 2; ++busy) {
      auto points = busy ? allocation->busy() : allocation->used();
      for (std::size_t p = 0; p < points.size(); ++p) {
        out << "occupied " << p << ':' << busy;
        points[p].physical.for_each([&](auto r) { out << " h" << r; }); points[p].stack.for_each([&](auto s) { out << " s" << s; }); out << '\n';
      }
    }
    out << "allocated-hardware"; for (auto r : allocation->used_registers()) out << ' ' << r.encoding(); out << '\n';
    for (auto &g : allocation->gaps()) {
      auto at = std::find(ranges->registers().begin(), ranges->registers().end(), MachineRegister(g.reg)); REQUIRE(at != ranges->registers().end());
      auto &all = ranges->all_ranges()[std::distance(ranges->registers().begin(), at)]; auto it = std::find(all.begin(), all.end(), g.range); REQUIRE(it != all.end());
      out << "gap " << g.physical.encoding() << ':' << reg(g.reg) << ':' << std::distance(all.begin(), it) << '\n';
    }
  }
  if (rewrite) {
    std::map<std::string, std::size_t> slots;
    for (auto &r : f.registers()) { auto slot = rewrite->spill_slot(r); REQUIRE(slot); if (*slot) slots.emplace(reg(&r), (**slot).offset / 8); }
    for (auto &[r, slot] : slots) out << "preservation " << r << ':' << slot << '\n';
  }
  return out.str();
}
std::string post_facts(const MachinePostView &view) {
  std::ostringstream out;
  const auto &f = view.function(); std::map<const MachineVirtualRegister *,std::size_t> names;
  auto name = [&](MachineRegister r) {
    if (auto p = std::get_if<target::PhysicalRegister>(&r)) return "h" + std::to_string(p->encoding());
    auto v = std::get<const MachineVirtualRegister *>(r); auto [at,fresh] = names.emplace(v,names.size() + 1);
    return "v" + std::to_string(at->second) + ':' + std::to_string(type_code(v->type())) + ':' + (v->binding() ? std::string(f.target().register_info(*v->binding())->name) : "");
  };
  for (auto a : f.arguments()) name(a);
  std::vector<MachineRegister> registers; for (auto &r : f.target().registers()) registers.emplace_back(r.reg); for (auto &r : f.registers()) registers.emplace_back(&r);
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) for (std::size_t p = 0; p < i.operands().size(); ++p) {
    auto &o = i.operands()[p];
    if (auto r = o.get<const MachineVirtualRegister *>()) name(*r);
    else if (auto m = o.get<MachineMemory>()) { if (m->base) name(*m->base); if (m->index) name(*m->index); }
    for (auto d : i.call_dependencies()) if (d.operand == p) name(d.value);
  }
  for (std::size_t b = 0; b < view.boundary_liveness().size(); ++b) {
    auto &bb = view.boundary_liveness()[b]; std::array<const SparseBitSet *,4> sets{&bb.in,&bb.out,&bb.gen,&bb.kill};
    for (unsigned k = 0; k < 4; ++k) { out << "post-live " << b << ':' << k; std::vector<std::string> values;
      sets[k]->for_each([&](auto r) { values.push_back(name(registers.at(r))); }); std::sort(values.begin(),values.end()); for (auto &r : values) out << ' ' << r; out << '\n'; }
  }
  std::size_t n = 0;
  for (auto &b : view.function().blocks()) for (auto &i : b.instructions()) {
    std::vector<std::string> dead; for (auto r : view.deaths(i)) dead.push_back(name(r)); std::sort(dead.begin(),dead.end());
    out << "post-dead " << n++; for (auto &r : dead) out << ' ' << r; out << '\n';
  }
  return out.str();
}
struct FrameFacts {
  std::size_t slots = 0, aggregate = 0;
  bool leaf = true, alloca = false, block = false;
  std::optional<bool> frame_pointer;
};
// Independent flat-list partition, following create_bb_stubs rather than the
// C++ basic-block containers. Full operands and function state are compared by
// the frame snapshot below; this record adds exact BBV boundaries and targets.
std::string reference_bbv_layout(MIR_context_t ctx,MIR_item_t item) {
  std::vector<std::vector<MIR_insn_t>> blocks;
  std::map<MIR_insn_t,std::size_t> labels;
  bool boundary = true;
  for (auto i = DLIST_HEAD(MIR_insn_t,item->u.func->insns); i; i = DLIST_NEXT(MIR_insn_t,i)) {
    if (i->code == MIR_LABEL || boundary) {
      blocks.emplace_back();
      if (i->code == MIR_LABEL) {
        labels.emplace(i,blocks.size()-1);
        while (auto next = DLIST_NEXT(MIR_insn_t,i)) { if (next->code != MIR_LABEL) break; i = next; labels.emplace(i,blocks.size()-1); }
      }
    }
    if (i->code != MIR_LABEL) blocks.back().push_back(i);
    boundary = MIR_any_branch_code_p(i->code) || i->code == MIR_RET || i->code == MIR_JRET || i->code == MIR_PRBEQ || i->code == MIR_PRBNE;
  }
  std::ostringstream out;
  for (std::size_t n = 0; n < blocks.size(); ++n) {
    out << "bb" << n << '\n';
    for (auto i : blocks[n]) {
      if (i->code == MIR_UNSPEC && i->nops == 3 && i->ops[0].mode == MIR_OP_INT && i->ops[0].u.i == 0) out << "x86-mov128";
      else out << MIR_insn_name(ctx,i->code);
      for (std::size_t p = 0; p < i->nops; ++p) if (i->ops[p].mode == MIR_OP_LABEL) out << ' ' << p << ":bb" << labels.at(i->ops[p].u.label);
      out << '\n';
    }
    auto last = blocks[n].empty() ? MIR_USE : blocks[n].back()->code;
    if (last != MIR_JMP && last != MIR_JMPI && last != MIR_SWITCH && last != MIR_RET && last != MIR_JRET && n+1 < blocks.size()) out << "fallthrough bb" << n+1 << '\n';
  }
  return out.str();
}
std::string native_bbv_layout(const BBVPreparation &p) {
  std::ostringstream out;
  for (auto &block : p.blocks()) {
    out << "bb" << block.index() << '\n';
    for (auto i : block.instructions()) {
      auto op = i->opcode(); bool vector = op == MachineOpcode(MachinePseudoOpcode::X86Mov128) || op == MachineOpcode(MachinePseudoOpcode::X86Mov128Unaligned);
      out << (vector ? "x86-mov128" : opcode_name(op));
      for (std::size_t n = 0; n < i->operands().size(); ++n) if (auto label = i->operands()[n].get<const MachineLabel *>()) {
        auto target = p.block_for(**label); REQUIRE(target); out << ' ' << n << ":bb" << (*target)->index();
      }
      out << '\n';
    }
    for (auto &e : block.successors()) if (e.kind == BBVSuccessor::Kind::Fallthrough) out << "fallthrough bb" << e.block->index() << '\n';
  }
  return out.str();
}
struct LoweredReference {
  std::string snapshot;
  std::string raw_snapshot;
  std::string post_live_snapshot;
  std::string frame_input_snapshot;
  bool bbv = false;
  std::string bbv_snapshot;
  const FrameFacts *frame_facts = nullptr;
  bool encoding = false;
  std::vector<std::uint8_t> encoded;
  std::vector<std::uint64_t> pool_values;
  std::vector<MIR_reference_encoded_constant> constants;
  std::vector<std::size_t> absolute_offsets, label_offsets;
  std::set<MIR_insn_t> aligned_labels;
  std::vector<std::size_t> aligned_offsets;
  std::set<MIR_insn_t> used_encoding_labels;
  bool ranges = false, raw = false, assignment = false, rewrite = false, post = false;
  unsigned level = 0;
  int points = 0;
  static void capture_post_live(MIR_context_t ctx, const char *stage, MIR_item_t item, FILE *, void *data) {
    if (std::string_view(stage) != "post-allocation-live-test") return;
    auto &self = *static_cast<LoweredReference *>(data); observe(ctx,item,data); self.post_live_snapshot = std::move(self.snapshot);
  }
  static void capture_frame_input(MIR_context_t ctx, const char *stage, MIR_item_t item, FILE *, void *data) {
    if (std::string_view(stage) == "input-encoding-test") {
      auto &self = *static_cast<LoweredReference *>(data);
      self.encoding = false; observe(ctx,item,data); self.encoding = true;
      _MIR_reference_visit_encoding_labels(ctx,item,+[](void *p,MIR_insn_t label,int aligned) {
        if (aligned) static_cast<LoweredReference *>(p)->aligned_labels.insert(label);
      },&self);
      // Both encoders see an identical explicit symbol environment. It uses
      // four nonzero chunks so relocatable and resolved instruction sizes agree.
      for (auto i = DLIST_HEAD(MIR_insn_t,item->u.func->insns); i; i = DLIST_NEXT(MIR_insn_t,i)) {
        if (i->code == MIR_MOV && i->ops[1].mode == MIR_OP_REF) i->ops[1] = MIR_new_uint_op(ctx,0x123456789abcdef0ull);
        if ((i->code == MIR_CALL || i->code == MIR_INLINE || i->code == MIR_JCALL) && i->nops >= 2 && i->ops[1].mode == MIR_OP_REF)
          i->ops[1] = MIR_new_uint_op(ctx,0x123456789abcdef0ull);
      }
      return;
    }
    if (std::string_view(stage) != "input-frame-test") return;
    auto &self = *static_cast<LoweredReference *>(data);
    if (self.frame_facts) {
      MIR_reference_machine_state state; _MIR_reference_get_machine_state(ctx,&state); auto &facts = *self.frame_facts;
      state.is_leaf = facts.leaf; state.uses_alloca = facts.alloca; state.has_block_arguments = facts.block; state.aggregate_save_bytes = facts.aggregate;
      if (facts.frame_pointer) state.requires_fp = *facts.frame_pointer;
      _MIR_reference_set_machine_state(ctx,&state);
      struct Facts { std::vector<std::uint32_t> used; static void slots(void *,std::size_t) {} static void reg(void *p,std::uint32_t r) { static_cast<Facts *>(p)->used.push_back(r); } } saved;
      _MIR_reference_visit_frame(ctx,Facts::slots,Facts::reg,&saved);
      _MIR_opt_test_set_frame_facts(ctx,facts.slots,saved.used.data(),saved.used.size());
    }
    if (!self.encoding) { observe(ctx,item,data); self.frame_input_snapshot = std::move(self.snapshot); }
  }
  static void capture_raw(MIR_context_t ctx, const char *stage, MIR_item_t item, FILE *, void *data) {
    if (std::string_view(stage) != "raw-live-ranges") return;
    auto &self = *static_cast<LoweredReference *>(data); self.raw = true; observe(ctx, item, data);
    self.raw_snapshot = std::move(self.snapshot); self.raw = false;
  }
  static void observe(MIR_context_t ctx, MIR_item_t item, void *data) {
    auto &self = *static_cast<LoweredReference *>(data); std::ostringstream out;
    if (self.encoding) {
      for (auto i = DLIST_HEAD(MIR_insn_t,item->u.func->insns); i; i = DLIST_NEXT(MIR_insn_t,i))
        for (std::size_t n = 0; n < i->nops; ++n) if (i->ops[n].mode == MIR_OP_LABEL) self.used_encoding_labels.insert(i->ops[n].u.label);
      for (auto r = item->u.func->first_lref; r; r = r->next) { self.used_encoding_labels.insert(r->label); if (r->label2) self.used_encoding_labels.insert(r->label2); }
      MIR_reference_encoded_visitor visitor{
        +[](void *p,const std::uint8_t *bytes,std::size_t size) { auto &s = *static_cast<LoweredReference *>(p); s.encoded.assign(bytes,bytes+size); },
        +[](void *p,std::size_t at) { static_cast<LoweredReference *>(p)->absolute_offsets.push_back(at); },
        +[](void *p,std::uint64_t value) { static_cast<LoweredReference *>(p)->pool_values.push_back(value); },
        +[](void *p,const MIR_reference_encoded_constant *c) { static_cast<LoweredReference *>(p)->constants.push_back(*c); },
        +[](void *p,MIR_insn_t label,std::size_t at) { auto &s = *static_cast<LoweredReference *>(p); if (s.used_encoding_labels.contains(label)) {
          s.label_offsets.push_back(at); if (s.aligned_labels.contains(label)) s.aligned_offsets.push_back(at);
        } }};
      _MIR_opt_test_visit_encoding(ctx,item,&visitor,&self); return;
    }
    struct Topology {
      std::map<std::size_t, std::size_t> order;
      std::map<std::size_t, std::pair<std::size_t, std::size_t>> edges;
      std::map<std::size_t, std::vector<std::size_t>> incoming;
      static void begin(void *, std::size_t, std::size_t) {}
      static void edge(void *p, std::size_t e, std::size_t s, std::size_t d, int, int, int, int) { static_cast<Topology *>(p)->edges.emplace(e, std::pair{s,d}); }
      static void block(void *p, std::size_t b, int out, std::size_t e) { auto &s = *static_cast<Topology *>(p); s.order.try_emplace(b, s.order.size()); if (!out) s.incoming[b].push_back(e); }
      static void cache(void *, unsigned, unsigned) {}
    } topology;
    MIR_reference_spill_graph_visitor topology_visitor{Topology::begin, Topology::edge, Topology::block, Topology::cache};
    _MIR_reference_visit_spill_graph(ctx, &topology_visitor, &topology);
    std::map<MIR_reg_t, std::size_t> regs; auto target = target::TargetMachine::native(); REQUIRE(target);
    auto reg = [&](MIR_reg_t r) {
      if (r == MIR_NON_VAR) return std::string("none");
      if (r < target->registers().size()) return "h" + std::to_string(r);
      auto v = _MIR_opt_test_var_register(r); REQUIRE(v); auto [at, fresh] = regs.emplace(v, regs.size() + 1);
      auto binding = MIR_reg_hard_reg_name(ctx, v, item->u.func);
      return "v" + std::to_string(at->second) + ':' + std::to_string(MIR_reg_type(ctx, v, item->u.func)) + ':' + (binding ? binding : "");
    };
    for (std::size_t n = 0; n < item->u.func->nargs; ++n) regs.emplace(MIR_reg(ctx, VARR_GET(MIR_var_t, item->u.func->vars, n).name, item->u.func), regs.size() + 1);
    std::map<MIR_insn_t, std::size_t> labels; std::size_t position = 0;
    struct CallRegisters {
      static void visit(MIR_insn_t i, std::uint32_t r, void *p) { (*static_cast<std::map<MIR_insn_t, std::vector<unsigned>> *>(p))[i].push_back(r); }
    };
    std::map<MIR_insn_t, std::vector<unsigned>> call_registers;
    _MIR_opt_test_visit_machine_calls(ctx, item, CallRegisters::visit, &call_registers);
    for (auto i = DLIST_HEAD(MIR_insn_t, item->u.func->insns); i; i = DLIST_NEXT(MIR_insn_t, i)) { if (i->code == MIR_LABEL) labels.emplace(i, position); else ++position; }
    for (auto i = DLIST_HEAD(MIR_insn_t, item->u.func->insns); i; i = DLIST_NEXT(MIR_insn_t, i)) if (i->code != MIR_LABEL) {
      const bool vector_save = target->architecture() == target::Architecture::X86_64 && i->code == MIR_UNSPEC;
      if (vector_save) { REQUIRE(i->nops == 3); REQUIRE(i->ops[0].mode == MIR_OP_INT); REQUIRE(i->ops[0].u.i == 0); }
      out << (vector_save ? "x86-mov128" : MIR_insn_name(ctx, i->code));
      for (std::size_t p = vector_save ? 1 : 0; p < i->nops; ++p) {
        auto o = i->ops[p]; out << ' ';
        if (!p && MIR_call_code_p(i->code)) {
          auto s = o.u.ref->u.proto; out << "signature:" << unsigned(s->vararg_p);
          for (std::size_t r = 0; r < s->nres; ++r) out << ":r" << s->res_types[r];
          for (std::size_t a = 0; a < VARR_LENGTH(MIR_var_t, s->args); ++a) { auto v = VARR_GET(MIR_var_t, s->args, a); out << ":a" << v.type << ':' << (MIR_all_blk_type_p(v.type) ? v.size : 0); }
        } else switch (o.mode) {
        case MIR_OP_VAR: out << reg(o.u.var); break;
        case MIR_OP_LABEL: out << 'l' << labels.at(o.u.label); break;
        case MIR_OP_INT: out << 'i' << bits(o.u.i); break; case MIR_OP_UINT: out << 'u' << bits(o.u.u); break;
        case MIR_OP_FLOAT: out << 'f' << bits(o.u.f); break; case MIR_OP_DOUBLE: out << 'd' << bits(o.u.d); break; case MIR_OP_LDOUBLE: out << 'e' << bits(o.u.ld); break;
        case MIR_OP_REF: {
          auto name = std::string_view(MIR_item_name(ctx, o.u.ref));
          if (name.starts_with("mir.") && o.u.ref->item_type == MIR_import_item && o.u.ref->addr) out << "builtin:" << name;
          else out << 's' << o.u.ref->module->name << '/' << name << ":0";
          break;
        }
        case MIR_OP_VAR_MEM: {
          auto m = o.u.var_mem; auto base = reg(m.base), index = reg(m.index);
          // Owning x86 call locations use undef address markers; block size
          // and ABI class remain in the typed call signature.
          auto type = target->architecture() == target::Architecture::X86_64 && MIR_call_code_p(i->code) && MIR_blk_type_p(m.type) ? MIR_T_UNDEF : m.type;
          out << 'm' << type << ':' << m.disp << ':' << base << ':' << index << ':' << (m.index != MIR_NON_VAR ? unsigned(m.scale) : 1)
            << ':' << (m.alias ? MIR_alias_name(ctx, m.alias) : "") << ':' << (m.nonalias ? MIR_alias_name(ctx, m.nonalias) : ""); break;
        }
        default: FAIL("unexpected lowered operand");
        }
      }
      for (auto r : call_registers[i]) out << " call-reg:" << r;
      out << " pattern:" << _MIR_opt_test_instruction_pattern(ctx, i);
      out << '\n';
    }
    struct State { static void visit(unsigned, std::int64_t value, void *p) { *static_cast<std::ostringstream *>(p) << ' ' << value; } };
    out << "state"; _MIR_opt_test_visit_lowering_state(ctx, State::visit, &out); out << '\n';
    struct CFG {
      std::vector<std::vector<std::size_t>> edges;
      std::vector<MIR_insn_t> last;
      std::vector<bool> executable;
      static void visit(std::size_t b, MIR_insn_t insn, std::size_t to, void *p) {
        auto &self = *static_cast<CFG *>(p); if (self.edges.size() <= b) self.edges.resize(b + 1);
        self.last.resize(self.edges.size()); self.executable.resize(self.edges.size());
        if (insn) { self.last[b] = insn; self.executable[b] = self.executable[b] || insn->code != MIR_LABEL; }
        if (!insn && to != SIZE_MAX) self.edges[b].push_back(to);
      }
    } cfg;
    _MIR_opt_test_visit_cfg(ctx, item, CFG::visit, &cfg);
    std::map<std::size_t, std::vector<std::size_t>> incoming;
    for (auto &[id, ordinal] : topology.order) for (auto e : topology.incoming[id]) incoming[ordinal].push_back(topology.order.at(topology.edges.at(e).first));
    std::vector<std::vector<std::size_t>> incoming_edges(cfg.edges.size()); std::vector<bool> aliases(cfg.edges.size());
    for (std::size_t b = 0; b < cfg.edges.size(); ++b) {
      incoming_edges[b] = incoming[b]; auto last = cfg.last[b]; auto next = last ? DLIST_NEXT(MIR_insn_t,last) : nullptr;
      aliases[b] = self.bbv && last && !cfg.executable[b] && next && next->code == MIR_LABEL;
    }
    record_cfg(out,cfg.edges,incoming_edges,aliases);
    if (self.ranges) {
      struct Ranges {
        const std::map<std::size_t, std::size_t> *ordinals;
        int points = 0; MIR_reg_t last = 0; std::vector<int> map;
        std::map<MIR_reg_t, std::string> items;
        std::vector<MIR_reg_t> crossed;
        std::map<std::pair<std::size_t, int>, std::vector<MIR_reg_t>> spill;
        static void begin(void *p, std::size_t, int points) { static_cast<Ranges *>(p)->points = points; }
        static void range(void *p, std::uint32_t r, int start, int finish, int cost) { auto &s = *static_cast<Ranges *>(p); s.last = r; s.items[r] += " " + std::to_string(start) + ':' + std::to_string(finish) + ':' + std::to_string(cost); }
        static void block(void *p, std::size_t b) { auto &s = *static_cast<Ranges *>(p); s.items[s.last] += 'b' + std::to_string(s.ordinals->at(b)); }
        static void point(void *p, int n) { static_cast<Ranges *>(p)->map.push_back(n); }
        static void call(void *p, std::uint32_t r) { static_cast<Ranges *>(p)->crossed.push_back(r); }
        static void bit(void *p, std::size_t b, int kind, std::uint32_t r) { auto &s = *static_cast<Ranges *>(p); s.spill[{s.ordinals->at(b), kind}].push_back(r); }
      } ranges{&topology.order};
      MIR_reference_ranges_visitor v{Ranges::begin, Ranges::range, Ranges::block, Ranges::point, Ranges::call, Ranges::bit};
      _MIR_opt_test_visit_live_ranges(ctx, item, &v, &ranges); out << "points " << ranges.points << '\n';
      self.points = ranges.points;
      std::map<std::string, std::string> values; for (auto &[r, text] : ranges.items) values.emplace(reg(r), text);
      for (auto &[name, value] : values) out << "range " << name << value << '\n';
      if (!self.raw) { out << "point-map"; for (auto p : ranges.map) out << ' ' << p; out << '\n'; }
      std::vector<std::string> crossed; for (auto r : ranges.crossed) crossed.push_back(reg(r)); std::sort(crossed.begin(), crossed.end());
      out << "crossed"; for (auto &r : crossed) out << ' ' << r; out << '\n';
      for (std::size_t b = 0; b < cfg.edges.size(); ++b) for (int kind = 0; kind < 2; ++kind) {
        std::vector<std::string> bits; for (auto r : ranges.spill[{b, kind}]) bits.push_back(reg(r)); std::sort(bits.begin(), bits.end());
        out << "spill " << b << ':' << kind; for (auto &r : bits) out << ' ' << r; out << '\n';
      }
    }
    if (self.assignment && !self.raw) {
      struct Assignment {
        std::size_t slots = 0; unsigned start = 0;
        std::map<unsigned, std::pair<unsigned, std::size_t>> locations;
        std::vector<unsigned> order, used;
        std::map<std::pair<std::size_t, int>, std::vector<unsigned>> points;
        std::vector<std::tuple<unsigned, unsigned, std::size_t>> gaps;
        static void begin(void *p, std::size_t slots, unsigned start) { auto &s = *static_cast<Assignment *>(p); s.slots = slots; s.start = start; }
        static void location(void *p, unsigned r, unsigned l, std::size_t length) { static_cast<Assignment *>(p)->locations.emplace(r, std::pair{l, length}); }
        static void ordered(void *p, unsigned r) { static_cast<Assignment *>(p)->order.push_back(r); }
        static void point(void *p, std::size_t n, int busy, unsigned loc) { static_cast<Assignment *>(p)->points[{n, busy}].push_back(loc); }
        static void hardware(void *p, unsigned r) { static_cast<Assignment *>(p)->used.push_back(r); }
        static void gap(void *p, unsigned h, unsigned r, std::size_t n) { static_cast<Assignment *>(p)->gaps.emplace_back(h, r, n); }
      } assigned;
      MIR_reference_assignment_visitor v{Assignment::begin, Assignment::location, Assignment::ordered, Assignment::point, Assignment::hardware, Assignment::gap};
      _MIR_opt_test_visit_assignment(ctx, item, &v, &assigned);
      auto physical = target->registers().size();
      auto location = [&](unsigned loc) { return loc == MIR_NON_VAR ? std::string("none") : loc < physical ? "h" + std::to_string(loc) : "s" + std::to_string(loc - physical); };
      out << "allocation " << assigned.slots << ':' << assigned.start - physical << '\n';
      std::map<std::string, std::string> values;
      for (auto &[r, value] : assigned.locations) if (r >= physical && (value.first != MIR_NON_VAR || value.second)) values.emplace(reg(r), location(value.first) + ':' + std::to_string(value.second));
      for (auto &[name, value] : values) out << "location " << name << ':' << value << '\n';
      allocation_order(out, assigned.order, reg, [&](auto r) { return MIR_reg_hard_reg_name(ctx, _MIR_opt_test_var_register(r), item->u.func) != nullptr; });
      for (unsigned busy = 0; busy < 2; ++busy) {
        if (busy && self.level < 2) continue;
        for (int p = 0; p <= self.points; ++p) { out << "occupied " << p << ':' << busy; for (auto r : assigned.points[{p, busy}]) out << ' ' << location(r); out << '\n'; }
      }
      out << "allocated-hardware"; for (auto r : assigned.used) out << ' ' << r; out << '\n';
      for (auto [h, r, n] : assigned.gaps) out << "gap " << h << ':' << reg(r) << ':' << n << '\n';
    }
    if (self.rewrite && !self.raw) {
      struct Slots { std::map<unsigned, unsigned> values; static void visit(void *p, unsigned r, unsigned slot) { static_cast<Slots *>(p)->values.emplace(r, slot); } } slots;
      _MIR_opt_test_visit_spill_cache(ctx, Slots::visit, &slots);
      std::map<std::string, unsigned> sorted;
      for (auto [r, slot] : slots.values) sorted.emplace(reg(r), slot - target->registers().size());
      for (auto &[r, slot] : sorted) out << "preservation " << r << ':' << slot << '\n';
    }
    if (self.post && self.level) {
      struct Post {
        const std::map<std::size_t,std::size_t> &ordinals;
        std::map<std::pair<std::size_t,unsigned>,std::vector<unsigned>> bits;
        std::map<MIR_insn_t,std::vector<unsigned>> deaths;
        MIR_insn_t current = nullptr;
        static void begin(void *,std::size_t,unsigned) {}
        static void reg(void *,std::uint32_t,int,long,int) {}
        static void single(void *,std::uint32_t) {}
        static void block(void *,std::size_t,std::size_t,unsigned,int,int) {}
        static void edge(void *,std::size_t,std::size_t) {}
        static void instruction(void *p,MIR_insn_t i,std::size_t,std::size_t) { static_cast<Post *>(p)->current = i; }
        static void operand(void *,std::uint32_t,int) {}
        static void bit(void *p,std::size_t b,unsigned k,std::uint32_t r) { auto &s = *static_cast<Post *>(p); s.bits[{s.ordinals.at(b),k}].push_back(r); }
        static void dead(void *p,std::uint32_t r) { auto &s = *static_cast<Post *>(p); s.deaths[s.current].push_back(r); }
      } post{topology.order};
      MIR_reference_live_visitor visitor{Post::begin,Post::reg,Post::single,Post::single,Post::block,Post::edge,Post::instruction,Post::operand,Post::single,Post::bit,Post::dead};
      _MIR_reference_visit_liveness(ctx,item,&visitor,&post);
      for (std::size_t b = 0; b < cfg.edges.size(); ++b) for (unsigned k = 0; k < 4; ++k) { out << "post-live " << b << ':' << k;
        std::vector<std::string> values; for (auto r : post.bits[{b,k}]) values.push_back(reg(r)); std::sort(values.begin(),values.end()); for (auto &r : values) out << ' ' << r; out << '\n'; }
      std::size_t n = 0;
      for (auto i = DLIST_HEAD(MIR_insn_t,item->u.func->insns); i; i = DLIST_NEXT(MIR_insn_t,i)) if (i->code != MIR_LABEL) {
        std::vector<std::string> dead; for (auto r : post.deaths[i]) dead.push_back(reg(r)); std::sort(dead.begin(),dead.end()); out << "post-dead " << n++; for (auto &r : dead) out << ' ' << r; out << '\n';
      }
    }
    self.snapshot = out.str();
    if (self.bbv) self.bbv_snapshot = reference_bbv_layout(ctx,item);
  }
};
std::size_t compared_edge_placements = 0, compared_split_blocks = 0;
std::size_t compared_post_substitutions = 0, compared_post_extensions = 0, compared_post_deletions = 0;
std::size_t compared_frames = 0, compared_frame_saves = 0;
std::size_t compared_encodings = 0, compared_code_bytes = 0, compared_code_relocations = 0;
void compare(const Context &source, unsigned level, bool lower = false, bool legalize = false, bool coalescing = false, bool ranges = false, bool assignment = false,
    bool run_optimization = true, bool rewrite = false, bool split = false, unsigned post = 0, bool remove_properties = true,
    bool preserve_host = false, const FrameFacts *frame_facts = nullptr,bool bbv = false) {
  transforms::OptimizationOptions options; options.level = static_cast<transforms::OptimizationLevel>(level);
  auto optimized = transforms::optimize(source, options); INFO((optimized ? "" : optimized.error().message)); REQUIRE(optimized);
  const auto &machine_source = run_optimization ? optimized->context() : source;
  auto text = serialization::write_text(machine_source); REQUIRE(text); auto c = MIR_init();
  // The C scanner canonicalizes an L-suffixed value to double on these hosts,
  // but forgets to canonicalize its data directive. Adjust only that directive
  // in the test loader; instructions, local types and constant bits are still
  // compared after the same native target canonicalization on both sides.
  if constexpr (sizeof(long double) == sizeof(double)) {
    for (std::size_t line = 0; line < text->size();) {
      auto end = text->find('\n', line); if (end == std::string::npos) end = text->size();
      auto view = std::string_view(*text).substr(line, end - line); auto at = view.find(": ld ");
      if (at != view.npos) { text->erase(line + at + 2, 1); --end; }
      else if (view.starts_with("\tld ")) { text->erase(line + 1, 1); --end; }
      line = end + 1;
    }
  }
  if (std::getenv("MIRNEXT_MACHINE_IR_REFERENCE_TRACE")) std::fprintf(stderr, "level %u\n%s\n", level, text->c_str());
  if (text->find("nan") != std::string::npos || text->find("inf") != std::string::npos) {
    auto binary = serialization::write_binary(machine_source); REQUIRE(binary); auto file = std::tmpfile(); REQUIRE(file);
    REQUIRE(std::fwrite(binary->data(), 1, binary->size(), file) == binary->size()); std::rewind(file); MIR_read(c, file); std::fclose(file);
  } else MIR_scan_string(c, text->c_str());
  // Parsing alone does not attach lref data to C functions. Loading normally
  // does so, but also simplifies this already optimized input a second time.
  // Prepare exactly the label environment, with no executable/runtime state.
  for (auto m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(c)); m; m = DLIST_NEXT(MIR_module_t, m)) _MIR_opt_test_prepare_module_lrefs(c, m);
  MIR_gen_init(c);
  auto target = target::TargetMachine::native(); REQUIRE(target);
  for (auto &module : machine_source.modules()) for (auto &function : module.functions()) {
    MIR_item_t item = nullptr;
    for (auto m = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(c)); m; m = DLIST_NEXT(MIR_module_t, m)) if (module.name() == m->name)
      for (auto i = DLIST_HEAD(MIR_item_t, m->items); i; i = DLIST_NEXT(MIR_item_t, i)) if (i->item_type == MIR_func_item && function.symbol().name() == MIR_item_name(c, i)) item = i;
    REQUIRE(item); CReference reference{}; LoweredReference lowered;
    lowered.ranges = ranges && !post;
    lowered.assignment = assignment && !post;
    lowered.rewrite = rewrite && !post;
    lowered.post = post && post < 3;
    lowered.frame_facts = frame_facts;
    lowered.level = level;
    lowered.bbv = bbv;
    if (post >= 3) MIR_opt_set_test_observer(c,LoweredReference::capture_frame_input,&lowered);
    else if (post && level) MIR_opt_set_test_observer(c,LoweredReference::capture_post_live,&lowered);
    else if (ranges && !post) MIR_opt_set_test_observer(c, LoweredReference::capture_raw, &lowered);
    lowered.encoding = post == 4;
    if (post) REQUIRE(_MIR_opt_test_allocation_function(c,item,level,(post >= 3 ? post + 5 : post == 1 ? 6 : 7) + (remove_properties ? 0 : 16) + (preserve_host ? 32 : 0),LoweredReference::observe,&lowered));
    else if (split) REQUIRE(_MIR_opt_test_allocation_function(c, item, level, 4, LoweredReference::observe, &lowered));
    else if (rewrite) REQUIRE(_MIR_opt_test_rewrite_function(c, item, level, LoweredReference::observe, &lowered));
    else if (assignment) REQUIRE(_MIR_opt_test_assign_function(c, item, level, LoweredReference::observe, &lowered));
    else if (coalescing) REQUIRE(_MIR_opt_test_allocation_function(c, item, level, ranges, LoweredReference::observe, &lowered));
    else if (legalize) REQUIRE(_MIR_opt_test_legalize_function(c, item, level, LoweredReference::observe, &lowered));
    else if (lower) REQUIRE(_MIR_opt_test_machinize_function(c, item, level, LoweredReference::observe, &lowered));
    else REQUIRE(_MIR_opt_test_analyze_function(c, item, level, CReference::observe, &reference));
    if (ranges) MIR_opt_set_test_observer(c, nullptr, nullptr);
    auto machine = prepare_machine_ir(function, *target); INFO((machine ? "" : machine.error().message)); REQUIRE(machine);
    if (lower) { auto result = machinize(**machine); INFO((result ? "" : result.error().message)); REQUIRE(result); }
    if (legalize) { auto result = legalize_operands(**machine); INFO((result ? "" : result.error().message)); REQUIRE(result); }
    if (frame_facts) {
      MachineRewriter rw(**machine); auto state = (*machine)->state(); state.is_leaf = frame_facts->leaf; state.uses_alloca = frame_facts->alloca; state.has_block_arguments = frame_facts->block;
      if (auto arm = std::get_if<MachineFunctionState::AArch64>(&state.target)) arm->aggregate_save_bytes = frame_facts->aggregate;
      if (frame_facts->frame_pointer) state.requires_frame_pointer = *frame_facts->frame_pointer;
      REQUIRE(rw.set_state(state));
      for (std::size_t n = 0; n < frame_facts->slots; ++n) REQUIRE((*machine)->create_register(Type::i64(),{},false,true));
    }
    if (coalescing) { auto graph = MachineGraphAnalysis::build(**machine); REQUIRE(graph); auto result = coalesce_registers(**machine, *graph, level); INFO((result ? "" : result.error().message)); REQUIRE(result); }
    std::optional<MachineLiveRanges> native_ranges;
    std::optional<MachineAllocation> native_allocation;
    std::optional<MachineRewriteResult> native_rewrite;
    std::optional<MachineSpillResult> native_split;
    std::optional<MachinePostResult> native_post;
    std::optional<MachineFrameResult> native_frame;
    std::string post_snapshot;
    std::string post_live_snapshot;
    std::string raw_actual;
    if (ranges) { auto graph = MachineGraphAnalysis::build(**machine); REQUIRE(graph); auto live = MachineLiveness::analyze(**machine, *graph, {.optimization_level = level}); REQUIRE(live);
      auto result = MachineLiveRanges::build(**machine, *graph, *live); INFO((result ? "" : result.error().message)); REQUIRE(result); native_ranges.emplace(std::move(*result));
      raw_actual = lowered_native(**machine, &*native_ranges, true);
      if (assignment) { auto a = MachineAllocation::assign(**machine, *graph, *live, *native_ranges); INFO((a ? "" : a.error().message)); REQUIRE(a); native_allocation.emplace(std::move(*a)); }
      if (rewrite) { auto r = rewrite_allocated(**machine, *graph, *live, *native_ranges, std::move(*native_allocation)); INFO((r ? "" : r.error().message)); REQUIRE(r); native_allocation.reset(); native_rewrite.emplace(std::move(*r)); }
      if (split) { auto count = native_rewrite->blocks().size(); auto r = split_allocated_spills(**machine, std::move(*native_rewrite)); INFO((r ? "" : r.error().message)); REQUIRE(r);
        compared_edge_placements += r->placements().size(); compared_split_blocks += r->cfg().blocks.size() - count;
        native_rewrite.reset(); native_split.emplace(std::move(*r)); }
    }
    if (post) {
      auto input_cfg = native_split->cfg();
      auto r = optimize_allocated_machine(**machine,std::move(*native_split),{.remove_properties=remove_properties,.observer=[&](MachinePostStage stage,const MachinePostView &view) -> Result<void> {
        if (stage == MachinePostStage::Liveness) post_live_snapshot = lowered_native(view.function(),nullptr,false,nullptr,nullptr,&view.input_cfg()) + post_facts(view);
        if (stage == (post == 1 ? MachinePostStage::Combine : MachinePostStage::DeadCode))
          post_snapshot = lowered_native(view.function(),nullptr,false,nullptr,nullptr,&view.input_cfg()) + post_facts(view);
        return {};
      }}); INFO((r ? "" : r.error().message)); REQUIRE(r); native_split.reset(); native_post.emplace(std::move(*r)); REQUIRE(native_post->cfg().verify(**machine));
      compared_post_substitutions += native_post->statistics().substitutions;
      compared_post_extensions += native_post->statistics().extensions;
      compared_post_deletions += native_post->statistics().dead_instructions;
      if (!level) post_snapshot = lowered_native(**machine,nullptr,false,nullptr,nullptr,&input_cfg);
      if (post >= 3) {
        auto before = lowered_native(**machine,nullptr,false,nullptr,nullptr,&input_cfg,bbv);
        if (post == 3) { INFO("C frame input:\n",lowered.frame_input_snapshot,"\nC++ frame input:\n",before); CHECK(before == lowered.frame_input_snapshot); }
        auto framed = form_frame(**machine,std::move(*native_post),{.preserve_host_registers=preserve_host}); INFO((framed ? "" : framed.error().message)); REQUIRE(framed);
        native_post.reset(); native_frame.emplace(std::move(*framed)); REQUIRE(native_frame->cfg().verify(**machine));
        compared_frames += native_frame->layout().frame_bytes != 0;
        compared_frame_saves += native_frame->layout().callee_saves.size();
        post_snapshot = lowered_native(**machine,nullptr,false,nullptr,nullptr,&input_cfg,bbv);
        if (bbv) {
          REQUIRE_FALSE(remove_properties); REQUIRE(post == 3); REQUIRE_FALSE(frame_facts);
          auto p = BBVPreparation::prepare(function,*target,{.optimization_level=level,.preserve_host_registers=preserve_host}); INFO((p ? "" : p.error().message)); REQUIRE(p);
          CHECK(p->function().dump() == (*machine)->dump()); auto layout = native_bbv_layout(*p);
          INFO("C BBV layout:\n",lowered.bbv_snapshot,"\nC++ BBV layout:\n",layout); CHECK(layout == lowered.bbv_snapshot);
        }
        if (post == 4) {
          auto encoded = encode_machine(**machine,*native_frame,{.known_addresses=[](auto &) -> Result<std::uint64_t> { return 0x123456789abcdef0ull; }});
          INFO((encoded ? "" : encoded.error().message)); REQUIRE(encoded);
          std::size_t unaligned_moves = 0;
          for (auto &b : (*machine)->blocks()) for (auto &i : b.instructions()) unaligned_moves += i.opcode() == MachineOpcode(MachinePseudoOpcode::X86Mov128Unaligned);
          auto compare_bytes = [&](std::span<const std::uint8_t> bytes) {
            REQUIRE(bytes.size() == lowered.encoded.size()); std::size_t corrected = 0;
            for (std::size_t n = 0; n < bytes.size(); ++n) if (bytes[n] != lowered.encoded[n]) {
              CAPTURE(n); REQUIRE(unaligned_moves != 0); CHECK(bytes[n] == 0xf3); CHECK(lowered.encoded[n] == 0x66);
              auto op = n+1; if (op < bytes.size() && bytes[op] >= 0x40 && bytes[op] <= 0x4f) ++op;
              REQUIRE(op+1 < bytes.size()); CHECK(bytes[op] == 0x0f); CHECK((bytes[op+1] == 0x6f || bytes[op+1] == 0x7f)); ++corrected;
            }
            CHECK(corrected == unaligned_moves);
          };
          compare_bytes(encoded->bytes());
          REQUIRE(encoded->constant_pool().size() == lowered.constants.size());
          for (std::size_t n = 0; n < lowered.constants.size(); ++n) {
            auto &a = encoded->constant_pool()[n]; auto &b = lowered.constants[n];
            CHECK(a.call == bool(b.call)); CHECK(a.reference_offset == b.pc); CHECK(a.next_instruction == b.next_instruction);
            REQUIRE(b.constant < lowered.pool_values.size()); std::uint64_t value = 0;
            for (unsigned k = 0; k < 8; ++k) value |= std::uint64_t(encoded->bytes()[a.offset+k]) << (8*k);
            CHECK(value == lowered.pool_values[b.constant]);
          }
          std::vector<std::size_t> absolute, labels;
          for (auto &r : encoded->relocations()) if (r.kind == CodeRelocation::Kind::LocalAddress) absolute.push_back(r.offset);
          std::set<const MachineLabel *> used;
          for (auto &b : (*machine)->blocks()) for (auto &i : b.instructions()) for (auto &o : i.operands()) if (auto l = o.get<const MachineLabel *>()) used.insert(*l);
          std::size_t ordinal = 0;
          std::map<const MachineLabel *,std::size_t> label_offsets;
          for (auto &l : (*machine)->labels()) { auto offset = encoded->labels()[ordinal++].offset; label_offsets.emplace(&l,offset); if (l.address_taken() || used.contains(&l)) labels.push_back(offset); }
          std::sort(labels.begin(),labels.end()); std::sort(lowered.label_offsets.begin(),lowered.label_offsets.end());
          // Ignore unused synthetic anchors and alias multiplicity. Both
          // pipelines may place unused entry/exit anchors at different positions;
          // every branch/address/data target retains its actual byte offset.
          labels.erase(std::unique(labels.begin(),labels.end()),labels.end());
          lowered.label_offsets.erase(std::unique(lowered.label_offsets.begin(),lowered.label_offsets.end()),lowered.label_offsets.end());
          CHECK(absolute == lowered.absolute_offsets); CHECK(labels == lowered.label_offsets);
          if (target->architecture() == target::Architecture::X86_64) {
            std::vector<std::size_t> aligned;
            for (auto l : native_frame->aligned_loop_labels()) if (l->address_taken() || used.contains(l)) {
              auto at = label_offsets.at(l);
              CHECK(at % 8 == 0); aligned.push_back(at);
            }
            std::sort(aligned.begin(),aligned.end()); aligned.erase(std::unique(aligned.begin(),aligned.end()),aligned.end());
            std::sort(lowered.aligned_offsets.begin(),lowered.aligned_offsets.end()); lowered.aligned_offsets.erase(std::unique(lowered.aligned_offsets.begin(),lowered.aligned_offsets.end()),lowered.aligned_offsets.end());
            CHECK(aligned == lowered.aligned_offsets);
          }
          REQUIRE(encoded->verify());
          auto relocatable = encode_machine(**machine,*native_frame); REQUIRE(relocatable);
          auto resolved = relocatable->relocate(0,[](auto &) -> Result<std::uint64_t> { return 0x123456789abcdef0ull; }); REQUIRE(resolved);
          compare_bytes(*resolved);
          ++compared_encodings; compared_code_bytes += encoded->bytes().size(); compared_code_relocations += relocatable->relocations().size();
        }
      }
    }
    auto actual = post ? post_snapshot : lower ? lowered_native(**machine, native_ranges ? &*native_ranges : nullptr, false, native_split ? &native_split->allocation() : native_rewrite ? &native_rewrite->allocation() : native_allocation ? &*native_allocation : nullptr, native_split ? &native_split->rewrite_result() : native_rewrite ? &*native_rewrite : nullptr, native_split ? &native_split->cfg() : nullptr) : native(**machine, level).str(), expected = lower ? lowered.snapshot : reference.snapshot.str();
    INFO("level ", level, "\ninput:\n", *text, "\nC:\n", expected, "\nC++:\n", actual);
    if (post >= 3 && actual.find("x86-mov128-unaligned ") != std::string::npos) {
      // Explicit C defect comparison: JRET has no stacked PC, so BP-relative
      // Windows vector slots are eight-byte aligned. Only the required move
      // opcode may differ; every operand, offset, ordering and state still agrees.
      REQUIRE(target->abi() == target::ABI::X86Windows); REQUIRE((*machine)->state().requires_frame_pointer);
      bool jump = false; for (auto &b : (*machine)->blocks()) for (auto &i : b.instructions()) jump |= i.opcode() == MachineOpcode(Opcode::JRet); REQUIRE(jump);
      CHECK(actual != expected); auto aligned = actual;
      constexpr std::string_view unaligned_move = "x86-mov128-unaligned ", aligned_move = "x86-mov128 ";
      for (std::size_t at = 0; (at = aligned.find(unaligned_move,at)) != std::string::npos; at += aligned_move.size()) aligned.replace(at,unaligned_move.size(),aligned_move);
      INFO("Known C defect: MOVDQA on an unaligned JRET frame; native code requires MOVDQU"); CHECK(aligned == expected);
    } else CHECK(actual == expected);
    if (post && post < 3 && level) { INFO("C post live:\n",lowered.post_live_snapshot,"\nC++ post live:\n",post_live_snapshot); CHECK(post_live_snapshot == lowered.post_live_snapshot); }
    if (ranges && !post) { INFO("C raw:\n", lowered.raw_snapshot, "\nC++ raw:\n", raw_actual); CHECK(raw_actual == lowered.raw_snapshot); }
  }
  MIR_gen_finish(c); MIR_finish(c);
}
}
TEST_CASE("AArch64 owning lowering and legalization match original instructions locations and function state") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  if (target->architecture() != target::Architecture::AArch64) return;
  for (unsigned stage = 0; stage < 7; ++stage) {
  bool legalize = stage >= 1, coalescing = stage >= 2, ranges = stage >= 3, assignment = stage >= 4, rewrite = stage >= 5, split = stage >= 6; CAPTURE(stage);
  for (auto &d : instruction_descriptors()) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(d.name); auto source = test::public_instruction_example(d); compare(*source, level, true, legalize, coalescing, ranges, assignment, true, rewrite, split);
  }
  auto inputs = test::inline_fixtures(); auto address = test::address_fixtures(); auto jumps = test::jump_fixtures();
  inputs.insert(inputs.end(), address.begin(), address.end()); inputs.insert(inputs.end(), jumps.begin(), jumps.end());
  auto abi = test::aarch64_lowering_fixtures(); inputs.insert(inputs.end(), abi.begin(), abi.end());
  auto random = test::random_jump_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(text); auto c = serialization::parse_text(text); REQUIRE(c); compare(**c, level, true, legalize, coalescing, ranges, assignment, true, rewrite, split);
  }
  auto large = serialization::parse_text(test::aarch64_large_argument_fixture()); REQUIRE(large); compare(**large, 0, true, legalize, coalescing, ranges, assignment, true, rewrite, split);
  for (unsigned file = 1; file <= 16; ++file) {
    auto path = std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR) / "mir-tests" / ("test" + std::to_string(file) + ".mir");
    std::ifstream input(path); REQUIRE(input); std::string text((std::istreambuf_iterator<char>(input)), {});
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) { CAPTURE(path); compare(**source, level, true, legalize, coalescing, ranges, assignment, true, rewrite, split); }
  }
  }
}
TEST_CASE("owning BBV preparation matches C property-preserving frames and flat block boundaries") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  auto compare_bbv = [&](const Context &c,unsigned level,bool optimized = true) {
    compare(c,level,true,true,true,true,true,optimized,true,true,3,false,false,nullptr,true);
  };
  for (auto &d : instruction_descriptors()) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(d.name); CAPTURE(level); auto c = test::public_instruction_example(d); compare_bbv(*c,level);
  }
  auto abi = target->architecture() == target::Architecture::AArch64 ? test::aarch64_lowering_fixtures() : test::x86_lowering_fixtures(target->abi() == target::ABI::X86Windows);
  for (auto &text : abi) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(text); CAPTURE(level); auto c = serialization::parse_text(text); REQUIRE(c); compare_bbv(**c,level);
  }
  for (auto &text : test::post_allocation_fixtures()) if (text.find("prset") != std::string::npos) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(text); CAPTURE(level); auto c = serialization::parse_text(text); REQUIRE(c); compare_bbv(**c,level,false);
  }
  auto c = serialization::parse_text("m:module\nf:func i64,i64:x\nlocal p:address\nentry:prset x,7\nprbeq alias,x,7\nladdr address,alias\njmpi address\nalias:\nsecond:\nswitch x,done,done,entry\ndone:ret x\nendfunc\nendmodule\n"); REQUIRE(c);
  for (unsigned level = 0; level < 4; ++level) compare_bbv(**c,level,false);
}
TEST_CASE("owning BBV property selection matches actual lazy C specialization at O0 through O3") {
  struct Trace {
    std::vector<std::string> blocks;
    std::vector<std::string> bytes;
    static void observe(MIR_context_t,const char *phase,MIR_item_t,FILE *file,void *data) {
      if (std::string_view(phase) == "bbv-encoding") {
        REQUIRE(file); char line[512]; std::string text;
        while (std::fgets(line,sizeof(line),file)) text += line;
        static_cast<Trace *>(data)->bytes.push_back(std::move(text)); return;
      }
      if (std::string_view(phase) != "bbv-specialization") return;
      REQUIRE(file); std::string in,out,emissions; char line[512];
      while (std::fgets(line,sizeof(line),file)) {
        if (std::string_view(line).starts_with("in")) in = line;
        else if (std::string_view(line).starts_with("out")) out = line;
        else emissions += line;
      }
      static_cast<Trace *>(data)->blocks.push_back(in+out+emissions);
    }
  };
  for (unsigned level = 0; level < 4; ++level) for (auto branch : {"prbeq","prbne"}) for (unsigned value : {0,1}) {
    CAPTURE(level); CAPTURE(branch); CAPTURE(value);
    auto text = std::string("m:module\nf:func i64,i64:x\nlocal i64:r\nprset x,1\n") + branch + " yes,x," + std::to_string(value)
      + "\nmov r,17\nret r\nyes:mov r,42\nret r\nendfunc\nendmodule\n";
    auto source = serialization::parse_text(text); REQUIRE(source);
    auto optimized = transforms::optimize(**source,{.level=static_cast<transforms::OptimizationLevel>(level)}); REQUIRE(optimized);
    auto target = target::TargetMachine::native(); REQUIRE(target);
    auto preparation = BBVPreparation::prepare(*optimized->find_function("m","f"),*target,{.optimization_level=level}); REQUIRE(preparation);
    auto program = BBVProgram::create(std::move(*preparation)); REQUIRE(program);
    auto empty = BBVProperties::create(program->preparation().function()); REQUIRE(empty);
    auto entry = program->request_version(*program->preparation().blocks().begin(),*empty,true); REQUIRE(entry);
    std::vector<std::string> native,native_bytes;
    for (auto version = entry->version;;) {
      REQUIRE(native.size() < 20); auto plan = program->specialize(*version); REQUIRE(plan); std::ostringstream out;
      auto properties = [&](std::string_view name,const BBVProperties &p) {
        out << name; for (auto &a : p.entries()) { auto r = std::get_if<target::PhysicalRegister>(&a.location); REQUIRE(r); out << ' ' << r->encoding()+1 << ':' << a.value; } out << '\n';
      };
      properties("in",version->incoming()); properties("out",(*plan)->outgoing());
      for (auto &i : (*plan)->emissions()) out << "emit " << opcode_name(i.opcode) << '\n';
      auto edges = (*plan)->successors();
      if (!edges.empty() && edges.back().kind == BBVSuccessor::Kind::Fallthrough) out << "emit jmp\n";
      native.push_back(out.str()); if (edges.empty()) break; REQUIRE(edges.size() == 1); version = edges.front().version;
    }
    for (auto version = entry->version;;) {
      auto code = encode_bbv(*program,*version); INFO((code ? "" : code.error().message)); REQUIRE(code); std::ostringstream hex;
      hex << "block " << version->block().index() << '\n' << std::hex << std::setfill('0');
      for (auto byte : code->bytes()) hex << std::setw(2) << unsigned(byte);
      native_bytes.push_back(hex.str()+'\n'); auto edges = version->specialization()->successors(); if (edges.empty()) break; version = edges.front().version;
    }
    // Run an independent owning runtime. Its real cold entries decide which
    // block compiles next; this is not a traversal of the expected C trace.
    auto runtime_preparation = BBVPreparation::prepare(*optimized->find_function("m","f"),*target,{.optimization_level=level}); REQUIRE(runtime_preparation);
    auto runtime_program = BBVProgram::create(std::move(*runtime_preparation)); REQUIRE(runtime_program);
    std::vector<std::string> actual,actual_bytes;
    auto runtime = execution::detail::BBVRuntime::create(std::move(*runtime_program),{.code_capacity=1024*1024,
      .failure_continuation=reinterpret_cast<std::uintptr_t>(+[] { std::abort(); }),.observe=[&](auto &v,auto &code) -> Result<void> {
        std::ostringstream out,hex; auto &plan = *v.specialization();
        auto properties = [&](auto name,const BBVProperties &p) {
          out << name; for (auto &a : p.entries()) { auto r = std::get_if<target::PhysicalRegister>(&a.location); REQUIRE(r); out << ' ' << r->encoding()+1 << ':' << a.value; } out << '\n';
        };
        properties("in",v.incoming()); properties("out",plan.outgoing());
        for (auto &i : plan.emissions()) out << "emit " << opcode_name(i.opcode) << '\n';
        if (!plan.successors().empty() && plan.successors().back().kind == BBVSuccessor::Kind::Fallthrough) out << "emit jmp\n";
        actual.push_back(out.str()); hex << "block " << v.block().index() << '\n' << std::hex << std::setfill('0');
        for (auto byte : code.bytes()) hex << std::setw(2) << unsigned(byte); actual_bytes.push_back(hex.str()+'\n'); return {};
      }}); REQUIRE(runtime);
    auto native_call = reinterpret_cast<std::int64_t (*)(std::int64_t)>((*runtime)->entry_address());
    execution::ExecutionInvocation invocation;
    CHECK(native_call(42) == (((std::string_view(branch) == "prbeq") == bool(value)) ? 42 : 17)); REQUIRE(invocation.status());
    CHECK(actual == native); CHECK(actual_bytes == native_bytes); auto warm_count = actual.size(); native_call(17); CHECK(actual.size() == warm_count);
    auto c = MIR_init(); MIR_scan_string(c,text.c_str()); Trace reference; MIR_opt_set_test_observer(c,Trace::observe,&reference);
    MIR_gen_init(c); MIR_gen_set_optimize_level(c,level); MIR_item_t function = nullptr;
    for (auto m = DLIST_HEAD(MIR_module_t,*MIR_get_module_list(c)); m; m = DLIST_NEXT(MIR_module_t,m)) {
      MIR_load_module(c,m);
      for (auto i = DLIST_HEAD(MIR_item_t,m->items); i; i = DLIST_NEXT(MIR_item_t,i)) if (i->item_type == MIR_func_item) function = i;
    }
    REQUIRE(function); MIR_link(c,MIR_set_lazy_bb_gen_interface,nullptr);
    CHECK(reinterpret_cast<std::int64_t (*)(std::int64_t)>(function->addr)(42) == (((std::string_view(branch) == "prbeq") == bool(value)) ? 42 : 17));
    REQUIRE(reference.blocks.size() == native.size());
    CHECK(reference.blocks == actual); CHECK(reference.bytes == actual_bytes);
    for (std::size_t n = 0; n < native.size(); ++n) { CAPTURE(n); INFO("C:\n",reference.blocks[n],"\nC++:\n",native[n]); CHECK(reference.blocks[n] == native[n]); }
    REQUIRE(reference.bytes.size() == native_bytes.size());
    for (std::size_t n = 0; n < native_bytes.size(); ++n) { CAPTURE(n); INFO("C encoding:\n",reference.bytes[n],"\nC++ encoding:\n",native_bytes[n]); CHECK(reference.bytes[n] == native_bytes[n]); }
    auto count = reference.blocks.size(); reinterpret_cast<std::int64_t (*)(std::int64_t)>(function->addr)(17); CHECK(reference.blocks.size() == count);
    MIR_gen_finish(c); MIR_finish(c); REQUIRE(program->verify());
  }
}
TEST_CASE("owning BBV bytes match actual C lazy generation for branches loops tables addresses floating and stack operations") {
  struct Trace {
    std::map<std::size_t,std::string> blocks;
    static void observe(MIR_context_t,const char *phase,MIR_item_t,FILE *file,void *data) {
      if (std::string_view(phase) != "bbv-encoding") return;
      REQUIRE(file); std::size_t block; REQUIRE(std::fscanf(file,"block %zu\n",&block) == 1);
      std::string bytes; char line[512]; while (std::fgets(line,sizeof(line),file)) bytes += line;
      CHECK(static_cast<Trace *>(data)->blocks.emplace(block,std::move(bytes)).second);
    }
  };
  struct Fixture { const char *text; std::array<std::int64_t,3> expected; };
  const Fixture fixtures[]{
    {"m:module\nf:func i64,i64:x\nlocal i64:r\nbt a,x\nmov r,7\nret r\na:mov r,9\nret r\nendfunc\nendmodule\n",{7,9,9}},
    {"m:module\nf:func i64,i64:x\nlocal i64:r\nmov r,0\nloop:ble done,x,0\nadd r,r,x\nsub x,x,1\njmp loop\ndone:ret r\nendfunc\nendmodule\n",{0,1,3}},
    {"m:module\nf:func i64,i64:x\nlocal i64:r\nswitch x,a,b,a\na:mov r,11\nret r\nb:mov r,29\nret r\nendfunc\nendmodule\n",{11,29,11}},
    {"m:module\nf:func i64,i64:x\nlocal i64:address,i64:r\nladdr address,target\njmpi address\ntarget:mov r,73\nret r\nendfunc\nendmodule\n",{73,73,73}},
    {"m:module\nf:func i64,i64:x\nlocal d:a,d:b,i64:r\ndmov a,0.0\ndmov b,0.0\nddiv a,a,b\ndbne yes,a,b\nmov r,17\nret r\nyes:mov r,42\nret r\nendfunc\nendmodule\n",{42,42,42}},
    {"m:module\nf:func i64,i64:x\nlocal i64:address,i64:r\nalloca address,32\nmov i64:(address),x\nmov r,i64:(address)\nret r\nendfunc\nendmodule\n",{0,1,2}},
    {"m:module\nf:func i64,i64:x\nlocal i64:r\nmov r,9223372036854775807\naddo r,r,x\nbo yes\nmov r,17\nret r\nyes:mov r,42\nret r\nendfunc\nendmodule\n",{17,42,42}},
  };
  for (auto &fixture : fixtures) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(fixture.text); CAPTURE(level);
    auto source = serialization::parse_text(fixture.text); INFO((source ? "" : source.error().message)); REQUIRE(source);
    auto optimized = transforms::optimize(**source,{.level=static_cast<transforms::OptimizationLevel>(level)}); REQUIRE(optimized);
    auto target = target::TargetMachine::native(); REQUIRE(target);
    auto preparation = BBVPreparation::prepare(*optimized->find_function("m","f"),*target,{.optimization_level=level}); REQUIRE(preparation);
    auto p = BBVProgram::create(std::move(*preparation)); REQUIRE(p); auto empty = BBVProperties::create(p->preparation().function()); REQUIRE(empty);
    auto entry = p->request_version(*p->preparation().blocks().begin(),*empty,true); REQUIRE(entry);
    // Start both backends from exactly the same middle-end output. Binary
    // loading preserves literal bits, including NaN. The file protocol omits
    // unused scales; canonicalize those below to match machine preparation.
    auto binary = serialization::write_binary(optimized->context()); REQUIRE(binary);
    auto file = std::tmpfile(); REQUIRE(file); REQUIRE(std::fwrite(binary->data(),1,binary->size(),file) == binary->size()); std::rewind(file);
    auto c = MIR_init(); MIR_read(c,file); std::fclose(file); Trace reference; MIR_opt_set_test_observer(c,Trace::observe,&reference);
    MIR_gen_init(c); MIR_gen_set_optimize_level(c,level); MIR_item_t function = nullptr;
    for (auto m = DLIST_HEAD(MIR_module_t,*MIR_get_module_list(c)); m; m = DLIST_NEXT(MIR_module_t,m)) {
      MIR_load_module(c,m); for (auto i = DLIST_HEAD(MIR_item_t,m->items); i; i = DLIST_NEXT(MIR_item_t,i)) if (i->item_type == MIR_func_item) function = i;
    }
    REQUIRE(function);
    MIR_link(c,MIR_set_lazy_bb_gen_interface,nullptr);
    // Both zero-index addressing forms execute identically, but AArch64's S
    // bit still reflects this otherwise unused field. Feed the same canonical
    // value (1) to C, rather than ignoring a byte difference in the comparison.
    for (auto i = DLIST_HEAD(MIR_insn_t,function->u.func->insns); i; i = DLIST_NEXT(MIR_insn_t,i))
      for (std::size_t n = 0; n < i->nops; ++n) if (i->ops[n].mode == MIR_OP_MEM && !i->ops[n].u.mem.index) i->ops[n].u.mem.scale = 1;
    for (std::int64_t value = 0; value < 3; ++value) CHECK(reinterpret_cast<std::int64_t (*)(std::int64_t)>(function->addr)(value) == fixture.expected[value]);
    CodeSymbolResolver symbols = [&](auto &request) -> Result<std::uint64_t> {
      REQUIRE(request.symbol); auto &symbol = *request.symbol;
      for (auto m = DLIST_HEAD(MIR_module_t,*MIR_get_module_list(c)); m; m = DLIST_NEXT(MIR_module_t,m)) if (symbol.module == m->name)
        for (auto item = DLIST_HEAD(MIR_item_t,m->items); item; item = DLIST_NEXT(MIR_item_t,item)) if (symbol.name == MIR_item_name(c,item)) return reinterpret_cast<std::uintptr_t>(item->addr);
      return Error{ErrorCode::UnknownName,"missing C symbol in BBV comparison"};
    };
    std::vector<const BBVVersion *> work{entry->version}; std::map<std::size_t,std::string> native;
    for (std::size_t n = 0; n < work.size(); ++n) {
      auto v = work[n]; if (native.contains(v->block().index())) continue;
      auto compiled = encode_bbv(*p,*v,{.known_addresses=symbols}); INFO((compiled ? "" : compiled.error().message)); REQUIRE(compiled);
      std::ostringstream bytes; bytes << std::hex << std::setfill('0'); for (auto byte : compiled->bytes()) bytes << std::setw(2) << unsigned(byte);
      native.emplace(v->block().index(),bytes.str()+'\n'); for (auto &edge : v->specialization()->successors()) work.push_back(edge.version);
    }
    CHECK_FALSE(reference.blocks.empty());
    for (auto &[index,bytes] : reference.blocks) {
      CAPTURE(index); REQUIRE(native.contains(index)); INFO("C:\n",bytes,"\nC++:\n",native.at(index)); CHECK(bytes == native.at(index));
    }
    auto runtime_preparation = BBVPreparation::prepare(*optimized->find_function("m","f"),*target,{.optimization_level=level}); REQUIRE(runtime_preparation);
    auto runtime_program = BBVProgram::create(std::move(*runtime_preparation)); REQUIRE(runtime_program);
    std::map<std::size_t,std::string> actual;
    auto runtime = execution::detail::BBVRuntime::create(std::move(*runtime_program),{.code_capacity=1024*1024,
      .failure_continuation=reinterpret_cast<std::uintptr_t>(+[] { std::abort(); }),.resolve=symbols,
      .observe=[&](auto &v,auto &code) -> Result<void> {
        std::ostringstream bytes; bytes << std::hex << std::setfill('0'); for (auto byte : code.bytes()) bytes << std::setw(2) << unsigned(byte);
        CHECK(actual.emplace(v.block().index(),bytes.str()+'\n').second); return {};
      }}); REQUIRE(runtime);
    execution::ExecutionInvocation invocation;
    auto native_call = reinterpret_cast<std::int64_t (*)(std::int64_t)>((*runtime)->entry_address());
    for (std::int64_t value = 0; value < 3; ++value) CHECK(native_call(value) == fixture.expected[value]);
    REQUIRE(invocation.status()); CHECK(actual == reference.blocks);
    auto warm_count = actual.size(); for (std::int64_t value = 0; value < 3; ++value) CHECK(native_call(value) == fixture.expected[value]);
    CHECK(actual.size() == warm_count); CHECK((*runtime)->statistics().compilation_attempts == warm_count);
    MIR_gen_finish(c); MIR_finish(c);
  }
}
TEST_CASE("owning BBV runtime preserves C first-version properties across joins reached in either order") {
  struct Trace {
    std::vector<std::string> bytes;
    static void observe(MIR_context_t,const char *phase,MIR_item_t,FILE *file,void *data) {
      if (std::string_view(phase) != "bbv-encoding") return;
      REQUIRE(file); std::string text; char line[512]; while (std::fgets(line,sizeof line,file)) text += line;
      static_cast<Trace *>(data)->bytes.push_back(std::move(text));
    }
  };
  constexpr auto text = "m:module\nf:func i64,i64:x\nlocal i64:r\nbt yes,x\nprset x,7\njmp join\nyes:prset x,9\njoin:prbeq seven,x,7\nmov r,29\nret r\nseven:mov r,11\nret r\nendfunc\nendmodule\n";
  for (unsigned level = 0; level < 4; ++level) for (std::int64_t first : {0,1}) {
    CAPTURE(level); CAPTURE(first); auto source = serialization::parse_text(text); REQUIRE(source);
    auto optimized = transforms::optimize(**source,{.level=static_cast<transforms::OptimizationLevel>(level)}); REQUIRE(optimized);
    auto preparation = BBVPreparation::prepare(*optimized->find_function("m","f"),*target::TargetMachine::native(),{.optimization_level=level}); REQUIRE(preparation);
    auto program = BBVProgram::create(std::move(*preparation)); REQUIRE(program); std::vector<std::string> actual;
    auto runtime = execution::detail::BBVRuntime::create(std::move(*program),{.code_capacity=1024*1024,
      .failure_continuation=reinterpret_cast<std::uintptr_t>(+[] { std::abort(); }),.observe=[&](auto &v,auto &code) -> Result<void> {
        std::ostringstream hex; hex << "block " << v.block().index() << '\n' << std::hex << std::setfill('0');
        for (auto byte : code.bytes()) hex << std::setw(2) << unsigned(byte); actual.push_back(hex.str()+'\n'); return {};
      }}); REQUIRE(runtime);
    Trace reference; auto c = MIR_init(); MIR_scan_string(c,text); MIR_opt_set_test_observer(c,Trace::observe,&reference);
    MIR_gen_init(c); MIR_gen_set_optimize_level(c,level); MIR_item_t function = nullptr;
    for (auto m = DLIST_HEAD(MIR_module_t,*MIR_get_module_list(c)); m; m = DLIST_NEXT(MIR_module_t,m)) {
      MIR_load_module(c,m);
      for (auto i = DLIST_HEAD(MIR_item_t,m->items); i; i = DLIST_NEXT(MIR_item_t,i)) if (i->item_type == MIR_func_item) function = i;
    }
    REQUIRE(function); MIR_link(c,MIR_set_lazy_bb_gen_interface,nullptr);
    auto legacy = reinterpret_cast<std::int64_t (*)(std::int64_t)>(function->addr);
    auto native = reinterpret_cast<std::int64_t (*)(std::int64_t)>((*runtime)->entry_address()); execution::ExecutionInvocation invocation;
    const auto expected = first == 0 ? 11 : 29;
    for (auto input : {first,1-first,first}) {
      CHECK(legacy(input) == expected); CHECK(native(input) == expected); REQUIRE(invocation.status());
      CHECK(actual == reference.bytes);
    }
    CHECK((*runtime)->statistics().compiled_blocks == actual.size()); CHECK((*runtime)->statistics().compilation_attempts == actual.size());
    MIR_gen_finish(c); MIR_finish(c);
  }
}
TEST_CASE("x86 owning lowering allocation rewrite and post passes match C instructions locations CFG and state") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  if (target->architecture() != target::Architecture::X86_64) return;
  for (unsigned stage = 0; stage < 9; ++stage) {
    auto compare_stage = [&](const Context &c,unsigned level) {
      compare(c,level,true,stage >= 1,stage >= 2,stage >= 3,stage >= 4,true,stage >= 5,stage >= 6,stage >= 7 ? stage - 6 : 0);
    };
    for (auto &d : instruction_descriptors()) for (unsigned level = 0; level < 4; ++level) {
      CAPTURE(stage); CAPTURE(d.name); CAPTURE(level);
      auto source = test::public_instruction_example(d); compare_stage(*source,level);
    }
    for (auto &text : test::x86_lowering_fixtures(target->abi() == target::ABI::X86Windows)) for (unsigned level = 0; level < 4; ++level) {
      CAPTURE(stage); CAPTURE(level); auto source = serialization::parse_text(text); REQUIRE(source); compare_stage(**source,level);
    }
  }
  for (auto &text : test::post_allocation_fixtures()) for (unsigned level = 0; level < 4; ++level) for (unsigned post : {1,2}) {
    CAPTURE(text); CAPTURE(level); CAPTURE(post); auto source = serialization::parse_text(text); REQUIRE(source);
    compare(**source,level,true,true,true,true,true,false,true,true,post);
    if (text.find("prset") != std::string::npos) compare(**source,level,true,true,true,true,true,false,true,true,post,false);
  }
}
TEST_CASE("owning assignment matches C under seeded branch and loop register pressure") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  compared_edge_placements = compared_split_blocks = 0;
  for (unsigned seed = 1; seed <= 8; ++seed) {
    std::ostringstream text; text << "m:module\nf:func i64,i64:condition";
    for (unsigned n = 0; n < 40; ++n) text << ",i64:x" << n;
    text << "\nlocal i64:acc,i64:tmp\nmov acc,0\nL0:\nbeq L1,condition,0\n";
    for (unsigned n = 0; n < 40; n += 2) text << "add tmp,x" << (n * seed % 40) << ",x" << ((n + 1) * seed % 40) << "\n";
    text << "jmp L2\nL1:\n";
    for (unsigned n = 0; n < 40; n += 2) text << "sub tmp,x" << ((n + seed) % 40) << ",x" << ((n + seed + 1) % 40) << "\n";
    text << "L2:\n"; for (unsigned n = 0; n < 40; ++n) text << "add acc,acc,x" << n << "\n";
    if (seed & 1) text << "sub condition,condition,1\nbt L0,condition\n";
    text << "ret acc\nendfunc\nendmodule\n";
    auto c = serialization::parse_text(text.str()); REQUIRE(c);
    for (unsigned level = 0; level < 4; ++level) for (unsigned stage : {4,5,6,7,8}) { CAPTURE(seed); CAPTURE(level); CAPTURE(stage); compare(**c, level, true, true, true, true, true, false, stage >= 5, stage >= 6, stage >= 7 ? stage - 6 : 0); }
  }
}
TEST_CASE("owning edge preservation matches C with actual whole-block spills and critical edge splitting") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  compared_edge_placements = compared_split_blocks = 0;
  for (bool critical : {false, true}) {
    auto source = serialization::parse_text(test::edge_spill_fixture(*target, critical)); INFO((source ? "" : source.error().message)); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) { CAPTURE(critical); CAPTURE(level); compare(**source, level, true, true, true, true, true, false, true, true); }
  }
  CHECK(compared_edge_placements > 0); CHECK(compared_split_blocks > 0);
  std::fprintf(stderr, "Compared edge placements: %zu; split blocks: %zu\n", compared_edge_placements, compared_split_blocks);
}
TEST_CASE("owning post-allocation combine and DCE match C instructions boundaries and deaths") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  if (target->architecture() != target::Architecture::AArch64) return;
  compared_post_substitutions = compared_post_extensions = compared_post_deletions = 0;
  for (auto &d : instruction_descriptors()) for (unsigned level = 0; level < 4; ++level) for (unsigned stage : {1,2}) {
    CAPTURE(d.name); CAPTURE(level); CAPTURE(stage); auto source = test::public_instruction_example(d);
    compare(*source,level,true,true,true,true,true,true,true,true,stage);
  }
  for (auto &text : test::aarch64_lowering_fixtures()) {
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) for (unsigned stage : {1,2}) { CAPTURE(text); CAPTURE(level); CAPTURE(stage); compare(**source,level,true,true,true,true,true,true,true,true,stage); }
  }
  for (bool critical : {false,true}) {
    auto source = serialization::parse_text(test::edge_spill_fixture(*target,critical)); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) for (unsigned stage : {1,2}) { CAPTURE(critical); CAPTURE(level); CAPTURE(stage); compare(**source,level,true,true,true,true,true,false,true,true,stage); }
  }
  for (auto &text : test::post_allocation_fixtures()) {
    auto source = serialization::parse_text(text); INFO((source ? "" : source.error().message)); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) for (unsigned stage : {1,2}) { CAPTURE(text); CAPTURE(level); CAPTURE(stage); compare(**source,level,true,true,true,true,true,false,true,true,stage); }
    if (text.find("prset") != std::string::npos) for (unsigned level = 0; level < 4; ++level) for (unsigned stage : {1,2}) {
      CAPTURE(text); CAPTURE(level); CAPTURE(stage); compare(**source,level,true,true,true,true,true,false,true,true,stage,false);
    }
  }
  CHECK(compared_post_substitutions > 0); CHECK(compared_post_extensions > 0); CHECK(compared_post_deletions > 0);
  std::fprintf(stderr,"Compared post-allocation substitutions: %zu; extensions: %zu; dead definitions: %zu\n",compared_post_substitutions,compared_post_extensions,compared_post_deletions);
  for (unsigned file = 1; file <= 16; ++file) {
    auto path = std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR) / "mir-tests" / ("test" + std::to_string(file) + ".mir");
    std::ifstream input(path); REQUIRE(input); std::string text((std::istreambuf_iterator<char>(input)), {});
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) for (unsigned stage : {1,2}) { CAPTURE(path); CAPTURE(level); CAPTURE(stage); compare(**source,level,true,true,true,true,true,true,true,true,stage); }
  }
}
TEST_CASE("owning AArch64 frames match C on identical post-allocation input") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  if (target->architecture() != target::Architecture::AArch64) return;
  compared_frames = compared_frame_saves = 0;
  for (auto &d : instruction_descriptors()) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(d.name); CAPTURE(level); auto source = test::public_instruction_example(d);
    compare(*source,level,true,true,true,true,true,true,true,true,3);
  }
  for (auto &text : test::aarch64_lowering_fixtures()) {
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) { CAPTURE(text); CAPTURE(level); compare(**source,level,true,true,true,true,true,true,true,true,3); }
  }
  for (bool critical : {false,true}) {
    auto source = serialization::parse_text(test::edge_spill_fixture(*target,critical)); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,false,true,true,3);
  }
  for (auto &text : test::post_allocation_fixtures()) {
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,false,true,true,3);
  }
  for (unsigned file = 1; file <= 16; ++file) {
    auto path = std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR) / "mir-tests" / ("test" + std::to_string(file) + ".mir");
    std::ifstream input(path); REQUIRE(input); std::string text((std::istreambuf_iterator<char>(input)),{});
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,true,true,true,3);
  }
  for (bool variadic : {false,true}) for (bool jump : {false,true}) {
    if (variadic && jump) continue; // JRET's public convention forbids parameters and varargs.
    auto text = std::string("m:module\nf:func") + (jump ? "" : " i64,i64:x") + (variadic ? ",..." : "") + "\nlocal i64:r\nmov r,42\n" + (jump ? "jret r" : "ret r") + "\nendfunc\nendmodule\n";
    auto source = serialization::parse_text(text); INFO((source ? "" : source.error().message)); REQUIRE(source);
    for (std::size_t slots : {0u,1u,3u,509u,510u,511u,512u,4095u,4096u}) for (unsigned kind = 0; kind < 4; ++kind) {
      FrameFacts facts{slots,kind == 3 ? 17u : 0u,kind != 0,kind == 1,kind == 2};
      for (unsigned level = 0; level < 4; ++level) {
        CAPTURE(variadic); CAPTURE(jump); CAPTURE(slots); CAPTURE(kind); CAPTURE(level);
        compare(**source,level,true,true,true,true,true,true,true,true,3,true,true,&facts);
      }
    }
  }
  CHECK(compared_frames > 0); CHECK(compared_frame_saves > 0);
  std::fprintf(stderr,"Compared frames: %zu; callee saves: %zu\n",compared_frames,compared_frame_saves);
}
TEST_CASE("owning x86 frames match C on identical post-allocation input") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  if (target->architecture() != target::Architecture::X86_64) return;
  for (auto &d : instruction_descriptors()) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(d.name); CAPTURE(level); auto source = test::public_instruction_example(d);
    compare(*source,level,true,true,true,true,true,true,true,true,3);
  }
  for (auto &text : test::x86_lowering_fixtures(target->abi() == target::ABI::X86Windows)) {
    CAPTURE(text); auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,true,true,true,3);
  }
  for (bool critical : {false,true}) {
    auto source = serialization::parse_text(test::edge_spill_fixture(*target,critical)); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,false,true,true,3);
  }
  for (auto &text : test::post_allocation_fixtures()) {
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,false,true,true,3);
  }
  for (unsigned file = 1; file <= 16; ++file) {
    auto path = std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR) / "mir-tests" / ("test" + std::to_string(file) + ".mir");
    std::ifstream input(path); REQUIRE(input); std::string text((std::istreambuf_iterator<char>(input)),{});
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,true,true,true,3);
  }
  for (bool variadic : {false,true}) for (bool jump : {false,true}) {
    if (variadic && jump) continue;
    auto text = std::string("m:module\nf:func") + (jump ? "" : " i64,i64:x") + (variadic ? ",..." : "")
      + "\nlocal i64:r\nmov r,42\n" + (jump ? "jret r" : "ret r") + "\nendfunc\nendmodule\n";
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (bool fp : {false,true}) {
      if (variadic && !fp) continue;
      for (std::size_t slots : {0u,1u,3u,511u,512u,4096u}) for (unsigned kind = 0; kind < 4; ++kind) {
        if (!fp && (kind == 1 || kind == 2)) continue;
        FrameFacts facts{slots,0,kind != 0,kind == 1,kind == 2,fp};
        for (unsigned level = 0; level < 4; ++level) for (bool host : {false,true}) {
          CAPTURE(variadic); CAPTURE(jump); CAPTURE(fp); CAPTURE(slots); CAPTURE(kind); CAPTURE(level); CAPTURE(host);
          compare(**source,level,true,true,true,true,true,true,true,true,3,true,host,&facts);
        }
      }
    }
  }
}
TEST_CASE("owning native encoding matches C bytes labels and absolute relocations") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  const auto arm = target->architecture() == target::Architecture::AArch64;
  compared_encodings = compared_code_bytes = compared_code_relocations = 0;
  for (auto &d : instruction_descriptors()) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(d.name); CAPTURE(level); auto source = test::public_instruction_example(d);
    compare(*source,level,true,true,true,true,true,true,true,true,4);
  }
  for (auto &text : arm ? test::aarch64_lowering_fixtures() : test::x86_lowering_fixtures(target->abi() == target::ABI::X86Windows)) {
    CAPTURE(text); auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) { CAPTURE(level); compare(**source,level,true,true,true,true,true,true,true,true,4); }
  }
  for (auto &text : test::post_allocation_fixtures()) {
    CAPTURE(text); auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,false,true,true,4);
  }
  for (bool critical : {false,true}) {
    auto source = serialization::parse_text(test::edge_spill_fixture(*target,critical)); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,false,true,true,4);
  }
  for (unsigned file = 1; file <= 16; ++file) {
    auto path = std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR) / "mir-tests" / ("test" + std::to_string(file) + ".mir");
    std::ifstream input(path); REQUIRE(input); std::string text((std::istreambuf_iterator<char>(input)),{});
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,true,true,true,4);
  }
  for (std::size_t slots : {1u,509u,510u,511u,512u,4095u,4096u}) {
    FrameFacts facts{slots,arm ? 17u : 0u,false,false,false};
    auto source = serialization::parse_text("m:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n"); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,true,true,true,4,true,true,&facts);
  }
  if (!arm) {
    for (unsigned padding : {0,1,15,16,17,32,64,128}) for (bool backwards : {false,true}) {
      std::string text = "m:module\nf:func i64,i64:x\nlocal i64:r\nmov r,x\n";
      if (backwards) text += "loop:label\n"; else text += "bt finish,x\n";
      // Side-effecting stores cannot disappear. They exercise actual instruction
      // lengths, conservative forward estimates and short/long backward choices.
      for (unsigned n = 0; n < padding; ++n) text += "mov i64:(x),r\n";
      text += backwards ? "bt loop,x\n" : "finish:label\n";
      text += "ret r\nendfunc\nendmodule\n";
      auto source = serialization::parse_text(text); REQUIRE(source);
      for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,true,true,true,4);
    }
    FrameFacts facts{3,0,false,true,false,true};
    auto source = serialization::parse_text("m:module\nf:func\nlocal i64:r\nmov r,42\njret r\nendfunc\nendmodule\n"); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) compare(**source,level,true,true,true,true,true,true,true,true,4,true,true,&facts);
  }
  CHECK(compared_encodings > 0); CHECK(compared_code_bytes > 0); CHECK(compared_code_relocations > 0);
  std::fprintf(stderr,"Compared encodings: %zu; code bytes: %zu; relocations: %zu\n",compared_encodings,compared_code_bytes,compared_code_relocations);
}
TEST_CASE("multiple memory fallback records the original C overwritten-operand defect explicitly") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  if (target->architecture() != target::Architecture::AArch64) return;
  const char *text = "m:module\nf:func i64,i64:a,i64:b,i64:c\nlocal i64:r\nadd i64:(a),i64:(b),i64:(c)\nmov r,i64:(a)\nret r\nendfunc\nendmodule\n";
  auto source = serialization::parse_text(text); REQUIRE(source);
  for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(level); auto c = MIR_init(); MIR_scan_string(c, text); MIR_gen_init(c);
    auto module = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(c)); REQUIRE(module);
    auto item = DLIST_HEAD(MIR_item_t, module->items); REQUIRE(item); REQUIRE(item->item_type == MIR_func_item);
    struct Counts {
      unsigned self_moves = 0, memory_moves = 0;
      static void observe(MIR_context_t ctx, MIR_item_t item, void *data) {
        auto &self = *static_cast<Counts *>(data);
        for (auto i = DLIST_HEAD(MIR_insn_t, item->u.func->insns); i; i = DLIST_NEXT(MIR_insn_t, i)) if (i->code == MIR_MOV) {
          self.self_moves += MIR_op_eq_p(ctx, i->ops[0], i->ops[1]);
          self.memory_moves += i->ops[0].mode == MIR_OP_VAR_MEM || i->ops[1].mode == MIR_OP_VAR_MEM;
        }
      }
    } counts;
    REQUIRE(_MIR_opt_test_legalize_function(c, item, level, Counts::observe, &counts));
    CHECK(counts.self_moves == 3); CHECK(counts.memory_moves == 1);
    auto &f = *(*source)->modules().begin()->functions().begin(); auto machine = prepare_machine_ir(f, *target); REQUIRE(machine);
    REQUIRE(machinize(**machine)); REQUIRE(legalize_operands(**machine)); unsigned memory_moves = 0, self_moves = 0;
    for (auto &b : (*machine)->blocks()) for (auto &i : b.instructions()) if (i.opcode() == MachineOpcode(Opcode::Mov)) {
      memory_moves += i.operands()[0].get<MachineMemory>() || i.operands()[1].get<MachineMemory>();
      self_moves += i.operands()[0] == i.operands()[1];
    }
    CHECK(memory_moves == 4); CHECK(self_moves == 0); REQUIRE((*machine)->verify());
    MIR_gen_finish(c); MIR_finish(c);
  }
}
TEST_CASE("machine preparation preserves all public opcode operands and ordered CFG at O0 through O3") {
  for (auto &d : instruction_descriptors()) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(d.name); auto source = test::public_instruction_example(d); compare(*source, level);
  }
}
TEST_CASE("machine preparation retains inlining address and fixed seed control flow behavior") {
  auto inputs = test::inline_fixtures(); auto jumps = test::random_jump_fixtures(); auto address = test::address_fixtures();
  inputs.insert(inputs.end(), jumps.begin(), jumps.end()); inputs.insert(inputs.end(), address.begin(), address.end());
  std::string nested = "m:module\nentry:func i64,i64:x\nmov x,x\n";
  for (unsigned n = 0; n < 32; ++n) nested += "header" + std::to_string(n) + ":\nmov x,x\n";
  for (unsigned n = 32; n-- > 0;) nested += "bt header" + std::to_string(n) + ",x\n";
  nested += "ret x\nendfunc\nendmodule\n"; inputs.push_back(std::move(nested));
  auto control_flow = test::jump_fixtures(); inputs.insert(inputs.end(), control_flow.begin(), control_flow.end());
  for (auto &text : inputs) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(text); auto source = serialization::parse_text(text); REQUIRE(source); compare(**source, level);
  }
}
TEST_CASE("native target register banks bindings reservations and clobbers match the C target") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  for (auto &d : instruction_descriptors()) {
    int code = -1;
    switch (d.opcode) {
#define MIRNEXT_OPCODE(name, wire, spelling, roles, count, flow) case Opcode::name: code = wire; break;
#include "mirnext/IR/Opcodes.def"
#undef MIRNEXT_OPCODE
    }
    std::array<std::uint32_t, 2> expected; _MIR_opt_test_live_target_clobbers(code, expected.data());
    auto actual = early_clobbers(*target, d.opcode);
    for (unsigned n = 0; n < 2; ++n) {
      CAPTURE(d.name); CHECK((actual[n] ? actual[n]->encoding() : UINT32_MAX) == expected[n]);
    }
  }
  for (auto &info : target->registers()) {
    CAPTURE(info.name); int integer, floating, extended, reserved, clobbered, extended_clobbered;
    REQUIRE(_MIR_opt_test_target_register_info(info.reg.encoding(), &integer, &floating, &extended, &reserved, &clobbered, &extended_clobbered));
    CHECK(target->accepts(info.reg, Type::i64()) == !!integer);
    CHECK(target->accepts(info.reg, Type(Type::Kind::F64)) == !!floating);
    // The C target's LD query is the storage-class rule before ABI
    // canonicalization. A double-width LD enters that same D register class.
    if constexpr (sizeof(long double) != sizeof(double)) CHECK(target->accepts(info.reg, Type(Type::Kind::LongDouble)) == !!extended);
    else CHECK(target->accepts(info.reg, Type(Type::Kind::LongDouble)) == !!floating);
    CHECK(info.reserved == !!reserved);
    CHECK(target->call_clobbers(info.reg, Type(Type::Kind::Undef)) == !!clobbered);
    CHECK(target->call_clobbers(info.reg, Type(Type::Kind::LongDouble)) == !!extended_clobbered);
  }
}
TEST_CASE("original MIR regression corpus prepares through the canonical owning pipeline") {
  for (unsigned file = 1; file <= 16; ++file) {
    auto path = std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR) / "mir-tests" / ("test" + std::to_string(file) + ".mir");
    std::ifstream input(path); REQUIRE(input); std::string text((std::istreambuf_iterator<char>(input)), {});
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) { CAPTURE(path); compare(**source, level); }
  }
}
