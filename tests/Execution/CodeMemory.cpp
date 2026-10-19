/* Licensed under the MIT License. Native executable ownership and failure paths. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Execution/CodeMemory.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <new>
#include <thread>
using namespace mirnext;
using namespace mirnext::execution;
namespace {
struct Provider final : CodeMemoryProvider {
  std::size_t page = 4096;
  unsigned allocations = 0, releases = 0, writes = 0, seals = 0, flushes = 0;
  unsigned fail_allocate = 0, fail_write = 0, fail_seal = 0, fail_flush = 0;
  bool bad_size = false, bad_alignment = false;
  bool writable = false;
  std::vector<std::string> events;
  std::vector<MappedCodeMemory> written,sealed,flushed;
  std::size_t last_size = 0;
  std::size_t page_size() const override { return page; }
  Result<MappedCodeMemory> allocate(std::size_t n) override {
    events.push_back("allocate"); last_size = n;
    if (++allocations == fail_allocate) return Error{ErrorCode::InvalidArgument,"injected allocation failure"};
    auto data = static_cast<std::uint8_t *>(::operator new(n,std::align_val_t(4096),std::nothrow));
    if (!data) return Error{ErrorCode::InvalidArgument,"test allocation failure"};
    std::memset(data,0,n);
    return MappedCodeMemory{data + (bad_alignment ? 1 : 0),n - (bad_size ? 1 : 0)};
  }
  Result<void> release(MappedCodeMemory m) override {
    ++releases; events.push_back("release");
    ::operator delete(m.data - (bad_alignment ? 1 : 0),std::align_val_t(4096)); return {};
  }
  Result<void> make_writable(MappedCodeMemory range) override {
    written.push_back(range);
    events.push_back("write"); if (++writes == fail_write) return Error{ErrorCode::InvalidArgument,"injected write failure"}; writable = true; return {};
  }
  Result<void> make_executable(MappedCodeMemory range) override {
    sealed.push_back(range);
    events.push_back("seal"); if (++seals == fail_seal) return Error{ErrorCode::InvalidArgument,"injected seal failure"}; writable = false; return {};
  }
  Result<void> flush(MappedCodeMemory range) override {
    flushed.push_back(range);
    events.push_back("flush"); CHECK_FALSE(writable);
    if (++flushes == fail_flush) return Error{ErrorCode::InvalidArgument,"injected flush failure"}; return {};
  }
};
std::vector<std::uint8_t> scalar(unsigned value) {
#if defined(__aarch64__) || defined(_M_ARM64)
  std::array<std::uint32_t,2> words{0xd2800000u | (value << 5),0xd65f03c0u};
  std::vector<std::uint8_t> bytes(sizeof words); std::memcpy(bytes.data(),words.data(),bytes.size()); return bytes;
#else
  return {0xb8,static_cast<std::uint8_t>(value),0,0,0,0xc3};
#endif
}
}
TEST_CASE("owning code memory batches aligned slots and rounds pages without unused extra pages") {
  auto p = std::make_shared<Provider>(); CodeMemory memory(p);
  std::array<std::size_t,3> sizes{1,17,16}; auto r = memory.reserve(sizes); REQUIRE(r); CHECK(r->size() == 3);
  auto a = r->address(0), b = r->address(1), c = r->address(2); REQUIRE(a); REQUIRE(b); REQUIRE(c);
  CHECK(*a % 4096 == 0); CHECK(*b == *a + 16); CHECK(*c == *a + 48); CHECK(p->last_size == 4096);
  CHECK_FALSE(r->address(3)); CHECK(p->writes == 0); CHECK(p->seals == 0);
  std::array<std::vector<std::uint8_t>,3> bytes{std::vector<std::uint8_t>(1,0xa1),std::vector<std::uint8_t>(17,0xb2),std::vector<std::uint8_t>(16,0xc3)};
  std::array<std::span<const std::uint8_t>,3> buffers{bytes[0],bytes[1],bytes[2]}; auto published = r->publish(buffers); REQUIRE(published);
  CHECK(r->size() == 0); CHECK_FALSE(r->address(0)); CHECK_FALSE(r->publish(buffers));
  CHECK(p->events == std::vector<std::string>{"allocate","write","seal","flush"});
  for (std::size_t n = 0; n < sizes.size(); ++n) {
    CHECK((*published)[n].bytes().size() == sizes[n]); CHECK((*published)[n].address() % 16 == 0);
    CHECK(std::equal(bytes[n].begin(),bytes[n].end(),(*published)[n].bytes().begin()));
  }
  std::size_t exact = 4096; auto page = memory.reserve({&exact,1}); REQUIRE(page); CHECK(p->last_size == 4096);
}
TEST_CASE("owning code handles retain their mapping and provider beyond all producer lifetimes") {
  auto provider = std::make_shared<Provider>(); std::weak_ptr<Provider> weak = provider;
  std::optional<ExecutableCode> code;
  {
    CodeMemory memory(provider); std::array<std::size_t,2> sizes{4,4}; auto r = memory.reserve(sizes); REQUIRE(r);
    auto moved = std::move(*r); CHECK(r->size() == 0); CHECK_FALSE(r->address(0));
    std::array<std::uint8_t,4> data{1,2,3,4}; std::array<std::span<const std::uint8_t>,2> buffers{data,data};
    auto batch = moved.publish(buffers); REQUIRE(batch); code = (*batch)[1];
  }
  CHECK(provider->releases == 0); provider.reset(); CHECK_FALSE(weak.expired());
  auto copy = code; code.reset(); REQUIRE(copy); CHECK(copy->bytes()[3] == 4); CHECK_FALSE(weak.expired()); copy.reset(); CHECK(weak.expired());
}
TEST_CASE("owning code reservations reject malformed sizes and mapping results before writing") {
  auto p = std::make_shared<Provider>(); CodeMemory memory(p);
  CHECK_FALSE(memory.reserve({})); CHECK_FALSE(CodeMemory(nullptr).reserve({}));
  for (auto size : {std::size_t{0},SIZE_MAX,SIZE_MAX-14}) { CHECK_FALSE(memory.reserve({&size,1})); CHECK(p->allocations == 0); }
  std::array<std::size_t,2> overflow{SIZE_MAX - 4095,4096}; CHECK_FALSE(memory.reserve(overflow)); CHECK(p->allocations == 0);
  for (std::size_t page : {0u,31u}) { p->page = page; std::size_t size = 1; CHECK_FALSE(memory.reserve({&size,1})); CHECK(p->allocations == 0); }
  p->page = 4096;
  for (unsigned kind = 0; kind < 3; ++kind) {
    p->fail_allocate = !kind ? p->allocations + 1 : 0; p->bad_size = kind == 1; p->bad_alignment = kind == 2;
    std::size_t size = 16; CHECK_FALSE(memory.reserve({&size,1})); CHECK(p->writes == 0);
  }
  CHECK(p->releases == 2);
}
TEST_CASE("owning publication validates the entire batch before touching code") {
  auto p = std::make_shared<Provider>(); CodeMemory memory(p); std::array<std::size_t,2> sizes{4,4};
  auto r = memory.reserve(sizes); REQUIRE(r); auto before = r->address(0); REQUIRE(before);
  std::array<std::uint8_t,4> data{1,2,3,4}; std::array<std::span<const std::uint8_t>,2> buffers{data,std::span(data).first(3)};
  CHECK_FALSE(r->publish(buffers)); CHECK_FALSE(r->publish(std::span(buffers).first(1))); CHECK(p->writes == 0);
  auto bytes = reinterpret_cast<const std::uint8_t *>(*before); CHECK(std::all_of(bytes,bytes + 20,[](auto v) { return v == 0; }));
  buffers[1] = data; auto code = r->publish(buffers); REQUIRE(code); CHECK(p->writes == 1);
}
TEST_CASE("owning publication returns no executable handles on platform failure and supports retry") {
  for (unsigned kind = 0; kind < 3; ++kind) {
    auto p = std::make_shared<Provider>(); CodeMemory memory(p); std::size_t size = 4;
    auto r = memory.reserve({&size,1}); REQUIRE(r); auto address = r->address(0); REQUIRE(address);
    if (!kind) p->fail_write = 1; else if (kind == 1) p->fail_seal = 1; else p->fail_flush = 1;
    std::array<std::uint8_t,4> data{1,2,3,4}; std::span<const std::uint8_t> buffer = data;
    auto failed = r->publish({&buffer,1}); REQUIRE_FALSE(failed); CHECK(failed.error().message.find("injected") != std::string::npos);
    CHECK_FALSE(p->writable); CHECK(r->size() == 1); CHECK(*r->address(0) == *address);
    auto code = r->publish({&buffer,1}); REQUIRE(code); CHECK((*code)[0].address() == *address); CHECK(p->allocations == 1);
  }
}
TEST_CASE("abandoned owning code reservations release unpublished pages exactly once") {
  auto p = std::make_shared<Provider>();
  { CodeMemory memory(p); std::size_t size = 16; auto r = memory.reserve({&size,1}); REQUIRE(r); auto moved = std::move(*r); CHECK(p->releases == 0); }
  CHECK(p->allocations == 1); CHECK(p->releases == 1); CHECK(p->writes == 0);
}
TEST_CASE("native immutable code batches execute after service destruction") {
  std::vector<ExecutableCode> functions;
  {
    CodeMemory memory; auto first = scalar(17), second = scalar(42);
    std::array<std::size_t,2> sizes{first.size(),second.size()}; auto r = memory.reserve(sizes); REQUIRE(r);
    std::array<std::span<const std::uint8_t>,2> buffers{first,second}; auto code = r->publish(buffers); REQUIRE(code); functions = std::move(*code);
  }
  CHECK(reinterpret_cast<int (*)()>(functions[0].address())() == 17);
  CHECK(reinterpret_cast<int (*)()>(functions[1].address())() == 42);
}
TEST_CASE("native independent code batches publish concurrently without reopening existing code") {
  CodeMemory memory; std::array<std::optional<ExecutableCode>,32> code; std::atomic<unsigned> failures{0};
  std::array<std::thread,4> workers;
  for (unsigned w = 0; w < workers.size(); ++w) workers[w] = std::thread([&,w] {
    for (unsigned n = w; n < code.size(); n += workers.size()) {
      auto bytes = scalar(n); std::size_t size = bytes.size(); auto r = memory.reserve({&size,1}); if (!r) { ++failures; continue; }
      std::span<const std::uint8_t> buffer = bytes; auto result = r->publish({&buffer,1}); if (!result) { ++failures; continue; } code[n] = std::move((*result)[0]);
    }
  });
  for (auto &w : workers) w.join(); CHECK(failures.load() == 0);
  for (unsigned n = 0; n < code.size(); ++n) { REQUIRE(code[n]); CHECK(reinterpret_cast<int (*)()>(code[n]->address())() == static_cast<int>(n)); }
}
TEST_CASE("owning code arenas reserve stable disjoint page batches and publish in any order") {
  auto p = std::make_shared<Provider>(); auto arena = CodeArena::create(3*4096,p); REQUIRE(arena);
  CHECK(p->allocations == 1); CHECK(arena->capacity() == 3*4096); CHECK(arena->remaining() == arena->capacity());
  std::array<std::size_t,2> sizes{4,17}; auto first = arena->reserve(sizes); REQUIRE(first);
  std::size_t size = 4; auto second = arena->reserve({&size,1}); REQUIRE(second);
  CHECK(*first->address(0) == arena->address()); CHECK(*first->address(1) == arena->address()+16);
  CHECK(*second->address(0) == arena->address()+4096); CHECK(arena->remaining() == 4096);
  CHECK(p->writes == 0); CHECK(p->allocations == 1);
  auto moved = std::move(*arena); CHECK(arena->capacity() == 0); CHECK(arena->remaining() == 0); CHECK(arena->address() == 0);
  CHECK_FALSE(arena->reserve({&size,1}));
  std::vector<std::uint8_t> a(4,0xa1),b(17,0xb2); std::span<const std::uint8_t> view = a;
  auto later = second->publish({&view,1}); REQUIRE(later); CHECK(p->written.back().data == reinterpret_cast<std::uint8_t *>(moved.address()+4096));
  const std::array<std::span<const std::uint8_t>,2> buffers{a,b}; auto earlier = first->publish(buffers); REQUIRE(earlier);
  CHECK(p->writes == 2); CHECK(p->seals == 2); CHECK(p->flushes == 2);
  for (unsigned n = 0; n < 2; ++n) {
    auto expected = reinterpret_cast<std::uint8_t *>(moved.address()+(n == 0 ? 4096 : 0));
    CHECK(p->written[n].data == expected); CHECK(p->sealed[n].data == expected); CHECK(p->flushed[n].data == expected);
    CHECK(p->written[n].size == 4096); CHECK(p->sealed[n].size == 4096); CHECK(p->flushed[n].size == 4096);
  }
  CHECK_FALSE(first->publish(buffers)); CHECK(p->writes == 2);
  auto third = moved.reserve({&size,1}); REQUIRE(third); CHECK(moved.remaining() == 0);
  CHECK_FALSE(moved.reserve({&size,1})); CHECK(p->allocations == 1);
  CHECK(std::equal(a.begin(),a.end(),later->front().bytes().begin()));
  CHECK(std::equal(b.begin(),b.end(),earlier->back().bytes().begin()));
}
TEST_CASE("owning code arena publication failures retain reservations and never touch other published pages") {
  for (unsigned kind = 0; kind < 3; ++kind) {
    auto p = std::make_shared<Provider>(); auto arena = CodeArena::create(8192,p); REQUIRE(arena);
    std::size_t size = 4; auto first = arena->reserve({&size,1}); REQUIRE(first);
    std::array<std::uint8_t,4> bytes{1,2,3,4}; std::span<const std::uint8_t> view = bytes;
    auto old = first->publish({&view,1}); REQUIRE(old); auto address = old->front().address();
    auto second = arena->reserve({&size,1}); REQUIRE(second); auto next = second->address(0); REQUIRE(next);
    if (!kind) p->fail_write = p->writes+1; else if (kind == 1) p->fail_seal = p->seals+1; else p->fail_flush = p->flushes+1;
    auto failed = second->publish({&view,1}); REQUIRE_FALSE(failed); CHECK_FALSE(p->writable);
    CHECK(failed.error().message.find("injected") != std::string::npos);
    CHECK(std::equal(bytes.begin(),bytes.end(),old->front().bytes().begin())); CHECK(old->front().address() == address);
    auto retried = second->publish({&view,1}); REQUIRE(retried); CHECK(retried->front().address() == *next);
    for (std::size_t n = 1; n < p->written.size(); ++n) CHECK(p->written[n].data == reinterpret_cast<std::uint8_t *>(*next));
    for (std::size_t n = 1; n < p->sealed.size(); ++n) CHECK(p->sealed[n].data == reinterpret_cast<std::uint8_t *>(*next));
    CHECK(p->allocations == 1); CHECK(arena->remaining() == 0);
  }
}
TEST_CASE("owning code arenas validate capacity layout providers and abandoned reservations without allocating again") {
  auto p = std::make_shared<Provider>();
  for (auto size : {std::size_t{0},SIZE_MAX,SIZE_MAX-4094}) CHECK_FALSE(CodeArena::create(size,p));
  CHECK_FALSE(CodeArena::create(1,nullptr)); CHECK(p->allocations == 0);
  for (auto page : {0u,31u}) { p->page = page; CHECK_FALSE(CodeArena::create(1,p)); } CHECK(p->allocations == 0); p->page = 4096;
  for (unsigned bad = 0; bad < 3; ++bad) {
    p->fail_allocate = !bad ? p->allocations+1 : 0; p->bad_size = bad == 1; p->bad_alignment = bad == 2;
    CHECK_FALSE(CodeArena::create(1,p));
  }
  CHECK(p->releases == 2); p->bad_size = p->bad_alignment = false;
  std::optional<CodeReservation> retained;
  {
    auto arena = CodeArena::create(4097,p); REQUIRE(arena); CHECK(arena->capacity() == 8192);
    auto left = arena->remaining(); CHECK_FALSE(arena->reserve({}));
    for (auto size : {std::size_t{0},SIZE_MAX,SIZE_MAX-14}) CHECK_FALSE(arena->reserve({&size,1}));
    std::size_t huge = 8193; CHECK_FALSE(arena->reserve({&huge,1})); CHECK(arena->remaining() == left);
    std::size_t size = 16; { auto abandoned = arena->reserve({&size,1}); REQUIRE(abandoned); }
    CHECK(arena->remaining() == 4096); auto r = arena->reserve({&size,1}); REQUIRE(r); retained.emplace(std::move(*r));
    CHECK(p->releases == 2);
  }
  CHECK(p->releases == 2); retained.reset(); CHECK(p->releases == 3);
}
TEST_CASE("native code arenas execute earlier batches while later batches publish and retain all mappings independently") {
  std::vector<ExecutableCode> code;
  {
    auto provider = native_code_memory_provider(); auto page = provider->page_size(); auto arena = CodeArena::create(32*page,provider); REQUIRE(arena);
    auto base = arena->address();
    for (unsigned n = 0; n < 32; ++n) {
      auto bytes = scalar(n+1); const auto size = bytes.size(); auto r = arena->reserve({&size,1}); REQUIRE(r);
      CHECK(*r->address(0) == base+n*page);
      for (auto &old : code) CHECK(reinterpret_cast<int (*)()>(old.address())() > 0);
      std::span<const std::uint8_t> view = bytes; auto batch = r->publish({&view,1}); REQUIRE(batch); code.push_back(std::move(batch->front()));
    }
    CHECK(arena->remaining() == 0);
  }
  for (unsigned n = 0; n < code.size(); ++n) CHECK(reinterpret_cast<int (*)()>(code[n].address())() == int(n+1));
}
