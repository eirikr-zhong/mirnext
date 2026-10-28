/* Licensed under the MIT License. Thread-local checked native checkpoints. */
#include "mirnext/Execution/detail/Recovery.hpp"
#include "mirnext/Execution/detail/NativeBoundary.hpp"
#include <cstring>
namespace mirnext::execution::detail {
namespace {
thread_local codegen::NativeInvocationState *current_state = nullptr;
struct RawFrame {
  codegen::NativeInvocationState state;
  codegen::NativeInvocationState *previous;
  std::uintptr_t original_return;
};
struct RawFrames {
  std::vector<std::unique_ptr<RawFrame>> frames;
  std::size_t depth = 0;
};
thread_local RawFrames raw_frames;
std::uintptr_t checkpoint () noexcept { return reinterpret_cast<std::uintptr_t> (current_state); }
}  // namespace
RecoveryActivation::RecoveryActivation (codegen::NativeInvocationState &state) noexcept
  : previous_ (current_state) {
  current_state = &state;
}
RecoveryActivation::~RecoveryActivation () { current_state = previous_; }
NativeBoundaryStateStatistics native_boundary_state_statistics () {
  return {raw_frames.depth, raw_frames.frames.size ()};
}
std::uintptr_t NativeBoundary::enter (const codegen::NativeInvocationState *incoming,
                                      const Descriptor *descriptor) noexcept {
  if (!incoming || !descriptor) return 0;
  if (raw_frames.depth == raw_frames.frames.size ())
    raw_frames.frames.push_back (std::make_unique<RawFrame> ());
  auto &frame = *raw_frames.frames[raw_frames.depth++];
  // Some CPU-specific slots are intentionally unused. Copy object bytes rather
  // than evaluating indeterminate scalar members of the other architecture.
  std::memcpy (&frame.state, incoming, sizeof (frame.state));
  frame.previous = current_state;
  frame.original_return = incoming->return_address;
  frame.state.general[10] = descriptor->target;
  frame.state.return_address = descriptor->continuation;
  frame.state.x87_results = descriptor->x87_results;
  current_state = &frame.state;
  return reinterpret_cast<std::uintptr_t> (&frame.state);
}
std::uintptr_t NativeBoundary::leave () noexcept {
  if (!raw_frames.depth) return 0;
  auto &frame = *raw_frames.frames[raw_frames.depth - 1];
  if (current_state != &frame.state) return 0;
#if defined(__x86_64__) || defined(_M_X64)
  // RET already consumed the substituted PC. Restore the original slot before
  // handing the caller its unchanged argument stack and continuation.
  std::memcpy (reinterpret_cast<void *> (frame.state.original_sp), &frame.original_return,
               sizeof (frame.original_return));
#endif
  current_state = frame.previous;
  --raw_frames.depth;
  return frame.original_return;
}
Result<NativeRecovery> NativeRecovery::create (std::shared_ptr<CodeMemoryProvider> provider) {
  auto target = target::TargetMachine::native ();
  if (!(target).has_value ()) return target.error ();
  auto bytes = codegen::make_invocation_entries ((target).value (),
                                                 reinterpret_cast<std::uintptr_t> (&checkpoint));
  if (!(bytes).has_value ()) return bytes.error ();
  const std::array sizes{bytes.value ().invoke.size (), bytes.value ().recover.size ()};
  auto reservation = CodeMemory (std::move (provider)).reserve (sizes);
  if (!(reservation).has_value ()) return reservation.error ();
  const std::array<std::span<const std::uint8_t>, 2> buffers{bytes.value ().invoke,
                                                             bytes.value ().recover};
  auto code = reservation.value ().publish (buffers);
  if (!(code).has_value ()) return code.error ();
  return NativeRecovery (std::move (((code).value ())[0]), std::move (((code).value ())[1]));
}
}  // namespace mirnext::execution::detail
