/* Licensed under the MIT License. Owning lazy JIT, without the C runtime. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include "mirnext/CodeGen/RuntimeEntry.hpp"
#include "../IR/Calls.hpp"
#include "../IR/Variables.hpp"
#include <array>
#include <bit>
using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::execution;
namespace {
bool windows() { return target::TargetMachine::native()->abi() == target::ABI::X86Windows; }
auto parse(std::string text) {
  if (target::TargetMachine::native()->architecture() == target::Architecture::X86_64) {
    for (const auto &[from,to] : {std::pair{":r5",":r9"},std::pair{":v20",":xmm3"},std::pair{":r19",":r12"}})
      for (auto position = text.find(from); position != std::string::npos; position = text.find(from,position+std::string_view(to).size())) text.replace(position,std::string_view(from).size(),to);
  }
  auto c = serialization::parse_text(text); INFO((c ? "" : c.error().message)); REQUIRE(c); return std::move(*c);
}
ExecutionSession lazy(const Context &c,unsigned level = 2,LinkOptions options = {}) {
  options.compilation = JITCompilation::Lazy;
  auto s = ExecutionSession::create(c,{.optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)},.link=std::move(options)});
  INFO((s ? "" : s.error().message)); REQUIRE(s); return std::move(*s);
}
FunctionHandle find(const ExecutionSession &s,std::string_view module,std::string_view name) {
  auto f = s.find_function(module,name); REQUIRE(f); return *f;
}
auto call(const FunctionHandle &f,std::span<const ExecutionArgument> args = {}) {
  auto r = f.call(args); INFO((r ? "" : r.error().message)); REQUIRE(r); return std::move(*r);
}
}
TEST_CASE("lazy JIT leaves bodies cold and retains stable entries after source and session destruction") {
  unsigned compilations = 0; std::optional<FunctionHandle> retained; std::optional<NativeEntry> entry;
  {
    auto c = test::variable_example(test::VariableExample::Initialized);
    auto s = lazy(*c,2,{.compilation_observer=[&](const auto &event) -> Result<void> {
      if (event.function.module_name() == "consumer" && event.stage == codegen::CompilationStage::PrepareMachineIR) ++compilations; return {};
    }}); retained = find(s,"consumer","entry"); auto e = retained->native_entry(); REQUIRE(e); entry = std::move(*e);
    CHECK(retained->compilation_state() == FunctionCompilationState::Uncompiled); CHECK_FALSE(retained->artifact());
    CHECK(s.statistics().compiled_functions == 0); CHECK(compilations == 0); CHECK(s.statistics().code_batches == 1);
  }
  auto address = entry->address(); auto result = retained->call_i64(); INFO((result ? "" : result.error().message)); REQUIRE(result); CHECK(*result == 42);
  CHECK(retained->compilation_state() == FunctionCompilationState::Compiled); REQUIRE(retained->artifact()); auto artifact = retained->artifact();
  REQUIRE(retained->compile()); CHECK(retained->artifact() == artifact); CHECK(compilations == 1);
  auto another = retained->native_entry(); REQUIRE(another); CHECK(another->address() == address);
  retained.reset(); ExecutionInvocation invocation; CHECK(reinterpret_cast<std::int64_t (*)()>(address)() == 64); CHECK(invocation.status()); CHECK(compilations == 1);
}
TEST_CASE("lazy JIT preserves every call ABI fixture through cold and warm native entries at O0-O3") {
  for (unsigned level = 0; level < 4; ++level) for (unsigned n = 0; n <= unsigned(test::CallExample::Inline); ++n) {
    CAPTURE(level); CAPTURE(n); auto kind = static_cast<test::CallExample>(n); auto c = test::call_example(kind); auto s = lazy(*c,level);
    auto f = find(s,"calls","sum"); CHECK(f.compilation_state() == FunctionCompilationState::Uncompiled);
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
      std::array<std::int64_t,2> bytes{20,22}; std::vector<ExecutionArgument> args;
      if (kind == test::CallExample::Block) args.push_back(ExecutionArgument::block(std::as_bytes(std::span(bytes))));
      else if (kind == test::CallExample::ReturnBlock) args.push_back(ExecutionArgument::return_buffer(std::as_writable_bytes(std::span(bytes))));
      else if (kind == test::CallExample::VariadicBlock) { args.emplace_back(0); args.push_back(ExecutionArgument::block(std::as_bytes(std::span(bytes)))); }
      else { if (kind == test::CallExample::Variadic) args.emplace_back(0); args.emplace_back(20); args.emplace_back(22); }
      if (windows() && kind == test::CallExample::MultipleResults) {
        auto unsupported = f.call(args); REQUIRE_FALSE(unsupported);
        CHECK(unsupported.error().message.find("Windows does not support multiple return values") != std::string::npos);
        CHECK(f.compilation_state() == FunctionCompilationState::Uncompiled); break;
      }
      auto values = call(f,args); CHECK(f.compilation_state() == FunctionCompilationState::Compiled);
      if (kind == test::CallExample::ReturnBlock) { CHECK(values.empty()); CHECK(bytes[0] == 42); }
      else if (kind == test::CallExample::MultipleResults) { REQUIRE(values.size() == 2); CHECK(*values[0].as_i64() == 20); CHECK(*values[1].as_i64() == 22); }
      else { REQUIRE(values.size() == 1); CHECK(*values[0].as_i64() == 42); }
    }
    if (windows() && kind == test::CallExample::MultipleResults) { CHECK(s.statistics().compiled_functions == 0); continue; }
    CHECK(s.statistics().compiled_functions == 1);
    // Compile a different record without invalidating an already borrowed artifact.
    auto artifact = f.artifact(); REQUIRE(find(s,"calls","entry").compile()); CHECK(f.artifact() == artifact); REQUIRE(artifact->verify());
  }
}
TEST_CASE("lazy JIT resolves cold indirect calls while preserving bound integer and floating registers") {
  auto c = parse("m:module\np:proto i64\ncallee:func i64\nglobal i64:h:r5,d:v:v20\nlocal i64:r\nd2i r,v\nadd r,r,h\nret r\nendfunc\nf:func i64,i64:address\nglobal i64:h:r5,d:v:v20\nlocal i64:r\nmov h,20\ndmov v,22.0\ncall p,address,r\nret r\nendfunc\nunused:func i64\nret 99\nendfunc\nendmodule\n");
  for (unsigned level = 0; level < 4; ++level) {
    auto s = lazy(*c,level); auto f = find(s,"m","f"),callee = find(s,"m","callee"),unused = find(s,"m","unused");
    auto entry = callee.native_entry(); REQUIRE(entry); std::array args{ExecutionArgument(ScalarValue::pointer(entry->address()))};
    CHECK(*call(f,args)[0].as_i64() == 42); CHECK(s.statistics().compiled_functions == 2);
    CHECK(unused.compilation_state() == FunctionCompilationState::Uncompiled); CHECK_FALSE(unused.artifact());
    CHECK(*call(f,args)[0].as_i64() == 42); CHECK(s.statistics().compiled_functions == 2);
  }
}
TEST_CASE("lazy JIT preserves cold scalar register banks stack arguments and floating bit patterns") {
  for (unsigned count : {1u,8u,9u,17u}) {
    Context c; auto m = c.create_module("m"); REQUIRE(m); auto f = (*m)->create_function("f",{{Type(Type::Kind::F64)},std::vector<Type>(count,Type(Type::Kind::F64))}); REQUIRE(f);
    auto block = (*f)->create_block(); REQUIRE(block); IRBuilder b(**block); REQUIRE(b.ret(Operand(*(*f)->arguments().back())));
    auto s = lazy(c); auto entry = find(s,"m","f"); const auto bits = UINT64_C(0xfff8000000012345);
    std::vector<double> args(count,1); args.back() = std::bit_cast<double>(bits); auto result = entry.call_double(args); REQUIRE(result); CHECK(std::bit_cast<std::uint64_t>(*result) == bits);
    args.back() = 42; result = entry.call_double(args); REQUIRE(result); CHECK(*result == 42); CHECK(s.statistics().compiled_functions == 1);
  }
}
TEST_CASE("lazy JIT cold entries preserve all five block ABI classes and mixed multiple results") {
  for (unsigned level = 0; level < 4; ++level) {
    for (unsigned category = 0; category < 5; ++category) {
      auto type = category ? "blk"+std::to_string(category) : std::string("blk");
      auto c = parse("m:module\nf:func i64,"+type+":16(a)\nlocal i64:r\nmov r,i64:0(a)\nmov i64:0(a),99\nret r\nendfunc\nendmodule\n");
      auto s = lazy(*c,level); auto f = find(s,"m","f"); std::array<std::int64_t,2> bytes{42,7}; auto data = std::as_bytes(std::span(bytes));
      const auto abi = static_cast<Type::BlockClass>(category); std::array invalid{ExecutionArgument::block(data.first(8),abi)};
      CHECK_FALSE(f.call(invalid)); CHECK(f.compilation_state() == FunctionCompilationState::Uncompiled); CHECK(s.host_entry_count() == 0);
      std::array args{ExecutionArgument::block(data,abi)}; CHECK(*call(f,args)[0].as_i64() == 42); CHECK(bytes[0] == 42); CHECK(s.statistics().compiled_functions == 1);
    }
    auto c = parse("m:module\nf:func i64,d,f,ld,i8:x,u16:y,d:a,f:b,ld:c\nlocal i64:r\nadd r,x,y\nret r,a,b,c\nendfunc\nendmodule\n");
    auto s = lazy(*c,level); const auto bits = UINT64_C(0x7ff8000000000042);
    std::array args{ExecutionArgument(255),ExecutionArgument(UINT64_C(0xffff)),ExecutionArgument(std::bit_cast<double>(bits)),ExecutionArgument(-0.0f),ExecutionArgument(2.5L)};
    if (windows()) {
      auto failed = find(s,"m","f").call(args); REQUIRE_FALSE(failed);
      CHECK(failed.error().message.find("Windows does not support multiple return values") != std::string::npos); continue;
    }
    auto values = call(find(s,"m","f"),args); REQUIRE(values.size() == 4); CHECK(*values[0].as_i64() == 65534);
    CHECK(std::bit_cast<std::uint64_t>(*values[1].as_double()) == bits); CHECK(std::bit_cast<std::uint32_t>(*values[2].as_float()) == UINT32_C(0x80000000)); CHECK(*values[3].as_long_double() == 2.5L);
  }
}
TEST_CASE("caller-clobbered module bindings remain implicit native call inputs at every level") {
  // The original C Debug range scan aborts on this valid variadic call. Its
  // CFG includes implicit r5/v20 inputs but the reverse scan drops those bits.
  auto c = parse("m:module\np:proto i64,i64:dummy,...\ncallee:func i64,i64:dummy,...\nglobal i64:h:r5,d:v:v20\nlocal i64:r\nd2i r,v\nadd r,r,h\nret r\nendfunc\nentry:func i64\nglobal i64:h:r5,d:v:v20\nlocal i64:r\nmov h,20\ndmov v,22.0\ncall p,callee,r,0\nret r\nendfunc\nendmodule\n");
  for (auto policy : {JITCompilation::Eager,JITCompilation::Lazy}) for (unsigned level = 0; level < 4; ++level) {
    auto s = ExecutionSession::create(*c,{.optimization=transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)},.link={.compilation=policy}});
    INFO((s ? "" : s.error().message)); REQUIRE(s); auto f = find(*s,"m","entry");
    for (unsigned repeat = 0; repeat < 2; ++repeat) { auto result = f.call_i64(); INFO((result ? "" : result.error().message)); REQUIRE(result); CHECK(*result == 42); }
  }
}
TEST_CASE("lazy JIT shares cross-module aliases function-pointer data and recursive destinations") {
  auto c = parse("a:module\nimport fact\nslot:ref fact,0\nendmodule\nb:module\nexport fact\np:proto i64,i64:n\nfact:func i64,i64:n\nlocal i64:previous,i64:result\nble done,n,1\nsub previous,n,1\ncall p,fact,result,previous\nmul result,result,n\nret result\ndone:ret 1\nendfunc\nendmodule\n");
  for (unsigned level = 0; level < 4; ++level) {
    auto s = lazy(*c,level); auto alias = find(s,"a","fact"),definition = find(s,"b","fact"); auto a = alias.native_entry(),b = definition.native_entry(); REQUIRE(a); REQUIRE(b);
    CHECK(a->address() == b->address()); auto slot = s.find_data("a","slot"); REQUIRE(slot); std::uintptr_t address; std::memcpy(&address,slot->bytes().data(),sizeof address); CHECK(address == a->address());
    CHECK(s.statistics().compiled_functions == 0); std::array<std::int64_t,1> args{6}; auto value = alias.call_i64(args); REQUIRE(value); CHECK(*value == 720);
    CHECK(definition.compilation_state() == FunctionCompilationState::Compiled); CHECK(alias.artifact() == definition.artifact()); CHECK(s.statistics().compiled_functions == 1);
    ExecutionInvocation invocation; CHECK(reinterpret_cast<std::int64_t (*)(std::int64_t)>(address)(5) == 120); CHECK(invocation.status()); CHECK(s.statistics().compiled_functions == 1);
  }
}
TEST_CASE("lazy JIT pins label owners and jump-return bodies while leaving ordinary callers cold") {
  for (bool label_data : {false,true}) {
    auto c = parse(std::string("m:module\np:proto\ncallee:func\nglobal i64:continuation:r19\njret continuation\nendfunc\nentry:func i64\nglobal i64:continuation:r19\nladdr continuation,done\njcall p,callee\nret -1\ndone:ret 42\nendfunc\n")+(label_data ? "slot:lref done,0\n" : "")+"endmodule\n");
    auto s = lazy(*c); auto callee = find(s,"m","callee"),entry = find(s,"m","entry");
    CHECK(callee.compilation_state() == FunctionCompilationState::Compiled); CHECK_FALSE(callee.native_entry());
    CHECK(entry.compilation_state() == (label_data ? FunctionCompilationState::Compiled : FunctionCompilationState::Uncompiled));
    if (label_data) {
      auto data = s.find_data("m","slot"); REQUIRE(data); std::uintptr_t address; std::memcpy(&address,data->bytes().data(),sizeof address);
      auto gate = entry.native_entry(); REQUIRE(gate); const auto artifact = entry.artifact(); REQUIRE(artifact); bool found = false;
      for (auto &label : artifact->labels()) if (label.name == "done") {
        REQUIRE(label.offset+4 <= artifact->bytes().size()); CHECK(std::memcmp(reinterpret_cast<const void *>(address),artifact->bytes().data()+label.offset,4) == 0);
        CHECK(address != gate->address()+label.offset); found = true;
      } CHECK(found);
    }
    auto result = entry.call_i64(); INFO((result ? "" : result.error().message)); REQUIRE(result); CHECK(*result == 42); CHECK(s.statistics().compiled_functions == 2);
  }
}
TEST_CASE("lazy JIT expression initializers materialize only their required native execution") {
  for (unsigned level = 0; level < 4; ++level) {
    auto c = test::variable_example(test::VariableExample::Expression); auto s = lazy(*c,level); CHECK(s.statistics().expressions == 1);
    auto entry = find(s,"consumer","entry"); CHECK(entry.compilation_state() == FunctionCompilationState::Uncompiled);
    auto result = entry.call_i64(); REQUIRE(result); CHECK(*result == 42);
  }
}
TEST_CASE("lazy JIT propagates nested cold failures and retries only the missing callee") {
  auto c = parse("m:module\np:proto i64\ncallee:func i64\nret 42\nendfunc\nentry:func i64,i64:address\nlocal i64:r\ncall p,address,r\nret r\nendfunc\nendmodule\n");
  bool reject = true; unsigned attempts = 0;
  auto s = lazy(*c,2,{.compilation_observer=[&](auto &event) -> Result<void> {
    if (event.function.name() == "callee" && event.stage == codegen::CompilationStage::PrepareMachineIR) {
      ++attempts; if (reject) return Error{ErrorCode::UnknownName,"nested lazy compilation failed"};
    } return {};
  }});
  auto entry = find(s,"m","entry"),callee = find(s,"m","callee"); auto gate = callee.native_entry(); REQUIRE(gate);
  std::array<std::int64_t,1> args{std::bit_cast<std::int64_t>(std::uint64_t(gate->address()))}; auto failed = entry.call_i64(args); REQUIRE_FALSE(failed);
  CHECK(failed.error().message.find("nested lazy compilation failed") != std::string::npos); CHECK(s.statistics().compiled_functions == 1);
  CHECK(entry.compilation_state() == FunctionCompilationState::Compiled); CHECK(callee.compilation_state() == FunctionCompilationState::Uncompiled);
  auto artifact = entry.artifact(); reject = false; auto result = entry.call_i64(args); REQUIRE(result); CHECK(*result == 42);
  CHECK(attempts == 2); CHECK(s.statistics().compiled_functions == 2); CHECK(entry.artifact() == artifact);
}
TEST_CASE("lazy JIT compilation failures retain cold state release mappings and retry without user effects") {
  struct Provider final : CodeMemoryProvider {
    std::shared_ptr<CodeMemoryProvider> native = native_code_memory_provider(); bool fail = false; unsigned maps = 0,unmaps = 0;
    std::size_t page_size() const override { return native->page_size(); }
    Result<MappedCodeMemory> allocate(std::size_t n) override { ++maps; return native->allocate(n); }
    Result<void> release(MappedCodeMemory m) override { ++unmaps; return native->release(m); }
    Result<void> make_writable(MappedCodeMemory m) override { if (fail) return Error{ErrorCode::InvalidArgument,"injected lazy publication failure"}; return native->make_writable(m); }
    Result<void> make_executable(MappedCodeMemory m) override { return native->make_executable(m); }
    Result<void> flush(MappedCodeMemory m) override { return native->flush(m); }
  };
  auto provider = std::make_shared<Provider>(); bool reject = true; unsigned attempts = 0;
  std::optional<FunctionHandle> f; std::optional<LinkedData> data;
  {
    auto c = test::variable_example(test::VariableExample::Initialized);
    auto s = lazy(*c,2,{.memory_provider=provider,.compilation_observer=[&](auto &event) -> Result<void> {
      if (event.function.module_name() == "consumer" && event.stage == codegen::CompilationStage::PrepareMachineIR) {
        ++attempts; CHECK(f->compilation_state() == FunctionCompilationState::Compiling);
        auto recursive = f->compile(); REQUIRE_FALSE(recursive); CHECK(recursive.error().message.find("recursive compilation") != std::string::npos);
        if (reject) return Error{ErrorCode::UnknownName,"injected lazy observer failure"};
      } return {};
    }}); f = find(s,"consumer","entry"); auto d = s.find_data("consumer","value"); REQUIRE(d); data = std::move(*d);
  }
  auto read = [&] { std::int64_t v; std::memcpy(&v,data->bytes().data(),sizeof v); return v; };
  auto failed = f->call_i64(); REQUIRE_FALSE(failed); CHECK(failed.error().message.find("lazy observer failure") != std::string::npos);
  CHECK(read() == 20); CHECK(f->compilation_state() == FunctionCompilationState::Uncompiled); CHECK_FALSE(f->artifact()); CHECK(attempts == 1);
  CHECK(provider->maps == 2); CHECK(provider->unmaps == 0);
  reject = false; provider->fail = true; failed = f->call_i64(); REQUIRE_FALSE(failed); CHECK(failed.error().message.find("lazy publication failure") != std::string::npos);
  CHECK(read() == 20); CHECK(f->compilation_state() == FunctionCompilationState::Uncompiled); CHECK(attempts == 2); CHECK(provider->unmaps == 1);
  provider->fail = false; auto result = f->call_i64(); REQUIRE(result); CHECK(*result == 42); CHECK(attempts == 3);
  result = f->call_i64(); REQUIRE(result); CHECK(*result == 64); CHECK(attempts == 3); CHECK(provider->maps == 4);
  f.reset(); data.reset(); CHECK(provider->unmaps == provider->maps);
}
TEST_CASE("lazy JIT rejects interpreter policies and invalid entry addresses") {
  Context c; auto s = ExecutionSession::create(c,{.mode=ExecutionMode::Interpreter,.link={.compilation=JITCompilation::Lazy}}); CHECK_FALSE(s);
  CHECK_FALSE(InterpretedProgram::create(c,{.link={.compilation=JITCompilation::Lazy}}));
  auto target = target::TargetMachine::native(); REQUIRE(target); CHECK_FALSE(codegen::make_lazy_entry(*target,{0,0,0}));
  auto p = LinkedProgram::create(c,{.link={.compilation=static_cast<JITCompilation>(99)}}); CHECK_FALSE(p);
}
TEST_CASE("lazy failures preserve zero one and two long-double result stacks across repeated retries") {
  for (unsigned count = 0; count < 3; ++count) for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(count); CAPTURE(level);
    auto text = std::string("m:module\nf:func") + (count == 0 ? "" : count == 1 ? " ld" : " ld,ld")
      + "\nret" + (count == 0 ? "" : count == 1 ? " 2.5l" : " 2.5l,3.5l") + "\nendfunc\nendmodule\n";
    auto c = parse(text); bool reject = true; unsigned attempts = 0;
    auto s = lazy(*c,level,{.compilation_observer=[&](auto &event) -> Result<void> {
      if (event.function.name() == "f" && event.stage == codegen::CompilationStage::PrepareMachineIR) {
        ++attempts; if (reject) return Error{ErrorCode::InvalidArgument,"cold extended-result failure"};
      } return {};
    }});
    auto f = find(s,"m","f");
    if (windows() && count == 2) {
      auto failed = f.call(); REQUIRE_FALSE(failed);
      CHECK(failed.error().message.find("Windows does not support multiple return values") != std::string::npos); continue;
    }
    for (unsigned n = 0; n < 20; ++n) {
      auto failed = f.call(); REQUIRE_FALSE(failed); CHECK(failed.error().message.find("cold extended-result failure") != std::string::npos);
      CHECK(f.compilation_state() == FunctionCompilationState::Uncompiled); CHECK_FALSE(f.artifact());
    }
    CHECK(attempts == 20); reject = false;
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
      auto values = call(f); REQUIRE(values.size() == count);
      if (count) CHECK(*values[0].as_long_double() == 2.5L);
      if (count == 2) CHECK(*values[1].as_long_double() == 3.5L);
    }
    CHECK(attempts == 21); CHECK(f.compilation_state() == FunctionCompilationState::Compiled);
  }
}
TEST_CASE("lazy JIT publishes one shared resolver rather than duplicating register saves per function") {
  constexpr unsigned count = 128; Context c; auto m = c.create_module("many"); REQUIRE(m);
  for (unsigned n = 0; n < count; ++n) {
    auto f = (*m)->create_function("f"+std::to_string(n),{{Type::i64()},{}}); REQUIRE(f); auto b = (*f)->create_block(); REQUIRE(b); REQUIRE(IRBuilder(**b).ret(std::int64_t(n)));
  }
  auto s = lazy(c); CHECK(s.statistics().functions == count); CHECK(s.statistics().compiled_functions == 0); CHECK(s.statistics().code_batches == 1);
  CHECK(s.statistics().code_bytes <= count*64+1024);
  auto f = find(s,"many","f127"); auto result = f.call_i64(); REQUIRE(result); CHECK(*result == 127);
  CHECK(s.statistics().compiled_functions == 1); CHECK(find(s,"many","f0").compilation_state() == FunctionCompilationState::Uncompiled);
}
