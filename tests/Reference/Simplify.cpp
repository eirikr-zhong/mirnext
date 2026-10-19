/* Licensed under the MIT License. C IR is read only in this isolated test adapter. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "COracle.h"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/Inline.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "../IR/PublicInstructions.hpp"
#include "../IR/Expressions.hpp"
#include "../IR/Calls.hpp"
#include "../IR/Variables.hpp"
#include "../Transforms/SimplifyFixtures.hpp"
#include "../Transforms/InlineFixtures.hpp"
#include "../Fixtures/UtilityPrograms.hpp"
#include <bit>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
using namespace mirnext;
using namespace mirnext::ir;
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
  case Type::Kind::Pointer: return MIR_T_P;
  case Type::Kind::Undef: return MIR_T_UNDEF;
  case Type::Kind::Block: return MIR_T_BLK + static_cast<int>(type.block_class());
  case Type::Kind::ReturnBlock: return MIR_T_RBLK;
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
  auto position = name.find("ld"); if (position != name.npos) name.replace(position, 2, "d"); return name;
}
struct NativeSnapshot {
  const Context &context; std::map<const Register *, std::size_t> registers;
  std::map<const Label *, std::size_t> labels; std::map<const Symbol *, std::string> names;
  std::size_t reg(const Register *r) { if (!r) return 0; auto [found, fresh] = registers.emplace(r, registers.size() + 1); return found->second; }
  std::string symbol(const Symbol *s) {
    if (s->kind() == Symbol::Kind::Import) {
      for (auto &module : context.modules()) for (auto &candidate : module.symbols())
        if (candidate.exported() && candidate.name() == s->name()) { s = &candidate; break; }
    }
    auto found = names.find(s); return found == names.end() ? std::string(s->name()) : found->second;
  }
  std::string operand(const Operand &o) {
    if (auto r = o.get<const Register *>()) return "r" + std::to_string(reg(*r)) + ":" + std::to_string(type_code((*r)->type(), true)) + ":" + std::string((*r)->hardware_binding());
    if (auto l = o.get<const Label *>()) return "l" + std::to_string(labels.at(*l));
    if (auto s = o.get<SymbolReference>()) return "s" + symbol(s->symbol) + ":" + std::to_string(s->displacement);
    if (auto m = o.get<Memory>()) {
      auto base = reg(m->base), index = reg(m->index);
      return "m" + std::to_string(type_code(m->type)) + ":" + std::to_string(m->type.is_block() ? m->type.block_size() : m->displacement)
        + ":" + std::to_string(base) + ":" + std::to_string(index) + ":" + std::to_string(m->index ? m->scale : 1) + ":" + m->alias + ":" + m->nonalias;
    }
    if (auto v = o.get<std::int64_t>()) return "i" + bits(*v);
    if (auto v = o.get<std::uint64_t>()) return "u" + bits(*v);
    if (auto v = o.get<float>()) return "f" + bits(*v);
    if (auto v = o.get<double>()) return "d" + bits(*v);
    if (auto v = o.get<long double>()) return (sizeof(long double) == 8 ? "d" : "e") + bits(*v);
    return "string:" + *o.get<std::string>();
  }
  std::string run() {
    std::ostringstream out;
    for (auto &module : context.modules()) {
      std::size_t pool = 0;
      for (auto &data : module.data()) if (data.is_literal_pool()) names.emplace(&data.symbol(), "pool" + std::to_string(pool++));
      for (auto &function : module.functions()) {
        registers.clear(); labels.clear(); std::size_t position = 0; std::set<std::size_t> anchors;
        for (auto argument : function.arguments()) reg(argument);
        for (auto &block : function.blocks()) {
          for (auto label : block.labels()) { labels.emplace(label, position); anchors.insert(position); }
          position += block.size();
        }
        out << "function:" << symbol(&function.symbol()) << '\n'; position = 0;
        for (auto &block : function.blocks()) for (auto &instruction : block.instructions()) {
          if (anchors.erase(position)) out << "anchor:" << position << '\n';
          out << target_opcode(std::string(instruction_descriptor(instruction.opcode())->name));
          for (std::size_t n = 0; n < instruction.operands().size(); ++n) {
            if (!n && instruction_descriptor(instruction.opcode())->count == OperandCount::Call) {
              auto sig = instruction.operands()[0].get<SymbolReference>()->symbol->signature(); out << " signature:" << sig->variadic;
              for (auto t : sig->results) out << ":r" << type_code(t);
              for (auto t : sig->parameters) out << ":a" << type_code(t) << ":" << t.block_size();
            } else out << ' ' << operand(instruction.operands()[n]);
          }
          out << '\n'; ++position;
        }
        if (anchors.erase(position)) out << "anchor:" << position << '\n';
      }
      for (auto &data : module.data()) if (data.is_literal_pool()) {
        auto &initializer = std::get<InitializedData>(data.initializer());
        out << symbol(&data.symbol()) << ':' << type_code(initializer.type) << ':' << initializer.values.size();
        for (auto &value : initializer.values) out << ' ' << operand(value); out << '\n';
      }
    }
    return out.str();
  }
};
struct CSnapshot {
  MIR_context_t context; std::map<MIR_reg_t, std::size_t> registers; std::map<MIR_insn_t, std::size_t> labels;
  std::map<MIR_item_t, std::string> names; std::set<MIR_item_t> original_data;
  std::map<MIR_item_t, std::string> identities;
  std::size_t reg(MIR_reg_t r) { if (!r) return 0; auto [found, fresh] = registers.emplace(r, registers.size() + 1); return found->second; }
  std::string symbol(MIR_item_t s) {
    while (s->ref_def) s = s->ref_def;
    if (auto found = names.find(s); found != names.end()) return found->second;
    auto found = identities.find(s); return found == identities.end() ? MIR_item_name(context, s) : found->second;
  }
  std::string operand(MIR_func_t function, MIR_op_t o) {
    switch (o.mode) {
    case MIR_OP_REG: { auto binding = MIR_reg_hard_reg_name(context, o.u.reg, function); return "r" + std::to_string(reg(o.u.reg)) + ":" + std::to_string(MIR_reg_type(context, o.u.reg, function)) + ":" + (binding ? binding : ""); }
    case MIR_OP_VAR: { auto binding = MIR_reg_hard_reg_name(context, o.u.var, function); return "r" + std::to_string(reg(o.u.var)) + ":" + std::to_string(MIR_reg_type(context, o.u.var, function)) + ":" + (binding ? binding : ""); }
    case MIR_OP_LABEL: return "l" + std::to_string(labels.at(o.u.label));
    case MIR_OP_REF: return "s" + symbol(o.u.ref) + ":0";
    case MIR_OP_INT: return "i" + bits(o.u.i); case MIR_OP_UINT: return "u" + bits(o.u.u);
    case MIR_OP_FLOAT: return "f" + bits(o.u.f); case MIR_OP_DOUBLE: return "d" + bits(o.u.d); case MIR_OP_LDOUBLE: return "e" + bits(o.u.ld);
    case MIR_OP_STR: return "string:" + std::string(o.u.str.s, o.u.str.len);
    case MIR_OP_MEM: {
      auto base = reg(o.u.mem.base), index = reg(o.u.mem.index);
      return "m" + std::to_string(o.u.mem.type) + ":" + std::to_string(o.u.mem.disp) + ":" + std::to_string(base) + ":" + std::to_string(index)
        + ":" + std::to_string(o.u.mem.index ? o.u.mem.scale : 1) + ":" + (o.u.mem.alias ? MIR_alias_name(context,o.u.mem.alias) : "")
        + ":" + (o.u.mem.nonalias ? MIR_alias_name(context,o.u.mem.nonalias) : "");
    }
    default: return "unexpected:" + std::to_string(o.mode);
    }
  }
  std::string run() {
    std::ostringstream out;
    for (auto module = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(context)); module; module = DLIST_NEXT(MIR_module_t, module)) {
      std::size_t pool = 0;
      for (auto item = DLIST_HEAD(MIR_item_t,module->items); item; item = DLIST_NEXT(MIR_item_t,item))
        if (item->item_type == MIR_data_item && !original_data.contains(item)) names.emplace(item, "pool" + std::to_string(pool++));
      for (auto item = DLIST_HEAD(MIR_item_t,module->items); item; item = DLIST_NEXT(MIR_item_t,item)) if (item->item_type == MIR_func_item) {
        auto function = item->u.func; registers.clear(); labels.clear(); std::size_t position = 0; std::set<std::size_t> anchors;
        for (std::size_t n = 0; n < function->nargs; ++n) reg(MIR_reg(context, VARR_GET(MIR_var_t, function->vars, n).name, function));
        for (auto i = DLIST_HEAD(MIR_insn_t,function->insns); i; i = DLIST_NEXT(MIR_insn_t,i)) {
          if (i->code == MIR_LABEL) { labels.emplace(i, position); anchors.insert(position); } else ++position;
        }
        out << "function:" << symbol(item) << '\n'; position = 0;
        for (auto i = DLIST_HEAD(MIR_insn_t,function->insns); i; i = DLIST_NEXT(MIR_insn_t,i)) {
          if (i->code == MIR_LABEL) continue;
          if (anchors.erase(position)) out << "anchor:" << position << '\n';
          out << MIR_insn_name(context,i->code);
          for (std::size_t n = 0; n < i->nops; ++n) {
            if (!n && MIR_call_code_p(i->code)) {
              auto sig = i->ops[0].u.ref->u.proto; out << " signature:" << static_cast<int>(sig->vararg_p);
              for (std::size_t r = 0; r < sig->nres; ++r) out << ":r" << sig->res_types[r];
              for (std::size_t a = 0; a < VARR_LENGTH(MIR_var_t,sig->args); ++a) {
                auto arg = VARR_GET(MIR_var_t,sig->args,a); out << ":a" << arg.type << ':' << (MIR_all_blk_type_p(arg.type) ? arg.size : 0);
              }
            } else out << ' ' << operand(function,i->ops[n]);
          }
          out << '\n'; ++position;
        }
        if (anchors.erase(position)) out << "anchor:" << position << '\n';
      }
      for (auto item = DLIST_HEAD(MIR_item_t,module->items); item; item = DLIST_NEXT(MIR_item_t,item)) if (names.contains(item)) {
        auto data = item->u.data; out << symbol(item) << ':' << data->el_type << ':' << data->nel;
        auto bytes = _MIR_data_els(data);
        for (std::size_t n = 0; n < data->nel; ++n) {
          if (data->el_type == MIR_T_U8) out << " i" << bits(static_cast<std::int64_t>(bytes[n]));
          else if (data->el_type == MIR_T_F) { float v; std::memcpy(&v,bytes + n*sizeof(v),sizeof(v)); out << " f" << bits(v); }
          else if (data->el_type == MIR_T_D) { double v; std::memcpy(&v,bytes + n*sizeof(v),sizeof(v)); out << " d" << bits(v); }
          else { long double v; std::memcpy(&v,bytes + n*sizeof(v),sizeof(v)); out << " e" << bits(v); }
        }
        out << '\n';
      }
    }
    return out.str();
  }
};
void compare(Context &context, void (*prepare)(MIR_context_t, Context &) = nullptr, bool inlining = false) {
  auto materialized = materialize(context); INFO((materialized ? "" : materialized.error().message)); REQUIRE(materialized);
  auto text = serialization::write_text(**materialized); REQUIRE(text);
  // Both passes consume the same textual IR, including explicit relocatable
  // address expansion. Native DSL-node behavior is checked by the C-off tests.
  auto canonical = serialization::parse_text(*text); REQUIRE(canonical); auto &source = **canonical;
  auto c = MIR_init(); MIR_scan_string(c,text->c_str()); CSnapshot reference{c};
  NativeSnapshot native{source};
  if (inlining) {
    for (auto &module : source.modules()) for (auto &symbol : module.symbols()) if (symbol.kind() != Symbol::Kind::Import)
      native.names.emplace(&symbol, std::string(module.name()) + "/" + std::string(symbol.name()));
    for (auto module = DLIST_HEAD(MIR_module_t,*MIR_get_module_list(c)); module; module = DLIST_NEXT(MIR_module_t,module))
      for (auto item = DLIST_HEAD(MIR_item_t,module->items); item; item = DLIST_NEXT(MIR_item_t,item))
        if (item->item_type != MIR_import_item && item->item_type != MIR_export_item && item->item_type != MIR_forward_item
            && item->item_type != MIR_proto_item)
          if (auto name = MIR_item_name(c,item)) reference.identities.emplace(item, std::string(module->name) + "/" + name);
  }
  if (prepare) prepare(c, source);
  for (auto module = DLIST_HEAD(MIR_module_t,*MIR_get_module_list(c)); module; module = DLIST_NEXT(MIR_module_t,module)) {
    for (auto item = DLIST_HEAD(MIR_item_t,module->items); item; item = DLIST_NEXT(MIR_item_t,item)) if (item->item_type == MIR_data_item) reference.original_data.insert(item);
    _MIR_opt_test_simplify_module(c,module);
  }
  for (auto &module : source.modules()) for (auto &function : module.functions()) {
    analysis::FunctionAnalysisManager analyses(function); transforms::FunctionPassManager passes; passes.add<transforms::SimplifyPass>();
    auto result = passes.run(function,analyses); INFO((result ? "" : result.error().message)); REQUIRE(result);
  }
  if (inlining) {
    _MIR_opt_test_inline_context(c);
    transforms::FunctionPassManager passes; passes.add<transforms::InlinePass>();
    auto result = passes.run(source); INFO((result ? "" : result.error().message)); REQUIRE(result);
  }
  auto expected = reference.run(), actual = native.run();
  INFO("input:\n", *text, "\nC:\n", expected, "\nC++:\n", actual); CHECK(actual == expected); MIR_finish(c);
}
}
TEST_CASE("native simplify matches isolated C instruction order operands and literal pools") {
  for (auto &descriptor : instruction_descriptors()) {
    CAPTURE(descriptor.name); auto context = test::public_instruction_example(descriptor);
    // The C null-address assertion is covered separately as a native regression.
    if (descriptor.opcode == Opcode::VaStart || descriptor.opcode == Opcode::VaEnd || descriptor.opcode == Opcode::VaArg || descriptor.opcode == Opcode::VaBlockArg) {
      for (auto &f : context->modules().begin()->functions()) for (auto &b : f.blocks()) for (auto &i : b.instructions())
        for (std::size_t n = 0; n < i.operands().size(); ++n) if (auto m = i.operands()[n].get<Memory>(); m && m->type.kind() == Type::Kind::Undef) {
          auto memory = *m; memory.displacement = 128; REQUIRE(IRRewriter(f).replace_operand(i,n,Operand(memory)));
        }
    }
    compare(*context);
  }
}
TEST_CASE("native simplify matches C for expressions calls globals and MIR regression corpus") {
  for (unsigned n = 0; n <= static_cast<unsigned>(test::ExpressionExample::Initializer); ++n) { CAPTURE(n); auto context = test::expression_example(static_cast<test::ExpressionExample>(n)); compare(*context); }
  for (unsigned n = 0; n <= static_cast<unsigned>(test::CallExample::Inline); ++n) { CAPTURE(n); auto context = test::call_example(static_cast<test::CallExample>(n)); compare(*context); }
  for (unsigned n = 0; n <= static_cast<unsigned>(test::VariableExample::Narrow); ++n) { CAPTURE(n); auto context = test::variable_example(static_cast<test::VariableExample>(n)); compare(*context); }
  for (unsigned n = 1; n <= 16; ++n) {
    CAPTURE(n); std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"mir-tests"/("test"+std::to_string(n)+".mir")); REQUIRE(input);
    std::string text((std::istreambuf_iterator<char>(input)),{}); auto parsed = serialization::parse_text(text); REQUIRE(parsed); compare(**parsed);
  }
}
TEST_CASE("native simplify matches all C rule boundary and interaction fixtures") {
  unsigned n = 0;
  for (auto &text : test::simplify_fixtures()) {
    CAPTURE(n); ++n; auto parsed = serialization::parse_text(text); INFO(text); REQUIRE(parsed); compare(**parsed);
  }
}
TEST_CASE("native simplify compares exact floating bits and unsigned operand categories") {
  auto parsed = serialization::parse_text("m:module\nf:func i64\nlocal i64:x, d:y\nmul x, 42, 1\ndmov y, 0.0\ndmov y, 0.0\ndmov y, 0.0\nret x\nendfunc\nendmodule\n"); REQUIRE(parsed);
  compare(**parsed, [](MIR_context_t c, Context &native) {
    constexpr std::uint64_t payloads[]{0x8000000000000000ULL, 0x7ff8000000000042ULL, 0x7ff0000000000042ULL};
    auto module = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(c)); auto item = DLIST_HEAD(MIR_item_t,module->items);
    while (item->item_type != MIR_func_item) item = DLIST_NEXT(MIR_item_t,item);
    unsigned index = 0;
    for (auto instruction = DLIST_HEAD(MIR_insn_t,item->u.func->insns); instruction; instruction = DLIST_NEXT(MIR_insn_t,instruction)) {
      if (instruction->code == MIR_MUL) instruction->ops[2] = MIR_new_uint_op(c,1);
      if (instruction->code == MIR_DMOV) instruction->ops[1] = MIR_new_double_op(c,std::bit_cast<double>(payloads[index++]));
    }
    auto &function = *native.modules().begin()->functions().begin(); IRRewriter rewriter(function); index = 0;
    for (auto &block : function.blocks()) for (auto &instruction : block.instructions()) {
      if (instruction.opcode() == Opcode::Mul) REQUIRE(rewriter.replace_operand(instruction,2,Operand(std::uint64_t(1))));
      if (instruction.opcode() == Opcode::DMov) REQUIRE(rewriter.replace_operand(instruction,1,Operand(std::bit_cast<double>(payloads[index++]))));
    }
    REQUIRE(native.verify());
  });
}
TEST_CASE("native simplify preserves hardware identity in the original hardware regression") {
  std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"c-tests/mir/reg2.mir"); REQUIRE(input);
  std::string text((std::istreambuf_iterator<char>(input)),{}); auto parsed = serialization::parse_text(text); REQUIRE(parsed); compare(**parsed);
}
TEST_CASE("native inline matches C for all typed call fixtures and hardware identity") {
  for (unsigned n = 0; n <= static_cast<unsigned>(test::CallExample::Inline); ++n) {
    CAPTURE(n); auto context = test::call_example(static_cast<test::CallExample>(n)); compare(*context, nullptr, true);
  }
  std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"c-tests/mir/reg2.mir"); REQUIRE(input);
  std::string text((std::istreambuf_iterator<char>(input)),{}); auto parsed = serialization::parse_text(text); REQUIRE(parsed); compare(**parsed, nullptr, true);
}
TEST_CASE("native inline matches C thresholds recursion frames cold layout and cross-module identities") {
  unsigned n = 0;
  for (auto &text : test::inline_fixtures()) {
    CAPTURE(n); ++n; auto parsed = serialization::parse_text(text); INFO(text); INFO((parsed ? "" : parsed.error().message)); REQUIRE(parsed); compare(**parsed, nullptr, true);
  }
  for (unsigned number = 1; number <= 16; ++number) {
    CAPTURE(number); std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"mir-tests"/("test"+std::to_string(number)+".mir")); REQUIRE(input);
    std::string text((std::istreambuf_iterator<char>(input)),{}); auto parsed = serialization::parse_text(text); REQUIRE(parsed); compare(**parsed, nullptr, true);
  }
}
TEST_CASE("native utility memory and API sieve simplification retain independent C stage checks") {
  auto memory = serialization::parse_text(test::utility_memory_text); REQUIRE(memory); compare(**memory);
  Context sieve; REQUIRE(test::append_api_sieve(sieve,"api",127,2)); REQUIRE(sieve.verify()); compare(sieve);
  std::ifstream input(std::filesystem::path(MIRNEXT_TEST_SOURCE_DIR)/"tests/Fixtures/scanner.mir"); REQUIRE(input);
  std::string text((std::istreambuf_iterator<char>(input)),{}); auto scanner = serialization::parse_text(text); REQUIRE(scanner); compare(**scanner);
}
