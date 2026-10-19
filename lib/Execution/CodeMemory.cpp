/* Licensed under the MIT License. Native OS mapping and atomic code publication. */
#include "mirnext/Execution/CodeMemory.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif
#endif
namespace mirnext::execution {
namespace {
Error invalid(std::string message) { return {ErrorCode::InvalidArgument,"code-memory: " + std::move(message)}; }
Error system_error(std::string operation) {
#ifdef _WIN32
  auto code = GetLastError();
#else
  auto code = errno;
#endif
  return invalid(std::move(operation) + " failed (OS error " + std::to_string(code) + ')');
}
class NativeCodeMemoryProvider final : public CodeMemoryProvider {
public:
  std::size_t page_size() const override {
#ifdef _WIN32
    SYSTEM_INFO info; GetSystemInfo(&info); return info.dwPageSize;
#else
    auto size = sysconf(_SC_PAGE_SIZE); return size > 0 ? static_cast<std::size_t>(size) : 0;
#endif
  }
  Result<MappedCodeMemory> allocate(std::size_t size) override {
#ifdef _WIN32
    auto data = VirtualAlloc(nullptr,size,MEM_COMMIT | MEM_RESERVE,PAGE_READWRITE);
    if (!data) return system_error("VirtualAlloc");
#else
#if defined(__APPLE__) && defined(__aarch64__)
    auto data = mmap(nullptr,size,PROT_READ | PROT_WRITE | PROT_EXEC,MAP_PRIVATE | MAP_ANON | MAP_JIT,-1,0);
#else
    auto data = mmap(nullptr,size,PROT_READ | PROT_WRITE,MAP_PRIVATE | MAP_ANON,-1,0);
#endif
    if (data == MAP_FAILED) return system_error("mmap");
#endif
    return MappedCodeMemory{static_cast<std::uint8_t *>(data),size};
  }
  Result<void> release(MappedCodeMemory memory) override {
#ifdef _WIN32
    if (!VirtualFree(memory.data,0,MEM_RELEASE)) return system_error("VirtualFree");
#else
    if (munmap(memory.data,memory.size)) return system_error("munmap");
#endif
    return {};
  }
  Result<void> make_writable(MappedCodeMemory memory) override {
#ifdef _WIN32
    DWORD old; if (!VirtualProtect(memory.data,memory.size,PAGE_READWRITE,&old)) return system_error("VirtualProtect(write)");
#elif defined(__APPLE__) && defined(__aarch64__)
    (void)memory; if (pthread_jit_write_protect_supported_np()) pthread_jit_write_protect_np(0);
#else
    if (mprotect(memory.data,memory.size,PROT_READ | PROT_WRITE)) return system_error("mprotect(write)");
#endif
    return {};
  }
  Result<void> make_executable(MappedCodeMemory memory) override {
#ifdef _WIN32
    DWORD old; if (!VirtualProtect(memory.data,memory.size,PAGE_EXECUTE_READ,&old)) return system_error("VirtualProtect(execute)");
#elif defined(__APPLE__) && defined(__aarch64__)
    (void)memory; if (pthread_jit_write_protect_supported_np()) pthread_jit_write_protect_np(1);
#else
    if (mprotect(memory.data,memory.size,PROT_READ | PROT_EXEC)) return system_error("mprotect(execute)");
#endif
    return {};
  }
  Result<void> flush(MappedCodeMemory memory) override {
#ifdef _WIN32
    if (!FlushInstructionCache(GetCurrentProcess(),memory.data,memory.size)) return system_error("FlushInstructionCache");
#elif defined(__APPLE__) && defined(__aarch64__)
    sys_icache_invalidate(memory.data,memory.size);
#elif defined(__GNUC__)
    __builtin___clear_cache(reinterpret_cast<char *>(memory.data),reinterpret_cast<char *>(memory.data + memory.size));
#else
    (void)memory;
#endif
    return {};
  }
};
}
std::shared_ptr<CodeMemoryProvider> native_code_memory_provider() { return std::make_shared<NativeCodeMemoryProvider>(); }
namespace detail {
struct CodeRegion {
  std::shared_ptr<CodeMemoryProvider> provider;
  MappedCodeMemory mapping;
  ~CodeRegion() { (void)provider->release(mapping); }
};
}
ExecutableCode::ExecutableCode(std::shared_ptr<const detail::CodeRegion> region,std::size_t offset,std::size_t size)
  : region_(std::move(region)),offset_(offset),size_(size) {}
std::uintptr_t ExecutableCode::address() const { return region_ ? reinterpret_cast<std::uintptr_t>(region_->mapping.data + offset_) : 0; }
std::span<const std::uint8_t> ExecutableCode::bytes() const { return region_ ? std::span<const std::uint8_t>(region_->mapping.data + offset_,size_) : std::span<const std::uint8_t>{}; }
CodeReservation::CodeReservation(std::shared_ptr<detail::CodeRegion> region,std::vector<Slot> slots,MappedCodeMemory publication)
  : region_(std::move(region)),slots_(std::move(slots)),publication_(publication) {}
CodeReservation::CodeReservation(CodeReservation &&) noexcept = default;
CodeReservation &CodeReservation::operator=(CodeReservation &&) noexcept = default;
CodeReservation::~CodeReservation() = default;
std::size_t CodeReservation::size() const { return region_ ? slots_.size() : 0; }
Result<std::uintptr_t> CodeReservation::address(std::size_t n) const {
  if (!region_ || n >= slots_.size()) return invalid("invalid or consumed reservation slot");
  return reinterpret_cast<std::uintptr_t>(region_->mapping.data + slots_[n].offset);
}
Result<std::vector<ExecutableCode>> CodeReservation::publish(std::span<const std::span<const std::uint8_t>> buffers) {
  if (!region_ || buffers.size() != slots_.size()) return invalid("invalid or consumed publication");
  for (std::size_t n = 0; n < buffers.size(); ++n)
    if (buffers[n].size() != slots_[n].size || !buffers[n].data()) return invalid("publication does not match reservation");
  // Allocate result bookkeeping before opening write access. No user code or
  // allocation is called while native Apple per-thread JIT writes are enabled.
  std::vector<ExecutableCode> code; code.reserve(slots_.size());
  auto &provider = *region_->provider; auto mapping = region_->mapping;
  auto writable = provider.make_writable(publication_);
  if (!writable) { (void)provider.make_executable(publication_); return writable.error(); }
  for (std::size_t n = 0; n < buffers.size(); ++n)
    std::memmove(mapping.data + slots_[n].offset,buffers[n].data(),buffers[n].size());
  auto executable = provider.make_executable(publication_);
  if (!executable) { (void)provider.make_executable(publication_); return executable.error(); }
  auto flushed = provider.flush(publication_); if (!flushed) return flushed.error();
  for (auto slot : slots_) code.push_back(ExecutableCode(region_,slot.offset,slot.size));
  region_.reset(); slots_.clear(); publication_ = {}; return code;
}
namespace {
template<class Slot> struct CodeSlotLayout { std::vector<Slot> slots; std::size_t length; };
template<class Slot> Result<CodeSlotLayout<Slot>> slot_layout(std::span<const std::size_t> sizes,std::size_t page) {
  if (sizes.empty()) return invalid("empty reservation");
  CodeSlotLayout<Slot> out; out.slots.reserve(sizes.size()); std::size_t length = 0;
  for (auto size : sizes) {
    if (!size || length > SIZE_MAX - 15) return invalid("empty allocation or code size overflow");
    length = (length + 15) / 16 * 16;
    if (size > SIZE_MAX - length) return invalid("code size overflow");
    out.slots.push_back({length,size}); length += size;
  }
  if (length > SIZE_MAX - (page - 1)) return invalid("mapping size overflow");
  out.length = (length + page - 1) / page * page; return out;
}
Result<std::shared_ptr<detail::CodeRegion>> allocate_region(const std::shared_ptr<CodeMemoryProvider> &provider,std::size_t length,std::size_t page) {
  auto memory = provider->allocate(length); if (!memory) return memory.error();
  if (!memory->data || reinterpret_cast<std::uintptr_t>(memory->data) % page || memory->size != length
      || length > UINTPTR_MAX - reinterpret_cast<std::uintptr_t>(memory->data)) {
    (void)provider->release(*memory); return invalid("invalid mapped region");
  }
  auto region = std::make_shared<detail::CodeRegion>(); region->provider = provider; region->mapping = *memory; return region;
}
}
Result<CodeReservation> CodeMemory::reserve(std::span<const std::size_t> sizes) const {
  if (!provider_) return invalid("missing provider");
  auto page = provider_->page_size(); if (!page || page%16) return invalid("invalid page size");
  auto layout = slot_layout<CodeReservation::Slot>(sizes,page); if (!layout) return layout.error();
  auto region = allocate_region(provider_,layout->length,page); if (!region) return region.error();
  return CodeReservation(*region,std::move(layout->slots),(*region)->mapping);
}
Result<CodeArena> CodeArena::create(std::size_t capacity,std::shared_ptr<CodeMemoryProvider> provider) {
  if (!provider) return invalid("missing arena provider");
  auto page = provider->page_size(); if (!page || page%16) return invalid("invalid arena page size");
  if (!capacity || capacity > SIZE_MAX-(page-1)) return invalid("empty arena or capacity overflow");
  auto length = (capacity+page-1)/page*page;
  auto region = allocate_region(provider,length,page); if (!region) return region.error(); return CodeArena(*region,page);
}
std::size_t CodeArena::capacity() const { return region_ ? region_->mapping.size : 0; }
std::size_t CodeArena::remaining() const { return region_ ? capacity()-used_ : 0; }
std::uintptr_t CodeArena::address() const { return region_ ? reinterpret_cast<std::uintptr_t>(region_->mapping.data) : 0; }
Result<CodeReservation> CodeArena::reserve(std::span<const std::size_t> sizes) {
  if (!region_) return invalid("moved-from code arena");
  auto layout = slot_layout<CodeReservation::Slot>(sizes,page_); if (!layout) return layout.error();
  if (layout->length > remaining()) return invalid("code arena capacity exhausted");
  for (auto &slot : layout->slots) slot.offset += used_;
  MappedCodeMemory publication{region_->mapping.data+used_,layout->length}; used_ += layout->length;
  return CodeReservation(region_,std::move(layout->slots),publication);
}
Result<ExecutableFunction> CodeMemory::publish(codegen::CompiledFunction artifact,const codegen::CodeSymbolResolver &resolver) const {
  auto target = target::TargetMachine::native(); if (!target) return target.error();
  auto diagnostic = [&](Error error) {
    error.message = std::string(artifact.module_name()) + '/' + std::string(artifact.name()) + " ["
      + std::string(target->name()) + "/publish]: " + error.message; return error;
  };
  if (artifact.architecture() != target->architecture()) return diagnostic(invalid("cannot publish code for another architecture"));
  auto valid = artifact.verify(); if (!valid) return diagnostic(valid.error());
  std::size_t size = artifact.bytes().size(); auto reservation = reserve({&size,1}); if (!reservation) return diagnostic(reservation.error());
  auto address = reservation->address(0); if (!address) return diagnostic(address.error());
  auto bytes = artifact.relocate(*address,resolver); if (!bytes) return diagnostic(bytes.error());
  std::span<const std::uint8_t> buffer = *bytes; auto code = reservation->publish({&buffer,1}); if (!code) return diagnostic(code.error());
  return ExecutableFunction(std::move(artifact),std::move((*code)[0]));
}
}
