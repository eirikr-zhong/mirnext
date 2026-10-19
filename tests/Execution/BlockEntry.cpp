/* Licensed under the MIT License. Live block state across native cold entries. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/BlockEntry.hpp"
#include "mirnext/CodeGen/BlockVersioning.hpp"
#include "mirnext/CodeGen/CompiledBlock.hpp"
#include "mirnext/CodeGen/Detail/X86RuntimeEntry.hpp"
#include "mirnext/Execution/CodeMemory.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <array>
#include <cfenv>
#include <set>
using namespace mirnext; using namespace mirnext::codegen; using namespace mirnext::execution;
TEST_CASE("x86 block resolver frames protect red zones and recover arbitrary incoming stack alignment") {
  for (auto abi : {target::ABI::X86SysV,target::ABI::X86Windows}) {
    auto layout = codegen::detail::x86_block_resolver_frame(abi); REQUIRE(layout); auto &f = layout->saved;
    CHECK(layout->red_zone_bytes == (abi == target::ABI::X86SysV ? 128 : 0));
    CHECK(f.frame_bytes%16 == 0); CHECK(f.fx_offset%16 == 0);
    CHECK(f.metadata_offset+8 <= f.frame_bytes);
    std::set<std::size_t> occupied;
    for (std::size_t n = 0; n < f.shadow_bytes; ++n) CHECK(occupied.insert(n).second);
    for (std::size_t n = 0; n < 512; ++n) CHECK(occupied.insert(f.fx_offset+n).second);
    for (auto &save : f.saves) for (unsigned n = 0; n < 8; ++n) CHECK(occupied.insert(save.offset+n).second);
    for (unsigned n = 0; n < 8; ++n) CHECK(occupied.insert(f.metadata_offset+n).second);
    for (auto original : {0x100000000ull,0x100000008ull}) {
      auto flags_sp = original-layout->red_zone_bytes-8;
      auto callback_sp = (flags_sp&~15ull)-f.frame_bytes;
      CHECK(callback_sp%16 == 0); CHECK(callback_sp+f.frame_bytes <= flags_sp);
      CHECK(flags_sp+8+layout->red_zone_bytes == original);
      CHECK(flags_sp+8 <= original-layout->red_zone_bytes);
    }
  }
  CHECK_FALSE(codegen::detail::x86_block_resolver_frame(target::ABI::AArch64Linux));
}
namespace {
struct alignas(16) State {
  std::array<std::uint64_t,32> gp{};
  std::array<std::array<std::uint64_t,2>,32> vectors{};
  std::uint64_t sp = 0,flags = 0,fpcr = 0,fpsr = 0;
  alignas(16) std::array<std::uint8_t,512> floating{};
};
struct Probe {
  State before,after;
  std::array<std::uint64_t,16> red_zone{};
  std::uint64_t path = 0;
};
struct ProbeCode { std::vector<std::uint8_t> bytes; std::size_t success,failure; };
struct Descriptor {
  std::uintptr_t destination = 0,target = 0;
  unsigned callbacks = 0;
  bool fail = false;
  std::function<Result<std::uintptr_t>()> compile;
  std::optional<Error> error;
};
std::uintptr_t resolve_block(Descriptor *d) {
  ++d->callbacks;
  // Deliberately change FP state and caller-clobbered vectors. Plain integer
  // return values alone cannot establish preservation at an internal boundary.
  std::fesetround(FE_DOWNWARD); std::feraiseexcept(FE_INEXACT);
#if defined(__aarch64__)
  asm volatile("movi v0.16b,#0\nmovi v1.16b,#0\nmovi v7.16b,#0\nmovi v16.16b,#0\nmovi v31.16b,#0\n"
               "cmp xzr,xzr" : : : "v0","v1","v7","v16","v31","cc");
#elif defined(__x86_64__) && !defined(_MSC_VER)
  asm volatile("pxor %%xmm0,%%xmm0\npxor %%xmm1,%%xmm1\npxor %%xmm7,%%xmm7\npxor %%xmm15,%%xmm15\n"
               "test %%eax,%%eax" : : : "xmm0","xmm1","xmm7","xmm15","cc");
#endif
  if (d->fail) return 0;
  if (d->compile) {
    auto compiled = d->compile(); if (!compiled) { d->error = compiled.error(); return 0; }
    return d->destination = *compiled;
  }
  return d->destination = d->target;
}
// The probe owns and unwinds a real frame, seeds usable registers, snapshots
// their complete bit patterns, and jumps into a gate from inside that frame.
// Its failure continuation is in that same frame, rather than a false RET.
ProbeCode aarch64_probe(const target::TargetMachine &target,Probe &probe,std::uintptr_t gate,unsigned extra) {
  struct Code {
    std::vector<std::uint32_t> words;
    void word(std::uint32_t value) { words.push_back(value); }
    void address(unsigned r,std::uint64_t value) {
      constexpr std::uint32_t moves[]{0xd2800000,0xf2a00000,0xf2c00000,0xf2e00000};
      for (unsigned n = 0; n < 4; ++n) word(moves[n] | std::uint32_t((value>>(16*n))&65535)<<5 | r);
    }
    void snapshot(State &s) {
      address(9,reinterpret_cast<std::uintptr_t>(&s));
      for (unsigned n = 0; n < 31; ++n) word(0xf9000120 | n<<10 | n); // str xn,[x9,#8*n]
      for (unsigned n = 0; n < 32; ++n) word(0x3d800120 | (16+n)<<10 | n);
      word(0x910003ea); word(0xf900012a | unsigned(offsetof(State,sp)/8)<<10);
      word(0xd53b420a); word(0xf900012a | unsigned(offsetof(State,flags)/8)<<10);
      word(0xd53b440a); word(0xf900012a | unsigned(offsetof(State,fpcr)/8)<<10);
      word(0xd53b442a); word(0xf900012a | unsigned(offsetof(State,fpsr)/8)<<10);
    }
  } c;
  c.word(0xd10003ff | 784u<<10);
  for (unsigned n = 0; n < 31; ++n) c.word(0xf90003e0 | n<<10 | n);
  for (unsigned n = 0; n < 32; ++n) c.word(0x3d8003e0 | (17+n)<<10 | n);
  if (extra) c.word(0xd10003ff | extra<<10);
  for (auto &info : target.registers()) if (info.bank == target::RegisterBank::Integer && !info.reserved)
    c.address(info.reg.encoding(),0x123456789abc0000ull+info.reg.encoding());
  for (unsigned n = 0; n < 32; ++n) c.word(0x4f00e400 | (n+1)%32<<5 | n);
  c.address(9,0xb0000000); c.word(0xd51b4209); c.snapshot(probe.before);
  c.address(9,gate); c.word(0xd61f0120);
  auto continuation = [&](unsigned path) {
    auto offset = c.words.size()*4; c.snapshot(probe.after);
    c.address(9,reinterpret_cast<std::uintptr_t>(&probe.path)); c.address(10,path); c.word(0xf900012a);
    if (extra) c.word(0x910003ff | extra<<10);
    for (unsigned n = 0; n < 32; ++n) c.word(0x3dc003e0 | (17+n)<<10 | n);
    for (unsigned n = 0; n < 31; ++n) c.word(0xf94003e0 | n<<10 | n);
    c.word(0x910003ff | 784u<<10); c.word(0xd65f03c0); return offset;
  };
  auto success = continuation(1),failure = continuation(2);
  ProbeCode out{{},success,failure}; for (auto w : c.words) for (unsigned n = 0; n < 4; ++n) out.bytes.push_back(std::uint8_t(w>>(8*n)));
  return out;
}
ProbeCode x86_probe(const target::TargetMachine &target,Probe &probe,std::uintptr_t gate,unsigned extra) {
  struct Code {
    std::vector<std::uint8_t> bytes;
    void emit(std::initializer_list<std::uint8_t> v) { bytes.insert(bytes.end(),v); }
    void integer(std::uint64_t v,unsigned size) { for (unsigned n = 0; n < size; ++n) bytes.push_back(std::uint8_t(v>>(8*n))); }
    void address(unsigned r,std::uint64_t v) { emit({std::uint8_t(0x48|(r>>3)),std::uint8_t(0xb8|(r&7))}); integer(v,8); }
    void memory(std::uint8_t op,unsigned r,unsigned base,std::int32_t offset,bool vector = false) {
      if (vector) emit({0xf3});
      emit({std::uint8_t((vector ? 0x40 : 0x48)|((r>>3)<<2)|(base>>3))});
      if (vector) emit({0x0f}); emit({op,std::uint8_t(0x80|((r&7)<<3)|(base&7))});
      if ((base&7) == 4) emit({0x24}); integer(std::uint32_t(offset),4);
    }
    void stack(std::int32_t delta) { memory(0x8d,4,4,delta); }
    void fx(bool restore) { emit({0x48,0x0f,0xae,std::uint8_t(restore ? 0x0c : 0x04),0x24}); }
    void snapshot(State &s) {
      address(11,reinterpret_cast<std::uintptr_t>(&s));
      for (unsigned n = 0; n < 16; ++n) if (n != 4 && n != 10 && n != 11) memory(0x89,n,11,n*8);
      for (unsigned n = 0; n < 16; ++n) memory(0x7f,n,11,offsetof(State,vectors)+n*16,true);
      emit({0x49,0x89,0xe2}); memory(0x89,10,11,offsetof(State,sp)); // mov r10,rsp
      stack(-128); emit({0x9c,0x41,0x5a}); stack(128); // protect the red zone even while inspecting flags
      memory(0x89,10,11,offsetof(State,flags));
      emit({0x49,0x0f,0xae,0x83}); integer(offsetof(State,floating),4); // fxsave64 [r11+offset]
    }
  } c;
  c.stack(-632); // incoming SP=8 mod16; aligned FXSAVE64 and 13 GP saves
  c.fx(false);
  unsigned saved = 0;
  for (unsigned n = 0; n < 16; ++n) if (n != 4 && n != 10 && n != 11) c.memory(0x89,n,4,512+8*saved++);
  if (extra) c.stack(-std::int32_t(extra));
  for (unsigned n = 0; n < 16; ++n) if (n != 4 && n != 10 && n != 11) c.address(n,0x123456789abc0000ull+n);
  c.address(11,reinterpret_cast<std::uintptr_t>(&probe.before.vectors));
  for (unsigned n = 0; n < 16; ++n) c.memory(0x6f,n,11,n*16,true);
  if (target.abi() == target::ABI::X86SysV) c.emit({0xd9,0xe8,0xd9,0xeb}); // live x87 1 and pi
  c.address(11,0xad7); c.emit({0x41,0x53,0x9d}); c.snapshot(probe.before);
  if (target.abi() == target::ABI::X86SysV) for (unsigned n = 0; n < 16; ++n) {
    c.address(10,0x7654321000000000ull+n); c.memory(0x89,10,4,-std::int32_t(8*(n+1)));
  }
  c.address(11,gate); c.emit({0x41,0xff,0xe3});
  auto continuation = [&](unsigned path) {
    auto offset = c.bytes.size(); c.snapshot(probe.after);
    c.address(11,reinterpret_cast<std::uintptr_t>(&probe.red_zone));
    if (target.abi() == target::ABI::X86SysV) for (unsigned n = 0; n < 16; ++n) {
      c.memory(0x8b,10,4,-std::int32_t(8*(n+1))); c.memory(0x89,10,11,n*8);
    }
    c.address(11,reinterpret_cast<std::uintptr_t>(&probe.path)); c.address(10,path); c.memory(0x89,10,11,0);
    if (extra) c.stack(extra); c.fx(true); saved = 0;
    for (unsigned n = 0; n < 16; ++n) if (n != 4 && n != 10 && n != 11) c.memory(0x8b,n,4,512+8*saved++);
    c.stack(632); c.emit({0xc3}); return offset;
  };
  auto success = continuation(1),failure = continuation(2); return {std::move(c.bytes),success,failure};
}
}
TEST_CASE("native block entries preserve full live state dynamic stacks flags and red zones on cold warm failure and retry") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  struct Restore { std::fenv_t env; Restore() { std::fegetenv(&env); } ~Restore() { std::fesetenv(&env); } } restore;
  const bool arm = target->architecture() == target::Architecture::AArch64;
  for (unsigned extra : (arm ? std::array{0u,16u,96u,112u} : std::array{0u,8u,96u,104u})) {
    CAPTURE(extra); Probe probe; Descriptor descriptor;
    for (unsigned n = 0; n < 32; ++n) probe.before.vectors[n] = {0x3ff0000000000000ull+n,0x1234567800000000ull+n};
    auto make_probe = [&](std::uintptr_t gate) { return arm ? aarch64_probe(*target,probe,gate,extra) : x86_probe(*target,probe,gate,extra); };
    auto p = make_probe(4); auto r = make_block_resolver(*target,reinterpret_cast<std::uintptr_t>(&resolve_block),4); REQUIRE(r);
    const std::array sizes{p.bytes.size(),r->size(),block_entry_bytes}; auto reserved = CodeMemory().reserve(sizes); REQUIRE(reserved);
    auto probe_address = reserved->address(0),resolver_address = reserved->address(1),gate_address = reserved->address(2);
    REQUIRE(probe_address); REQUIRE(resolver_address); REQUIRE(gate_address);
    descriptor.target = *probe_address+p.success; descriptor.destination = *gate_address+block_entry_cold_offset;
    p = make_probe(*gate_address);
    r = make_block_resolver(*target,reinterpret_cast<std::uintptr_t>(&resolve_block),*probe_address+p.failure); REQUIRE(r);
    auto gate = make_block_entry(*target,{reinterpret_cast<std::uintptr_t>(&descriptor.destination),reinterpret_cast<std::uintptr_t>(&descriptor),*resolver_address}); REQUIRE(gate);
    CHECK(gate->size() == block_entry_bytes);
    const std::array<std::span<const std::uint8_t>,3> bytes{p.bytes,*r,*gate}; auto code = reserved->publish(bytes); REQUIRE(code);
    auto call = reinterpret_cast<void (*)()>((*code)[0].address());
    // Cold, warm, failure (still cold), successful retry, then warm again.
    for (unsigned iteration = 0; iteration < 5; ++iteration) {
      CAPTURE(iteration);
      if (iteration == 2) { descriptor.destination = *gate_address+block_entry_cold_offset; descriptor.fail = true; }
      if (iteration == 3) descriptor.fail = false;
      REQUIRE(std::fesetround(FE_TONEAREST) == 0); REQUIRE(std::feclearexcept(FE_ALL_EXCEPT) == 0);
      call(); CHECK(probe.path == (iteration == 2 ? 2 : 1));
      CHECK(descriptor.callbacks == (iteration < 2 ? 1 : iteration == 2 ? 2 : 3));
      CHECK(probe.before.sp == probe.after.sp); CHECK(probe.before.sp != 0);
      auto flags_mask = arm ? 0xf0000000ull : 0xcd5ull;
      CHECK((probe.before.flags&flags_mask) == (probe.after.flags&flags_mask));
      CHECK((probe.before.flags&flags_mask) != 0);
      CHECK(probe.before.fpcr == probe.after.fpcr); CHECK(probe.before.fpsr == probe.after.fpsr);
      if (!arm) {
        // Complete x87 stack, control/status/tag words and MXCSR, including
        // values that are live across the callback rather than ABI results.
        CHECK(std::equal(probe.before.floating.begin(),probe.before.floating.begin()+160,probe.after.floating.begin()));
        if (target->abi() == target::ABI::X86SysV) CHECK(probe.before.floating[4] != 0);
      }
      for (auto &info : target->registers()) if (info.bank == target::RegisterBank::Integer && !info.reserved) {
        auto n = info.reg.encoding(); CAPTURE(n); CHECK(probe.before.gp[n] == probe.after.gp[n]);
      }
      for (unsigned n = 0; n < (arm ? 32u : 16u); ++n) { CAPTURE(n); CHECK(probe.before.vectors[n] == probe.after.vectors[n]); }
      if (target->abi() == target::ABI::X86SysV) for (unsigned n = 0; n < 16; ++n) CHECK(probe.red_zone[n] == 0x7654321000000000ull+n);
      CHECK(std::fegetround() == FE_TONEAREST); CHECK(std::fetestexcept(FE_INEXACT) == 0);
      CHECK(descriptor.destination == (iteration == 2 ? *gate_address+block_entry_cold_offset : descriptor.target));
    }
  }
}
TEST_CASE("block entry validation rejects missing and misaligned recovery and dispatch addresses") {
  auto t = target::TargetMachine::native(); REQUIRE(t);
  CHECK_FALSE(make_block_resolver(*t,0,4)); CHECK_FALSE(make_block_resolver(*t,4,0));
  if (t->architecture() == target::Architecture::AArch64) {
    CHECK_FALSE(make_block_resolver(*t,4,3)); CHECK_FALSE(make_block_resolver(*t,3,4));
  }
  for (auto a : {BlockEntryAddresses{0,8,4},BlockEntryAddresses{1,8,4},BlockEntryAddresses{8,0,4},BlockEntryAddresses{8,8,0}}) CHECK_FALSE(make_block_entry(*t,a));
  if (t->architecture() == target::Architecture::AArch64) CHECK_FALSE(make_block_entry(*t,{8,8,3}));
}
TEST_CASE("native BBV cold callbacks encode publish and redirect independently selected stack-using fragments once") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  struct Restore { std::fenv_t env; Restore() { std::fegetenv(&env); } ~Restore() { std::fesetenv(&env); } } restore;
  // A focused real BBV boundary: entry fragments allocate the stack and
  // stores an argument, then dispatches through an immutable cold gate. Each
  // selected continuation is encoded and published only inside its callback.
  // This does not implement general session publication or error unwinding.
  for (unsigned level = 0; level < 4; ++level) {
    CAPTURE(level);
    auto prep = [&] {
      auto c = serialization::parse_text("m:module\nf:func i64,i64:x\nlocal p:stack,i64:r\nalloca stack,64\nmov i64:(stack),x\nbt yes,x\nmov r,i64:(stack)\nadd r,r,17\njmp done\nyes:mov r,i64:(stack)\nadd r,r,41\ndone:ret r\nendfunc\nendmodule\n"); REQUIRE(c);
      auto p = BBVPreparation::prepare(*(*c)->modules().begin()->functions().begin(),*target,{.optimization_level=level});
      INFO((p ? "" : p.error().message)); REQUIRE(p); return std::move(*p);
    }();
    auto program = BBVProgram::create(std::move(prep)); REQUIRE(program);
    auto props = BBVProperties::create(program->preparation().function()); REQUIRE(props);
    auto version = program->request_version(*program->preparation().blocks().begin(),*props,true); REQUIRE(version);
    auto first = encode_bbv(*program,*version->version); REQUIRE(first);
    std::size_t count = 0; const BBVBlock *terminal = nullptr;
    for (auto &b : program->preparation().blocks()) { ++count; terminal = &b; }
    REQUIRE(terminal);
    // The explicit common epilogue is a safe test-only recovery continuation.
    // This fixture has no properties, so reserving its version early has no
    // effect on specialization. General session recovery is separate work.
    auto recovery_version = program->request_version(*terminal,*props); REQUIRE(recovery_version);
    auto recovery = encode_bbv(*program,*recovery_version->version); REQUIRE(recovery); REQUIRE(recovery->successors().empty());
    auto resolver = make_block_resolver(*target,reinterpret_cast<std::uintptr_t>(&resolve_block),4); REQUIRE(resolver);
    std::vector<std::size_t> sizes{first->bytes().size(),resolver->size(),recovery->bytes().size()}; sizes.resize(3+count,block_entry_bytes);
    auto reserve = CodeMemory().reserve(sizes); REQUIRE(reserve);
    std::vector<std::uintptr_t> addresses(sizes.size());
    for (unsigned n = 0; n < addresses.size(); ++n) { auto a = reserve->address(n); REQUIRE(a); addresses[n] = *a; }
    std::vector<Descriptor> descriptors(count); std::vector<ExecutableCode> generated;
    std::vector<const BBVVersion *> versions(count);
    auto remember = [&](const BBVVersion &v) -> Result<void> {
      if (!v.id().same_program(first->version()) || v.id().index() >= count)
        return Error{ErrorCode::InvalidArgument,"unexpected test BBV version"};
      versions[v.id().index()] = &v; return {};
    };
    REQUIRE(remember(*version->version)); REQUIRE(remember(*recovery_version->version));
    for (auto &edge : version->version->specialization()->successors()) REQUIRE(remember(*edge.version));
    BlockAddressResolver resolve = [&](const BlockVersionId &id) -> Result<std::uint64_t> {
      if (!id.same_program(first->version()) || id.index() >= count) return Error{ErrorCode::UnknownName,"unexpected test BBV successor"};
      return addresses[3+id.index()];
    };
    std::vector<std::vector<std::uint8_t>> storage(sizes.size());
    for (unsigned n = 0; n < descriptors.size(); ++n) {
      auto &d = descriptors[n]; d.destination = addresses[3+n]+block_entry_cold_offset;
      d.compile = [&,n]() -> Result<std::uintptr_t> {
        if (!versions[n]) return Error{ErrorCode::UnknownName,"test block has not been selected"};
        auto block = encode_bbv(*program,*versions[n]); if (!block) return block.error();
        for (auto &edge : versions[n]->specialization()->successors()) { auto saved = remember(*edge.version); if (!saved) return saved.error(); }
        const std::array sizes{block->bytes().size()}; auto slot = CodeMemory().reserve(sizes); if (!slot) return slot.error();
        auto a = slot->address(0); if (!a) return a.error();
        auto bytes = block->relocate(*a,resolve); if (!bytes) return bytes.error();
        const std::array<std::span<const std::uint8_t>,1> views{*bytes}; auto code = slot->publish(views); if (!code) return code.error();
        generated.push_back(std::move(code->front())); return generated.back().address();
      };
      auto gate = make_block_entry(*target,{reinterpret_cast<std::uintptr_t>(&d.destination),reinterpret_cast<std::uintptr_t>(&d),addresses[1]});
      REQUIRE(gate); storage[3+n] = std::move(*gate);
    }
    descriptors[first->version().index()].destination = addresses[0];
    auto entry = first->relocate(addresses[0],resolve); REQUIRE(entry); storage[0] = std::move(*entry);
    resolver = make_block_resolver(*target,reinterpret_cast<std::uintptr_t>(&resolve_block),addresses[2]); REQUIRE(resolver); storage[1] = std::move(*resolver);
    auto fallback = recovery->relocate(addresses[2],{}); REQUIRE(fallback); storage[2] = std::move(*fallback);
    std::vector<std::span<const std::uint8_t>> views; for (auto &bytes : storage) views.push_back(bytes);
    auto published = reserve->publish(views); REQUIRE(published);
    auto call = reinterpret_cast<std::int64_t (*)(std::int64_t)>(published->front().address());
    unsigned iteration = 0; std::size_t after_first = 0,after_second = 0;
    for (auto input : {0,0,1,1,2,0}) {
      CAPTURE(input); CHECK(call(input) == (input == 0 ? 17 : input+41));
      unsigned callbacks = 0;
      for (auto &d : descriptors) { INFO((d.error ? d.error->message : "")); CHECK_FALSE(d.error); CHECK(d.callbacks <= 1); callbacks += d.callbacks; }
      CHECK(callbacks == generated.size());
      if (iteration == 0) { after_first = generated.size(); CHECK(after_first > 0); }
      else if (iteration == 1) CHECK(generated.size() == after_first);
      else if (iteration == 2) { after_second = generated.size(); CHECK(after_second == after_first+1); }
      else CHECK(generated.size() == after_second);
      ++iteration;
    }
    CHECK(generated.size() == count-1);
    for (unsigned n = 0; n < storage.size(); ++n)
      CHECK(std::equal(storage[n].begin(),storage[n].end(),(*published)[n].bytes().begin(),(*published)[n].bytes().end()));
  }
}
