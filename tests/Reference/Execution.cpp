/* Licensed under the MIT License. Independent C interpreter/JIT execution oracle. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mir.h"
#include "mir-gen.h"
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/Interpreter.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include "../Execution/InterpreterFixtures.hpp"
#include "../Execution/NativeTarget.hpp"
#include "../Execution/SeededPrograms.hpp"
#include "../Fixtures/ClassicPrograms.hpp"
#include "../Fixtures/UtilityPrograms.hpp"
#include "../IR/Calls.hpp"
#include "../IR/Variables.hpp"
#include "../IR/StackScopes.hpp"
#include <array>
using namespace mirnext; using namespace mirnext::ir;
namespace {
bool native_available() { return test::native_execution_available(); }
struct Oracle {
  MIR_context_t context = MIR_init(); bool jit,blocks;
  Oracle(const Context &source,unsigned level,bool jit,bool finite_text = false,bool block_versioning = false) : jit(jit),blocks(block_versioning) {
    // The binary format preserves all floating bit patterns and does not
    // depend on the C scanner's spelling/long-double limitations.
    if (finite_text) {
      auto text = serialization::write_text(source); REQUIRE(text); MIR_scan_string(context,text->c_str());
    } else {
    auto binary = serialization::write_binary(source); REQUIRE(binary);
    auto file = std::tmpfile(); REQUIRE(file); REQUIRE(std::fwrite(binary->data(),1,binary->size(),file) == binary->size());
    std::rewind(file); MIR_read(context,file); std::fclose(file); }
    if (jit) { MIR_gen_init(context); MIR_gen_set_optimize_level(context,level); }
    for (auto m = DLIST_HEAD(MIR_module_t,*MIR_get_module_list(context)); m; m = DLIST_NEXT(MIR_module_t,m)) MIR_load_module(context,m);
    MIR_link(context,jit ? (block_versioning ? MIR_set_lazy_bb_gen_interface : MIR_set_gen_interface) : MIR_set_interp_interface,nullptr);
  }
  ~Oracle() { if (jit) MIR_gen_finish(context); MIR_finish(context); }
  MIR_item_t find(std::string_view module,std::string_view name) {
    for (auto m = DLIST_HEAD(MIR_module_t,*MIR_get_module_list(context)); m; m = DLIST_NEXT(MIR_module_t,m)) if (module == m->name)
      for (auto i = DLIST_HEAD(MIR_item_t,m->items); i; i = DLIST_NEXT(MIR_item_t,i)) if (auto n = MIR_item_name(context,i); n && name == n) return i;
    FAIL("missing C oracle symbol"); return nullptr;
  }
  std::int64_t call(std::string_view module,std::string_view name,std::span<const std::int64_t> arguments = {}) {
    auto f = find(module,name); REQUIRE(f);
    while (f->ref_def) f = f->ref_def;
    REQUIRE(f->item_type == MIR_func_item);
    if (jit) {
      auto code = blocks ? f->addr : MIR_gen(context,f);
      if (arguments.empty()) return reinterpret_cast<std::int64_t (*)()>(code)();
      REQUIRE(arguments.size() == 1); return reinterpret_cast<std::int64_t (*)(std::int64_t)>(code)(arguments[0]);
    }
    std::vector<MIR_val_t> values(arguments.size()); for (std::size_t n = 0; n < arguments.size(); ++n) values[n].i = arguments[n];
    MIR_val_t result; MIR_interp_arr(context,f,&result,values.size(),values.data()); return result.i;
  }
  std::uint64_t memory_call(std::uint64_t input,std::uint64_t *left,std::uint64_t *right) {
    auto f = find("main","entry"); REQUIRE(f);
    while (f->ref_def) f = f->ref_def;
    REQUIRE(f->item_type == MIR_func_item);
    if (jit) {
      auto address = blocks ? f->addr : MIR_gen(context,f);
      return reinterpret_cast<std::uint64_t (*)(std::uint64_t,std::uint64_t *,std::uint64_t *)>(address)(input,left,right);
    }
    std::array<MIR_val_t,3> args{}; args[0].u = input;
    args[1].u = reinterpret_cast<std::uintptr_t>(left); args[2].u = reinterpret_cast<std::uintptr_t>(right);
    MIR_val_t result{}; MIR_interp_arr(context,f,&result,args.size(),args.data()); return result.u;
  }
  execution::ScalarValue scalar(std::string_view module,std::string_view name,Type result_type) {
    auto f = find(module,name); REQUIRE(f); while (f->ref_def) f = f->ref_def;
    REQUIRE(f->item_type == MIR_func_item); REQUIRE(f->u.func->nres == 1); auto type = f->u.func->res_types[0];
    MIR_val_t result{};
    if (!jit) MIR_interp_arr(context,f,&result,0,nullptr);
    else {
      auto code = blocks ? f->addr : MIR_gen(context,f);
      if (type == MIR_T_F) result.f = reinterpret_cast<float (*)()>(code)();
      else if (type == MIR_T_D) result.d = reinterpret_cast<double (*)()>(code)();
      else if (type == MIR_T_LD) result.ld = reinterpret_cast<long double (*)()>(code)();
      else result.i = reinterpret_cast<std::int64_t (*)()>(code)();
    }
    if (type == MIR_T_F) return execution::ScalarValue(result.f);
    if (type == MIR_T_LD) return execution::ScalarValue(result.ld);
    if (type == MIR_T_D) return result_type.kind() == Type::Kind::LongDouble
      ? execution::ScalarValue(static_cast<long double>(result.d)) : execution::ScalarValue(result.d);
    return execution::ScalarValue(result.i);
  }
};
void compare(const Context &context,std::string_view module,std::string_view name,unsigned level,unsigned repeats = 1) {
  auto session = execution::ExecutionSession::create(context,{.optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)}});
  INFO((session ? "" : session.error().message)); REQUIRE(session); auto f = session->find_function(module,name); REQUIRE(f);
  const auto initializer_entries = session->host_entry_count();
  auto lazy = execution::ExecutionSession::create(context,{.optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)},
    .link={.compilation=execution::JITCompilation::Lazy}}); REQUIRE(lazy); auto lf = lazy->find_function(module,name); REQUIRE(lf);
  auto bbv = execution::ExecutionSession::create(context,{.optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)},
    .link={.compilation=execution::JITCompilation::BlockVersioning}}); INFO((bbv ? "" : bbv.error().message)); REQUIRE(bbv);
  auto bf = bbv->find_function(module,name); REQUIRE(bf);
  auto mixed = execution::ExecutionSession::create(context,{.optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)},
    .select_engine=[=](const Function &f) -> Result<execution::ExecutionEngine> {
      return f.module().name() == module && f.symbol().name() == name ? execution::ExecutionEngine::Interpreter : execution::ExecutionEngine::LazyJIT;
    }}); INFO((mixed ? "" : mixed.error().message)); REQUIRE(mixed); auto mf = mixed->find_function(module,name); REQUIRE(mf);
  auto mixed_blocks = execution::ExecutionSession::create(context,{.optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)},
    .select_engine=[=](const Function &f) -> Result<execution::ExecutionEngine> {
      return f.module().name() == module && f.symbol().name() == name ? execution::ExecutionEngine::BlockVersioning : execution::ExecutionEngine::Interpreter;
    }}); INFO((mixed_blocks ? "" : mixed_blocks.error().message)); REQUIRE(mixed_blocks); auto mbf = mixed_blocks->find_function(module,name); REQUIRE(mbf);
  Oracle interpreter(context,level,false), jit(context,level,true), legacy_blocks(context,level,true,false,true);
  auto raw = execution::InterpretedProgram::create(context); REQUIRE(raw); auto rf = raw->find_function(module,name); REQUIRE(rf);
  auto optimized = transforms::optimize(context,{.level=static_cast<transforms::OptimizationLevel>(level)}); REQUIRE(optimized);
  auto native = execution::InterpretedProgram::create(std::move(*optimized)); REQUIRE(native); auto nf = native->find_function(module,name); REQUIRE(nf);
  auto original = dump(context);
  for (unsigned n = 0; n < repeats; ++n) {
    auto expected = interpreter.call(module,name), compiled = jit.call(module,name); auto value = session->call_i64(*f);
    INFO((value ? "" : value.error().message)); REQUIRE(value); CHECK(*value == expected); CHECK(*value == compiled);
    auto delayed = lf->call_i64(); INFO((delayed ? "" : delayed.error().message)); REQUIRE(delayed); CHECK(*delayed == expected);
    auto blocks = bf->call_i64(); INFO((blocks ? "" : blocks.error().message)); REQUIRE(blocks); CHECK(*blocks == expected); CHECK(*blocks == compiled);
    CHECK(*blocks == legacy_blocks.call(module,name));
    auto mixed_result = mf->call_i64(),mixed_block_result = mbf->call_i64();
    INFO((mixed_result ? "" : mixed_result.error().message)); REQUIRE(mixed_result);
    INFO((mixed_block_result ? "" : mixed_block_result.error().message)); REQUIRE(mixed_block_result);
    CHECK(*mixed_result == expected); CHECK(*mixed_block_result == expected);
    auto interpreted = raw->call_i64(*rf), transformed = native->call_i64(*nf); REQUIRE(interpreted); REQUIRE(transformed);
    CHECK(*interpreted == expected); CHECK(*transformed == expected);
  }
  CHECK(dump(context) == original); CHECK(session->host_entry_count() == initializer_entries+1);
}
TEST_CASE("Windows stack block arguments consume copied values before scalar argument setup") {
  if (!native_available() || target::TargetMachine::native()->abi() != target::ABI::X86Windows) return;
  for (unsigned size : {8,16,24}) for (bool stack : {false,true}) {
    CAPTURE(size); CAPTURE(stack);
    auto prefix = stack ? std::string("i64:a,i64:b,i64:c,i64:d,") : std::string();
    auto text = "m:module\nbytes:i64 20,0,0\np:proto i64," + prefix + "blk:" + std::to_string(size) + "(block),i64:last\n"
      "read:func i64," + prefix + "blk:" + std::to_string(size) + "(block),i64:last\nlocal i64:r,i64:t\nmov r,i64:0(block)\n";
    for (unsigned offset = 8; offset < size; offset += 8)
      text += "mov t,i64:" + std::to_string(offset) + "(block)\nadd r,r,t\n";
    if (stack) text += "add r,r,a\nadd r,r,b\nadd r,r,c\nadd r,r,d\n";
    text += "add r,r,last\nmov i64:0(block),99\nret r\nendfunc\nentry:func i64\nlocal i64:address,i64:r\nmov address,bytes\ncall p,read,r,";
    if (stack) text += "1,2,3,4,";
    text += "blk:" + std::to_string(size) + "(address)," + (stack ? "12" : "22") + "\nret r\nendfunc\nendmodule\n";
    auto source = serialization::parse_text(text); REQUIRE(source);
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE(level); Oracle original(**source,level,false); CHECK(original.call("m","entry") == 42);
      compare(**source,"m","entry",level,2);
    }
  }
}
TEST_CASE("owning interpreter scalar and comparison decisions match C interpreter and JIT") {
  for (unsigned fixture = 0; fixture < 3; ++fixture) {
  CAPTURE(fixture);
  auto source = fixture == 0 ? test::interpreter_scalars() : fixture == 1 ? test::interpreter_ieee() : test::interpreter_overflow();
  auto module = source->modules().begin()->name();
  for (unsigned level = 0; level < 4; ++level) {
    Oracle interpreter(*source,level,false,fixture == 0),jit(*source,level,true,fixture == 0);
    auto raw = execution::InterpretedProgram::create(*source); REQUIRE(raw);
    auto optimized = transforms::optimize(*source,{.level=static_cast<transforms::OptimizationLevel>(level)}); REQUIRE(optimized);
    auto transformed = execution::InterpretedProgram::create(std::move(*optimized)); REQUIRE(transformed);
    auto session = execution::ExecutionSession::create(*source,{.mode=execution::ExecutionMode::Interpreter,
      .optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)}}); REQUIRE(session);
    std::optional<execution::ExecutionSession> compiled,entries,lazy;
    if (native_available()) {
      auto native = execution::ExecutionSession::create(*source,{.optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)}});
      INFO((native ? "" : native.error().message)); REQUIRE(native); compiled.emplace(std::move(*native));
      auto delayed = execution::ExecutionSession::create(*source,{.optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)},
        .link={.compilation=execution::JITCompilation::Lazy}}); REQUIRE(delayed); lazy.emplace(std::move(*delayed));
      auto published = execution::ExecutionSession::create(*source,{.mode=execution::ExecutionMode::Interpreter,
        .optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)},.native_entries=true});
      INFO((published ? "" : published.error().message)); REQUIRE(published); entries.emplace(std::move(*published));
    }
    for (auto &m : source->modules()) for (auto &fn : m.functions()) {
      CAPTURE(level); INFO(fn.symbol().name());
      auto type = fn.signature().results[0]; auto a = interpreter.scalar(module,fn.symbol().name(),type),b = jit.scalar(module,fn.symbol().name(),type);
      auto rf = raw->find_function(module,fn.symbol().name()),tf = transformed->find_function(module,fn.symbol().name()); REQUIRE(rf); REQUIRE(tf);
      auto rv = raw->call(*rf),tv = transformed->call(*tf); REQUIRE(rv); REQUIRE(tv);
      std::vector<execution::ScalarValue> actuals{(*rv)[0],(*tv)[0],b};
      auto sf = session->find_function(module,fn.symbol().name()); REQUIRE(sf); auto sr = session->call(*sf); REQUIRE(sr); actuals.push_back((*sr)[0]);
      if (compiled) {
        auto cf = compiled->find_function(module,fn.symbol().name()); REQUIRE(cf); auto cr = compiled->call(*cf);
        INFO((cr ? "" : cr.error().message)); REQUIRE(cr); actuals.push_back((*cr)[0]);
      }
      if (lazy) {
        auto cf = lazy->find_function(module,fn.symbol().name()); REQUIRE(cf); auto cr = cf->call();
        INFO((cr ? "" : cr.error().message)); REQUIRE(cr); actuals.push_back((*cr)[0]);
      }
      if (entries) {
        auto ef = entries->find_function(module,fn.symbol().name()); REQUIRE(ef); auto e = entries->native_entry(*ef); REQUIRE(e);
        execution::ExecutionInvocation invocation;
        if (type.is_integer()) actuals.emplace_back(reinterpret_cast<std::uint64_t (*)()>(e->address())());
        else if (type.kind() == Type::Kind::F32) actuals.emplace_back(reinterpret_cast<float (*)()>(e->address())());
        else if (type.kind() == Type::Kind::F64) actuals.emplace_back(reinterpret_cast<double (*)()>(e->address())());
        else actuals.emplace_back(reinterpret_cast<long double (*)()>(e->address())());
        auto valid = invocation.status(); INFO((valid ? "" : valid.error().message)); REQUIRE(valid);
      }
      unsigned engine = 0;
      for (auto &actual : actuals) {
        CAPTURE(engine);
        if (type.is_integer()) CHECK(*actual.as_u64() == *a.as_u64());
        else if (type.kind() == Type::Kind::F32) CHECK(std::bit_cast<std::uint32_t>(*actual.as_float()) == std::bit_cast<std::uint32_t>(*a.as_float()));
        else if (type.kind() == Type::Kind::F64) CHECK(std::bit_cast<std::uint64_t>(*actual.as_double()) == std::bit_cast<std::uint64_t>(*a.as_double()));
        else CHECK(*actual.as_long_double() == *a.as_long_double());
        ++engine;
      }
    }
  }
  }
}
}
TEST_CASE("owning BBV session property branches execute like the independent C BBV engine") {
  for (unsigned level = 0; level < 4; ++level) for (auto branch : {"prbeq","prbne"}) for (unsigned property : {0,1}) {
    CAPTURE(level); CAPTURE(branch); CAPTURE(property);
    auto text = std::string("m:module\nf:func i64,i64:x\nprset x,1\n")+branch+" yes,x,"+std::to_string(property)+"\nret 17\nyes:ret 42\nendfunc\nendmodule\n";
    auto c = serialization::parse_text(text); REQUIRE(c);
    Oracle reference(**c,level,true,false,true);
    auto s = execution::ExecutionSession::create(**c,{.optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)},.link={.compilation=execution::JITCompilation::BlockVersioning}}); REQUIRE(s);
    auto f = s->find_function("m","f"); REQUIRE(f);
    for (std::int64_t x : {0,1,42,-1}) { std::array args{x}; auto value = f->call_i64(args); INFO((value ? "" : value.error().message)); REQUIRE(value); CHECK(*value == reference.call("m","f",args)); }
    CHECK_FALSE(f->artifact()); REQUIRE(f->block_statistics()); CHECK(f->block_statistics()->compiled_blocks > 0);
  }
}
TEST_CASE("seeded memory corpus matches C interpretation and O0 O1 before the original address defect") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  // This shared variant has no cross-function hardware side effects; the
  // native matrix also covers those with checked host ABI boundaries.
  auto fixtures = test::seeded_programs({},false);
  for (auto &fixture : fixtures) {
    CAPTURE(fixture.seed); INFO(fixture.text);
    auto source = serialization::parse_text(fixture.text); REQUIRE(source);
    // Original C O2/O3 replace this partial store with EXT32 and lose the high
    // bytes. The separate regression below records that optimizer defect.
    for (unsigned level = 0; level < 2; ++level) for (unsigned engine = 0; engine < 3; ++engine) {
      CAPTURE(level); CAPTURE(engine); Oracle oracle(**source,level,engine != 0,false,engine == 2);
      for (auto input : {std::uint64_t{0},UINT64_C(0x7fffffffffffffff),UINT64_C(0x8000000000000000)}) for (unsigned alias : {0,2}) {
        CAPTURE(input); CAPTURE(alias); auto memory = fixture.initial_memory(),expected = memory;
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
          CAPTURE(repeat); auto value = fixture.evaluate(input,expected,alias);
          CHECK(oracle.memory_call(input,memory.data(),memory.data()+alias) == value); CHECK(memory == expected);
        }
      }
    }
  }
}
TEST_CASE("original C partial local stores have a recorded O2 O3 address conversion defect") {
  auto source = serialization::parse_text("m:module\nf:func i64\nlocal i64:v,p:p,i64:r\n"
    "mov v,1311768467463790320\naddr p,v\nmov i32:(p),1\nmov r,i64:(p)\nret r\nendfunc\nendmodule\n"); REQUIRE(source);
  constexpr std::uint64_t expected = 0x1234567800000001ULL;
  for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(level); Oracle interpreter(**source,level,false),jit(**source,level,true);
    CHECK(std::uint64_t(interpreter.call("m","f")) == expected);
    // This asserts the known original result, not the native implementation's
    // required behavior. Native tests require preserved bytes at every level.
    CHECK(std::uint64_t(jit.call("m","f")) == (level < 2 ? expected : 1));
  }
}
TEST_CASE("owning sessions match independent C interpretation and JIT for calls and loaded data") {
  if (!native_available()) return;
  for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(level);
    for (unsigned mode = 0; mode <= static_cast<unsigned>(test::CallExample::Inline); ++mode) {
      CAPTURE(mode); auto context = test::call_example(static_cast<test::CallExample>(mode));
      if (test::check_native_result_limit(*context,{.optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)}})) continue;
      compare(*context,"calls","entry",level);
    }
    for (unsigned mode = 0; mode <= static_cast<unsigned>(test::VariableExample::Narrow); ++mode) {
      CAPTURE(mode); auto context = test::variable_example(static_cast<test::VariableExample>(mode)); compare(*context,"consumer","entry",level,3);
    }
    for (auto mode : {test::ScopeTransfer::Branch,test::ScopeTransfer::Switch,test::ScopeTransfer::Indirect}) {
      CAPTURE(mode); auto context = test::stack_scope_example(mode); compare(*context,"scopes","entry",level);
    }
  }
}
TEST_CASE("owning session typed argument boundary matches independent C at narrow integer limits") {
  if (!native_available()) return;
  auto context = serialization::parse_text("m:module\nf:func i8,i8:x\nlocal i64:r\nadd r,x,7\nret r\nendfunc\nendmodule\n"); REQUIRE(context);
  for (unsigned level = 0; level < 4; ++level) {
    auto session = execution::ExecutionSession::create(**context,{.optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)}}); REQUIRE(session);
    auto f = session->find_function("m","f"); REQUIRE(f); Oracle interpreter(**context,level,false), jit(**context,level,true);
    auto native = execution::InterpretedProgram::create(**context); REQUIRE(native); auto nf = native->find_function("m","f"); REQUIRE(nf);
    auto callbacks = execution::ExecutionSession::create(**context,{.mode=execution::ExecutionMode::Interpreter,
      .optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)},.native_entries=true}); REQUIRE(callbacks);
    auto cf = callbacks->find_function("m","f"); REQUIRE(cf); auto entry = callbacks->native_entry(*cf); REQUIRE(entry);
    for (std::int64_t argument : {-129,-128,-7,0,120,127,128,255}) {
      std::array arguments{argument}; auto value = session->call_i64(*f,arguments); REQUIRE(value);
      CHECK(*value == interpreter.call("m","f",arguments)); CHECK(*value == jit.call("m","f",arguments));
      auto result = native->call_i64(*nf,arguments); REQUIRE(result); CHECK(*result == *value);
      execution::ExecutionInvocation invocation;
      auto returned = reinterpret_cast<std::int8_t (*)(std::int8_t)>(entry->address())(std::bit_cast<std::int8_t>(std::uint8_t(argument)));
      CHECK(std::int64_t(returned) == *value); CHECK(invocation.status());
    }
    CHECK(session->host_entry_count() == 1);
  }
}
TEST_CASE("classic loop and sieve results retain independent original C execution checks") {
  auto loop = test::classic_loop(); REQUIRE(loop);
  auto sieve = serialization::parse_text(test::classic_sieve_text(127,2)); REQUIRE(sieve);
  for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(level); Oracle loop_interpreter(**loop,level,false),loop_jit(**loop,level,true);
    for (std::int64_t n : {-10,0,1,257}) {
      std::array args{n}; auto expected = n < 0 ? 0 : n;
      CHECK(loop_interpreter.call("m","loop",args) == expected); CHECK(loop_jit.call("m","loop",args) == expected);
    }
    Oracle sieve_interpreter(**sieve,level,false),sieve_jit(**sieve,level,true);
    // The original counts 4 and omits 2; retain its count, pi(127) = 31.
    CHECK(sieve_interpreter.call("m_sieve","sieve") == 31); CHECK(sieve_jit.call("m_sieve","sieve") == 31);
  }
}
namespace { std::int32_t utility_callback_length(const char *text) { return static_cast<std::int32_t>(std::string_view(text).size()); } }
TEST_CASE("unregistered hello callback pointers retain original C interpreter and JIT behavior") {
  auto source = test::utility_hello(); REQUIRE(source);
  for (unsigned level = 0; level < 4; ++level) for (bool jit : {false,true}) {
    CAPTURE(level); CAPTURE(jit); Oracle oracle(**source,level,jit); auto f = oracle.find("hello","hello"); REQUIRE(f);
    while (f->ref_def) f = f->ref_def; REQUIRE(f->item_type == MIR_func_item);
    for (std::int32_t id : {0,1}) {
      auto text = id ? "goodbye" : "hello"; std::int32_t value;
      if (jit) value = reinterpret_cast<std::int32_t (*)(const char *,std::int32_t (*)(const char *),std::int32_t)>(MIR_gen(oracle.context,f))(text,&utility_callback_length,id);
      else {
        MIR_val_t arguments[3]{},result{}; arguments[0].u = reinterpret_cast<std::uintptr_t>(text);
        arguments[1].u = reinterpret_cast<std::uintptr_t>(&utility_callback_length); arguments[2].i = id;
        MIR_interp_arr(oracle.context,f,&result,3,arguments); value = static_cast<std::int32_t>(result.i);
      }
      CHECK(value == 10);
    }
  }
}
