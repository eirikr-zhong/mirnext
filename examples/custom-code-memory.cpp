/* Licensed under the MIT License. A C++ code-memory service with owned lifetime. */
#include <mirnext/Execution/Session.hpp>
#include <array>
#include <atomic>
#include <iostream>

class MeteredCodeMemory final : public mirnext::execution::CodeMemoryProvider {
 public:
  std::size_t page_size () const override { return native_->page_size (); }
  mirnext::Result<mirnext::execution::MappedCodeMemory> allocate (std::size_t bytes) override {
    auto memory = native_->allocate (bytes);
    if (!memory) return memory.error ();
    active_.fetch_add (memory->size, std::memory_order_relaxed);
    allocations_.fetch_add (1, std::memory_order_relaxed);
    return *memory;
  }
  mirnext::Result<void> release (mirnext::execution::MappedCodeMemory memory) override {
    auto released = native_->release (memory);
    if (!released) return released.error ();
    active_.fetch_sub (memory.size, std::memory_order_relaxed);
    releases_.fetch_add (1, std::memory_order_relaxed);
    return {};
  }
  mirnext::Result<void> make_writable (mirnext::execution::MappedCodeMemory range) override {
    return native_->make_writable (range);
  }
  mirnext::Result<void> make_executable (mirnext::execution::MappedCodeMemory range) override {
    return native_->make_executable (range);
  }
  mirnext::Result<void> flush (mirnext::execution::MappedCodeMemory range) override {
    return native_->flush (range);
  }
  std::size_t active_bytes () const { return active_.load (std::memory_order_relaxed); }
  std::size_t allocations () const { return allocations_.load (std::memory_order_relaxed); }
  std::size_t releases () const { return releases_.load (std::memory_order_relaxed); }

 private:
  std::shared_ptr<mirnext::execution::CodeMemoryProvider> native_
    = mirnext::execution::native_code_memory_provider ();
  std::atomic<std::size_t> active_{0}, allocations_{0}, releases_{0};
};

mirnext::Result<mirnext::execution::FunctionHandle> build (
  std::shared_ptr<MeteredCodeMemory> provider) {
  using namespace mirnext;
  using namespace mirnext::ir;
  Context context;
  auto module = context.create_module ("metered");
  if (!module) return module.error ();
  auto function = (*module)->create_function ("entry", {{Type::i64 ()}, {Type::i64 ()}});
  if (!function) return function.error ();
  auto block = (*function)->create_block ();
  if (!block) return block.error ();
  IRBuilder body (**block);
  auto returned = body.ret (body.expression (*(*function)->arguments ()[0]) + 1);
  if (!returned) return returned.error ();
  execution::ExecutionOptions options;
  options.link.memory_provider = std::move (provider);
  options.link.compilation = execution::JITCompilation::Lazy;
  auto session = execution::ExecutionSession::create (context, std::move (options));
  if (!session) return session.error ();
  return session->find_function ("metered", "entry");
}

int main () {
  auto provider = std::make_shared<MeteredCodeMemory> ();
  {
    auto entry = build (provider);
    if (!entry) {
      std::cerr << entry.error ().message << '\n';
      return 1;
    }
    // Source Context and session are gone. The handle retains both the loaded
    // program and the provider used by first-call compilation and publication.
    std::array<std::int64_t, 1> arguments{41};
    auto value = entry->call_i64 (arguments);
    if (!value) {
      std::cerr << value.error ().message << '\n';
      return 1;
    }
    if (*value != 42 || !provider->active_bytes ()) return 1;
    std::cout << "entry(41) = " << *value << ", mapped bytes = " << provider->active_bytes ()
              << '\n';
  }
  std::cout << "mappings = " << provider->allocations () << ", releases = " << provider->releases ()
            << '\n';
  return provider->active_bytes () == 0 && provider->allocations () != 0
             && provider->allocations () == provider->releases ()
           ? 0
           : 1;
}
