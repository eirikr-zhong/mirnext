/* Licensed under the MIT License. Owning BBV compilation and publication. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Execution/detail/BBVRuntime.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include "mirnext/Execution/Builtins.hpp"
#include "mirnext/Execution/detail/NativeCalls.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include <array>
#include <set>
#include "../IR/Calls.hpp"
using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::codegen; using namespace mirnext::execution;
namespace {
using Runtime = execution::detail::BBVRuntime;
using Options = execution::detail::BBVRuntimeOptions;
auto host() { auto target = target::TargetMachine::native(); REQUIRE(target); return *target; }
// Unexpected failure while executing a success-only test must not RET from an
// arbitrary frame. Actual failure-path tests below use direct checked compile.
[[noreturn]] void unexpected_failure() { std::abort(); }
BBVProgram prepare(std::string_view text,unsigned level = 0) {
  auto source = serialization::parse_text(text); INFO((source ? "" : source.error().message)); REQUIRE(source);
  auto optimized = transforms::optimize(**source,{.level=static_cast<transforms::OptimizationLevel>(level)}); REQUIRE(optimized);
  auto preparation = BBVPreparation::prepare(*optimized->context().modules().begin()->functions().begin(),host(),{.optimization_level=level});
  INFO((preparation ? "" : preparation.error().message)); REQUIRE(preparation);
  auto program = BBVProgram::create(std::move(*preparation)); REQUIRE(program); return std::move(*program);
}
std::unique_ptr<Runtime> runtime(std::string_view text,unsigned level = 0,Options options = {}) {
  if (!options.code_capacity) options.code_capacity = 1024*1024;
  if (!options.failure_continuation) options.failure_continuation = reinterpret_cast<std::uintptr_t>(&unexpected_failure);
  auto result = Runtime::create(prepare(text,level),std::move(options)); INFO((result ? "" : result.error().message)); REQUIRE(result); return std::move(*result);
}
std::int64_t call(Runtime &r,std::int64_t argument) {
  ExecutionInvocation invocation; auto value = reinterpret_cast<std::int64_t (*)(std::int64_t)>(r.entry_address())(argument);
  auto status = invocation.status(); INFO((status ? "" : status.error().message)); REQUIRE(status); return value;
}
const MachineLabel &label(Runtime &r,std::string_view name) {
  for (auto &l : r.program().preparation().function().labels()) if (l.name() == name) return l;
  FAIL("missing fixture label"); return *r.program().preparation().function().labels().begin();
}
struct Provider final : CodeMemoryProvider {
  std::shared_ptr<CodeMemoryProvider> native = native_code_memory_provider();
  unsigned allocations = 0,releases = 0,writes = 0,seals = 0,flushes = 0;
  unsigned fail_write = 0,fail_seal = 0,fail_flush = 0;
  std::vector<MappedCodeMemory> published,written;
  std::size_t page_size() const override { return native->page_size(); }
  Result<MappedCodeMemory> allocate(std::size_t n) override { ++allocations; return native->allocate(n); }
  Result<void> release(MappedCodeMemory m) override { ++releases; return native->release(m); }
  Result<void> make_writable(MappedCodeMemory m) override {
    ++writes; written.push_back(m);
    for (auto old : published) CHECK((m.data+m.size <= old.data || old.data+old.size <= m.data));
    if (writes == fail_write) return Error{ErrorCode::InvalidArgument,"injected BBV write failure"}; return native->make_writable(m);
  }
  Result<void> make_executable(MappedCodeMemory m) override {
    if (++seals == fail_seal) return Error{ErrorCode::InvalidArgument,"injected BBV seal failure"}; return native->make_executable(m);
  }
  Result<void> flush(MappedCodeMemory m) override {
    if (++flushes == fail_flush) return Error{ErrorCode::InvalidArgument,"injected BBV flush failure"};
    auto result = native->flush(m); if (result) published.push_back(m); return result;
  }
};
constexpr auto branching = "m:module\nf:func i64,i64:x\nlocal p:stack,i64:r\nalloca stack,64\nmov i64:(stack),x\nbt yes,x\nmov r,i64:(stack)\nadd r,r,17\nret r\nyes:mov r,i64:(stack)\nadd r,r,41\nret r\nendfunc\nendmodule\n";
constexpr auto loop = "m:module\nf:func i64,i64:x\nlocal i64:r\nmov r,0\nloop:ble done,x,0\nadd r,r,x\nsub x,x,1\njmp loop\ndone:ret r\nendfunc\nendmodule\n";
}
TEST_CASE("owning BBV runtime leaves unused versions cold and compiles reached blocks once at O0-O3") {
  for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(level); std::set<std::size_t> observed;
    auto provider = std::make_shared<Provider>(); auto r = runtime(branching,level,{.memory_provider=provider,.observe=[&](auto &v,auto &artifact) -> Result<void> {
      CHECK(observed.insert(v.id().index()).second); CHECK(artifact.verify()); return {};
    }});
    auto entry = r->entry_address(); std::vector<std::uint8_t> gate(reinterpret_cast<const std::uint8_t *>(entry),reinterpret_cast<const std::uint8_t *>(entry)+64);
    CHECK(r->statistics().compiled_blocks == 0); CHECK(r->statistics().compilation_attempts == 0); CHECK(r->statistics().code_batches == 1);
    CHECK_FALSE(r->artifact(r->entry_version().id())); CHECK(provider->allocations == 1);
    CHECK(call(*r,0) == 17); auto compiled = r->statistics().compiled_blocks;
    CHECK(compiled > 0); CHECK(compiled < r->statistics().blocks); auto artifact = r->artifact(r->entry_version().id()); REQUIRE(artifact);
    auto before = r->remaining_code_bytes(); CHECK(call(*r,0) == 17); CHECK(r->statistics().compiled_blocks == compiled); CHECK(r->remaining_code_bytes() == before);
    CHECK(call(*r,1) == 42); CHECK(r->statistics().compiled_blocks == compiled+1);
    compiled = r->statistics().compiled_blocks;
    for (auto x : {0,1,2,-1,0,9}) CHECK(call(*r,x) == (x ? x+41 : 17));
    CHECK(r->statistics().compiled_blocks == compiled); CHECK(r->statistics().compilation_attempts == compiled);
    CHECK(r->statistics().code_batches == compiled+1); CHECK(observed.size() == compiled); CHECK(provider->allocations == 1);
    CHECK(r->entry_address() == entry); CHECK(r->artifact(r->entry_version().id()) == artifact);
    CHECK(std::equal(gate.begin(),gate.end(),reinterpret_cast<const std::uint8_t *>(entry)));
    REQUIRE(r->compile(r->entry_version().id())); CHECK(r->statistics().compilation_attempts == compiled);
    auto saved = std::move(r); CHECK(call(*saved,3) == 44); CHECK(provider->releases == 0); saved.reset(); CHECK(provider->releases == 1);
  }
}
TEST_CASE("owning BBV runtime executes cold loop backedges switch tables and stable label-address roots") {
  constexpr auto switches = "m:module\nf:func i64,i64:x\nswitch x,a,b,a\na:ret 11\nb:ret 29\nendfunc\nendmodule\n";
  constexpr auto indirect = "m:module\nf:func i64,i64:x\nlocal p:address\nladdr address,target\njmpi address\ntarget:ret x\nendfunc\nendmodule\n";
  for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(level); auto r = runtime(loop,level);
    for (auto n : {0,1,2,9,20,0}) CHECK(call(*r,n) == n*(n+1)/2);
    CHECK(r->statistics().compiled_blocks <= r->statistics().blocks); CHECK(r->statistics().compilation_attempts == r->statistics().compiled_blocks);
    auto s = runtime(switches,level); CHECK(call(*s,0) == 11); CHECK(call(*s,1) == 29); CHECK(call(*s,2) == 11);
    auto p = runtime(indirect,level); auto root = p->label_address(label(*p,"target")); REQUIRE(root);
    CHECK(p->statistics().compiled_blocks == 0);
    for (auto n : {0,1,42,-1}) { CHECK(call(*p,n) == n); CHECK(*p->label_address(label(*p,"target")) == *root); }
    auto id = p->entry_version().id(); auto foreign = r->address(id); REQUIRE_FALSE(foreign);
    CHECK_FALSE(r->compile(id)); CHECK_FALSE(r->artifact(id)); CHECK_FALSE(r->label_address(label(*p,"target")));
  }
}
TEST_CASE("owning BBV runtime specializes property snapshots only when their source blocks compile") {
  constexpr auto text = "m:module\nf:func i64,i64:x\nlocal i64:r\nbt yes,x\nprset x,7\njmp join\nyes:prset x,9\njoin:prbeq seven,x,7\nmov r,29\nret r\nseven:mov r,11\nret r\nendfunc\nendmodule\n";
  for (unsigned level = 0; level < 4; ++level) for (bool seven_first : {true,false}) {
    CAPTURE(level); CAPTURE(seven_first); auto r = runtime(text,level);
    auto initial = seven_first ? 0 : 1; auto expected = seven_first ? 11 : 29;
    CHECK(call(*r,initial) == expected);
    // Original policy retains the first join version even after another path
    // supplies different properties. Creating all versions eagerly breaks this.
    CHECK(call(*r,!initial) == expected); CHECK(call(*r,initial) == expected);
    CHECK(r->statistics().compiled_blocks < r->statistics().blocks);
    auto data_root = runtime(text,level); auto root = data_root->label_address(label(*data_root,"join")); REQUIRE(root);
    CHECK(call(*data_root,0) == 29); CHECK(call(*data_root,1) == 29);
    CHECK(*data_root->label_address(label(*data_root,"join")) == *root);
  }
}
TEST_CASE("owning BBV runtime retries observer and symbol errors without publishing or changing its stable gate") {
  bool fail = true; Runtime *active = nullptr; unsigned observations = 0;
  auto r = runtime("m:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n",0,{.observe=[&](auto &v,auto &) -> Result<void> {
    ++observations;
    auto nested = active->compile(v.id()); CHECK_FALSE(nested); CHECK(nested.error().message.find("recursive") != std::string::npos);
    if (fail) return Error{ErrorCode::InvalidArgument,"observer rejected block"}; return {};
  }}); active = r.get(); auto id = r->entry_version().id(); auto entry = r->entry_address(),remaining = r->remaining_code_bytes();
  auto first = r->compile(id); REQUIRE_FALSE(first); CHECK(first.error().message.find("/bbv-observe #") != std::string::npos);
  CHECK(first.error().message.find("observer rejected") != std::string::npos); CHECK(r->entry_address() == entry); CHECK(r->remaining_code_bytes() == remaining);
  CHECK_FALSE(r->artifact(id)); CHECK(r->statistics().compiled_blocks == 0);
  fail = false; REQUIRE(r->compile(id)); CHECK(observations == 2); CHECK(r->statistics().compilation_attempts == 2); CHECK(call(*r,42) == 42);
  // Windows can put the function entry and body in separate blocks.
  CHECK(r->statistics().compilation_attempts == r->statistics().compiled_blocks+1);
  CHECK(observations == r->statistics().compilation_attempts);
  auto attempts = r->statistics().compilation_attempts; CHECK(call(*r,42) == 42); CHECK(r->statistics().compilation_attempts == attempts);
  bool known = false; std::uint64_t value = 1234;
  auto symbols = runtime("m:module\nf:func i64,i64:x\nlocal i64:r\nmov r,data\nret r\nendfunc\ndata:bss 8\nendmodule\n",0,{.resolve=[&](auto &request) -> Result<std::uint64_t> {
    REQUIRE(request.symbol); CHECK(request.symbol->name == "data"); if (!known) return Error{ErrorCode::UnknownName,"unresolved test symbol"}; return value;
  }});
  auto symbol_block = symbols->entry_version().id(); auto failed = symbols->compile(symbol_block);
  while (failed) {
    auto artifact = symbols->artifact(symbol_block); REQUIRE(artifact); REQUIRE(artifact->successors().size() == 1);
    symbol_block = artifact->successors()[0].target; failed = symbols->compile(symbol_block);
  }
  CHECK(failed.error().code == ErrorCode::UnknownName); CHECK_FALSE(symbols->artifact(symbol_block));
  auto compiled = symbols->statistics().compiled_blocks;
  known = true; REQUIRE(symbols->compile(symbol_block)); CHECK(symbols->statistics().compiled_blocks == compiled+1);
  CHECK(call(*symbols,0) == 1234); CHECK(symbols->statistics().compilation_attempts == symbols->statistics().compiled_blocks+1);
}
TEST_CASE("owning BBV runtime retries platform errors at the same address without leaking arena pages") {
  for (unsigned kind = 0; kind < 3; ++kind) {
    auto provider = std::make_shared<Provider>(); auto r = runtime("m:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n",0,{.memory_provider=provider});
    auto before = r->remaining_code_bytes(),entry = r->entry_address();
    if (!kind) provider->fail_write = provider->writes+1; else if (kind == 1) provider->fail_seal = provider->seals+1; else provider->fail_flush = provider->flushes+1;
    auto first = r->compile(r->entry_version().id()); REQUIRE_FALSE(first); CHECK(first.error().message.find("/bbv-publish #") != std::string::npos);
    auto after = r->remaining_code_bytes(); CHECK(before-after == provider->page_size()); CHECK_FALSE(r->artifact(r->entry_version().id()));
    REQUIRE(r->compile(r->entry_version().id())); CHECK(r->remaining_code_bytes() == after); CHECK(call(*r,42) == 42);
    CHECK(provider->allocations == 1); CHECK(provider->written[1].data == provider->written[2].data); CHECK(r->entry_address() == entry);
    CHECK(r->statistics().compilation_attempts == r->statistics().compiled_blocks+1);
  }
}
TEST_CASE("owning BBV runtime reports exhaustion and invalid recovery before executing any partial block") {
  auto provider = std::make_shared<Provider>(); Options options{.code_capacity=provider->page_size(),.failure_continuation=reinterpret_cast<std::uintptr_t>(&unexpected_failure),.memory_provider=provider};
  auto r = Runtime::create(prepare("m:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n"),options); REQUIRE(r);
  auto failed = (*r)->compile((*r)->entry_version().id()); REQUIRE_FALSE(failed); CHECK(failed.error().message.find("capacity exhausted") != std::string::npos);
  CHECK((*r)->statistics().compiled_blocks == 0); CHECK((*r)->statistics().code_batches == 1); CHECK((*r)->remaining_code_bytes() == 0);
  failed = (*r)->compile((*r)->entry_version().id()); REQUIRE_FALSE(failed); CHECK(provider->allocations == 1);
  options.failure_continuation = 0; CHECK_FALSE(Runtime::create(prepare(loop),options)); CHECK(provider->allocations == 1);
  options.failure_continuation = reinterpret_cast<std::uintptr_t>(&unexpected_failure); options.code_capacity = 0;
  CHECK_FALSE(Runtime::create(prepare(loop),options)); CHECK(provider->allocations == 1);
}
TEST_CASE("owning BBV runtime accepts native block return-block variadic and multiple-result call adapters") {
  for (unsigned level = 0; level < 4; ++level) for (unsigned n = 0; n <= unsigned(test::CallExample::Inline); ++n) {
    CAPTURE(level); CAPTURE(n); auto kind = static_cast<test::CallExample>(n); auto source = test::call_example(kind);
    auto optimized = transforms::optimize(*source,{.level=static_cast<transforms::OptimizationLevel>(level)}); REQUIRE(optimized);
    auto p = BBVPreparation::prepare(*optimized->find_function("calls","sum"),host(),{.optimization_level=level});
    if (host().abi() == target::ABI::X86Windows && kind == test::CallExample::MultipleResults) {
      REQUIRE_FALSE(p); CHECK(p.error().message.find("Windows does not support multiple return values") != std::string::npos); continue;
    }
    REQUIRE(p); auto program = BBVProgram::create(std::move(*p)); REQUIRE(program);
    source.reset(); optimized = Error{ErrorCode::InvalidArgument,"source owner deliberately destroyed"};
    auto r = Runtime::create(std::move(*program),{.code_capacity=1024*1024,
      .failure_continuation=reinterpret_cast<std::uintptr_t>(&unexpected_failure),.resolve=[](auto &request) -> Result<std::uint64_t> {
        if (!request.builtin) return Error{ErrorCode::UnknownName,"unexpected symbol in BBV ABI fixture"};
        auto address = builtin_address(*request.builtin); if (!address) return address.error(); return *address;
      }}); REQUIRE(r);
    execution::detail::NativeCalls adapter({}); auto &signature = (*r)->program().preparation().function().signature();
    std::array<std::int64_t,2> bytes{20,22}; std::vector<ExecutionArgument> args;
    if (kind == test::CallExample::Block) args.push_back(ExecutionArgument::block(std::as_bytes(std::span(bytes))));
    else if (kind == test::CallExample::ReturnBlock) args.push_back(ExecutionArgument::return_buffer(std::as_writable_bytes(std::span(bytes))));
    else if (kind == test::CallExample::VariadicBlock) { args.emplace_back(0); args.push_back(ExecutionArgument::block(std::as_bytes(std::span(bytes)))); }
    else { if (kind == test::CallExample::Variadic) args.emplace_back(0); args.emplace_back(20); args.emplace_back(22); }
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
      auto values = adapter.call((*r)->entry_address(),signature,args); INFO((values ? "" : values.error().message)); REQUIRE(values);
      if (kind == test::CallExample::ReturnBlock) { CHECK(values->empty()); CHECK(bytes[0] == 42); }
      else if (kind == test::CallExample::MultipleResults) { REQUIRE(values->size() == 2); CHECK(*(*values)[0].as_i64() == 20); CHECK(*(*values)[1].as_i64() == 22); }
      else { REQUIRE(values->size() == 1); CHECK(*(*values)[0].as_i64() == 42); }
    }
    CHECK((*r)->statistics().compiled_blocks == (*r)->statistics().compilation_attempts); CHECK(adapter.size() == 1);
  }
}
