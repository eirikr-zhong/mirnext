/* Licensed under the MIT License. Native lazy block publication. */
#include "mirnext/Execution/detail/BBVRuntime.hpp"
#include "mirnext/CodeGen/BlockEntry.hpp"
#include "mirnext/Execution/Invocation.hpp"
namespace mirnext::execution::detail {
struct BBVRuntime::State {
  codegen::BBVProgram program;
  BBVRuntimeOptions options;
  CodeArena arena;
  enum class Phase { Cold, Compiling, Compiled };
  struct Block {
    State &owner;
    std::size_t index;
    Block (State &state, std::size_t n) : owner (state), index (n) {}
    const codegen::BBVVersion *version = nullptr;
    Phase phase = Phase::Cold;
    std::uintptr_t gate = 0, destination = 0;
    std::optional<codegen::CompiledBlock> artifact;
    std::optional<CodeReservation> reservation;
    std::optional<ExecutableCode> code;
  };
  // Sized once before any descriptor escapes. Neither the state nor this
  // storage moves afterwards; artifacts and destination slots stay stable.
  std::vector<Block> blocks;
  const codegen::BBVVersion *entry = nullptr;
  std::vector<ExecutableCode> entries;
  BBVRuntimeStatistics statistics;
  State (codegen::BBVProgram p, BBVRuntimeOptions o, CodeArena a)
    : program (std::move (p)), options (std::move (o)), arena (std::move (a)) {}
  Error diagnostic (Error error, std::string_view stage,
                    std::optional<std::size_t> block = {}) const {
    auto &f = program.preparation ().function ();
    error.message = std::string (f.module_name ()) + '/' + std::string (f.name ()) + " ["
                    + std::string (f.target ().name ()) + "/bbv-" + std::string (stage)
                    + (block ? " #" + std::to_string (*block) : "")
                    + "]: " + std::move (error.message);
    return error;
  }
  Result<Block *> lookup (const codegen::BlockVersionId &id) {
    if (!entry || !id.same_program (entry->id ()) || id.index () >= blocks.size ()
        || !blocks[id.index ()].version)
      return diagnostic ({ErrorCode::InvalidOperand, "foreign or unselected block version"},
                         "lookup");
    return &blocks[id.index ()];
  }
  Result<void> remember (const codegen::BBVVersion &v) {
    if (!v.id ().same_program (entry->id ()) || v.id ().index () >= blocks.size ())
      return diagnostic ({ErrorCode::InvalidOperand, "invalid selected version"}, "select");
    auto &b = blocks[v.id ().index ()];
    if (b.version && b.version != &v)
      return diagnostic ({ErrorCode::InvalidOperand, "first-version identity changed"}, "select",
                         b.index);
    b.version = &v;
    return {};
  }
  static std::uintptr_t resolve (Block *block) {
    auto result = block->owner.compile (*block);
    if (!result) {
      report_execution_error (result.error ());
      return 0;
    }
    return block->destination;
  }
  Result<void> initialize (std::vector<std::uint8_t> resolver) {
    const auto &prepared = program.preparation ();
    blocks.reserve (std::distance (prepared.blocks ().begin (), prepared.blocks ().end ()));
    for (auto &b : prepared.blocks ()) blocks.emplace_back (*this, b.index ());
    if (blocks.empty ())
      return diagnostic ({ErrorCode::InvalidOperand, "empty block layout"}, "prepare");
    auto properties = codegen::BBVProperties::create (prepared.function ());
    if (!properties) return diagnostic (properties.error (), "prepare");
    auto version = program.request_version (*prepared.blocks ().begin (), *properties, true);
    if (!version) return diagnostic (version.error (), "prepare");
    entry = version->version;
    auto saved = remember (*entry);
    if (!saved) return saved;
    auto &target = prepared.function ().target ();
    std::vector<std::size_t> sizes (blocks.size () + 1, codegen::block_entry_bytes);
    sizes[0] = resolver.size ();
    auto slots = arena.reserve (sizes);
    if (!slots) return diagnostic (slots.error (), "reserve");
    auto resolver_address = slots->address (0);
    if (!resolver_address) return diagnostic (resolver_address.error (), "reserve");
    std::vector<std::vector<std::uint8_t>> bytes;
    bytes.reserve (sizes.size ());
    bytes.push_back (std::move (resolver));
    for (std::size_t n = 0; n < blocks.size (); ++n) {
      auto &b = blocks[n];
      auto address = slots->address (n + 1);
      if (!address) return diagnostic (address.error (), "reserve", n);
      b.gate = *address;
      b.destination = b.gate + codegen::block_entry_cold_offset;
      auto gate
        = codegen::make_block_entry (target,
                                     {reinterpret_cast<std::uintptr_t> (&b.destination),
                                      reinterpret_cast<std::uintptr_t> (&b), *resolver_address});
      if (!gate) return diagnostic (gate.error (), "prepare", n);
      bytes.push_back (std::move (*gate));
    }
    std::vector<std::span<const std::uint8_t>> views;
    views.reserve (bytes.size ());
    for (auto &b : bytes) views.push_back (b);
    auto published = slots->publish (views);
    if (!published) return diagnostic (published.error (), "publish");
    entries = std::move (*published);
    statistics.blocks = blocks.size ();
    statistics.code_batches = 1;
    for (auto &code : entries) statistics.code_bytes += code.bytes ().size ();
    return {};
  }
  Result<void> compile (Block &b) {
    if (b.phase == Phase::Compiled) return {};
    if (b.phase == Phase::Compiling)
      return diagnostic ({ErrorCode::InvalidOperand, "recursive compilation of the same block"},
                         "encode", b.index);
    b.phase = Phase::Compiling;
    ++statistics.compilation_attempts;
    struct Reset {
      Block &block;
      ~Reset () {
        if (block.phase == Phase::Compiling) block.phase = Phase::Cold;
      }
    } reset{b};
    if (!b.artifact) {
      auto artifact
        = codegen::encode_bbv (program, *b.version, {.known_addresses = options.resolve});
      if (!artifact) return diagnostic (artifact.error (), "encode", b.index);
      for (auto &edge : b.version->specialization ()->successors ()) {
        auto saved = remember (*edge.version);
        if (!saved) return saved;
      }
      b.artifact.emplace (std::move (*artifact));
    }
    if (options.observe) {
      auto observed = options.observe (*b.version, *b.artifact);
      if (!observed) return diagnostic (observed.error (), "observe", b.index);
    }
    if (!b.reservation) {
      const auto size = b.artifact->bytes ().size ();
      auto slots = arena.reserve ({&size, 1});
      if (!slots) return diagnostic (slots.error (), "reserve", b.index);
      b.reservation.emplace (std::move (*slots));
    }
    auto address = b.reservation->address (0);
    if (!address) return diagnostic (address.error (), "reserve", b.index);
    auto bytes = b.artifact->relocate (
      *address,
      [&] (const codegen::BlockVersionId &id) -> Result<std::uint64_t> {
        auto target = lookup (id);
        if (!target) return target.error ();
        return (*target)->gate;
      },
      options.resolve);
    if (!bytes) return diagnostic (bytes.error (), "relocate", b.index);
    std::span<const std::uint8_t> view = *bytes;
    auto published = b.reservation->publish ({&view, 1});
    if (!published) return diagnostic (published.error (), "publish", b.index);
    b.code.emplace (std::move (published->front ()));
    b.reservation.reset ();
    // Only a successfully published target becomes visible through the gate.
    b.destination = b.code->address ();
    b.phase = Phase::Compiled;
    ++statistics.compiled_blocks;
    ++statistics.code_batches;
    statistics.code_bytes += b.code->bytes ().size ();
    return {};
  }
};
BBVRuntime::BBVRuntime (std::unique_ptr<State> state) : state_ (std::move (state)) {}
BBVRuntime::~BBVRuntime () = default;
bool BBVRuntime::compiling () const {
  for (auto &block : state_->blocks)
    if (block.phase == State::Phase::Compiling) return true;
  return false;
}
Result<std::unique_ptr<BBVRuntime>> BBVRuntime::create (codegen::BBVProgram program,
                                                        BBVRuntimeOptions options) {
  auto valid = program.verify ();
  if (!valid) return valid.error ();
  auto &f = program.preparation ().function ();
  auto diagnostic = [&] (Error error) {
    error.message = std::string (f.module_name ()) + '/' + std::string (f.name ()) + " ["
                    + std::string (f.target ().name ())
                    + "/bbv-prepare]: " + std::move (error.message);
    return error;
  };
  auto resolver
    = codegen::make_block_resolver (f.target (), reinterpret_cast<std::uintptr_t> (&State::resolve),
                                    options.failure_continuation);
  if (!resolver) return diagnostic (resolver.error ());
  auto arena = CodeArena::create (options.code_capacity, options.memory_provider);
  if (!arena) return diagnostic (arena.error ());
  auto state
    = std::make_unique<State> (std::move (program), std::move (options), std::move (*arena));
  auto initialized = state->initialize (std::move (*resolver));
  if (!initialized) return initialized.error ();
  return std::unique_ptr<BBVRuntime> (new BBVRuntime (std::move (state)));
}
const codegen::BBVProgram &BBVRuntime::program () const { return state_->program; }
const codegen::BBVVersion &BBVRuntime::entry_version () const { return *state_->entry; }
std::uintptr_t BBVRuntime::entry_address () const {
  return state_->blocks[state_->entry->id ().index ()].gate;
}
Result<std::uintptr_t> BBVRuntime::address (const codegen::BlockVersionId &id) const {
  auto b = state_->lookup (id);
  if (!b) return b.error ();
  return (*b)->gate;
}
Result<void> BBVRuntime::compile (const codegen::BlockVersionId &id) {
  auto b = state_->lookup (id);
  if (!b) return b.error ();
  return state_->compile (**b);
}
const codegen::CompiledBlock *BBVRuntime::artifact (const codegen::BlockVersionId &id) const {
  auto b = state_->lookup (id);
  return b && (*b)->phase == State::Phase::Compiled ? &*(*b)->artifact : nullptr;
}
Result<std::uintptr_t> BBVRuntime::label_address (const codegen::MachineLabel &label) {
  auto block = state_->program.preparation ().block_for (label);
  if (!block) return state_->diagnostic (block.error (), "label");
  auto properties = codegen::BBVProperties::create (state_->program.preparation ().function ());
  if (!properties) return properties.error ();
  auto version = state_->program.request_version (**block, *properties);
  if (!version) return state_->diagnostic (version.error (), "label");
  auto saved = state_->remember (*version->version);
  if (!saved) return saved.error ();
  return address (version->version->id ());
}
const BBVRuntimeStatistics &BBVRuntime::statistics () const { return state_->statistics; }
std::size_t BBVRuntime::remaining_code_bytes () const { return state_->arena.remaining (); }
}  // namespace mirnext::execution::detail
