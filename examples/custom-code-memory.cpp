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
    if (!memory.has_value ()) return memory.error ();
    active_.fetch_add (memory.value ().size, std::memory_order_relaxed);
    allocations_.fetch_add (1, std::memory_order_relaxed);
    return memory.value ();
  }
  mirnext::Result<void> release (mirnext::execution::MappedCodeMemory memory) override {
    auto released = native_->release (memory);
    if (!released.has_value ()) return released.error ();
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
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("metered");
  builder.create_function<std::int64_t (std::int64_t)> (module, "entry");
  builder.create_return (builder.create_add (builder.argument (0), builder.constant_i64 (1)));
  auto built = builder.finish ();
  if (!built.has_value ()) {
    return built.error ();
  }
  execution::ExecutionOptions options;
  options.link.memory_provider = std::move (provider);
  options.link.compilation = execution::JITCompilation::Lazy;
  auto session = execution::ExecutionSession::create (std::move (program), std::move (options));
  if (!session.has_value ()) return session.error ();
  return session.value ().find_function ("metered", "entry");
}

int main () {
  auto provider = std::make_shared<MeteredCodeMemory> ();
  {
    auto entry = build (provider);
    if (!entry.has_value ()) {
      std::cerr << entry.error ().message << '\n';
      return 1;
    }
    // Source Program and session are gone. The handle retains both the loaded
    // program and the provider used by first-call compilation and publication.
    auto value = entry.value ().call_i64 ({41});
    if (!value.has_value ()) {
      std::cerr << value.error ().message << '\n';
      return 1;
    }
    if (value.value () != 42 || !provider->active_bytes ()) return 1;
    std::cout << "entry(41) = " << value.value ()
              << ", mapped bytes = " << provider->active_bytes () << '\n';
  }
  std::cout << "mappings = " << provider->allocations () << ", releases = " << provider->releases ()
            << '\n';
  return provider->active_bytes () == 0 && provider->allocations () != 0
             && provider->allocations () == provider->releases ()
           ? 0
           : 1;
}
