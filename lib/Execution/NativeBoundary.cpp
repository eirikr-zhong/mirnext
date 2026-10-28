/* Licensed under the MIT License. Immutable native gates and owned descriptors. */
#include "mirnext/Execution/detail/NativeBoundary.hpp"
namespace mirnext::execution::detail {
Result<NativeBoundary> NativeBoundary::create (std::shared_ptr<CodeMemoryProvider> provider) {
  auto target = target::TargetMachine::native ();
  if (!(target).has_value ()) return target.error ();
  auto recovery = NativeRecovery::create (provider);
  if (!(recovery).has_value ()) return recovery.error ();
  auto bytes = codegen::make_native_boundary_entries ((target).value (),
                                                      reinterpret_cast<std::uintptr_t> (&enter),
                                                      reinterpret_cast<std::uintptr_t> (&leave));
  if (!(bytes).has_value ()) return bytes.error ();
  const std::array sizes{bytes.value ().enter.size (), bytes.value ().leave.size ()};
  auto reserved = CodeMemory (provider).reserve (sizes);
  if (!(reserved).has_value ()) return reserved.error ();
  const std::array<std::span<const std::uint8_t>, 2> buffers{bytes.value ().enter,
                                                             bytes.value ().leave};
  auto code = reserved.value ().publish (buffers);
  if (!(code).has_value ()) return code.error ();
  return NativeBoundary (std::move ((recovery).value ()), std::move (((code).value ())[0]),
                         std::move (((code).value ())[1]), std::move (provider));
}
Result<std::vector<std::uintptr_t>> NativeBoundary::add_entries (
  std::span<const NativeBoundaryTarget> targets) {
  if (!enter_.address () || !leave_.address ())
    return Error{ErrorCode::InvalidArgument, "moved-from native boundary"};
  if (targets.empty ()) return std::vector<std::uintptr_t>{};
  auto target = target::TargetMachine::native ();
  if (!(target).has_value ()) return target.error ();
  for (auto &entry : targets)
    if (!entry.address
        || (target.value ().architecture () == target::Architecture::AArch64
            && (entry.address % 4 || entry.x87_results))
        || entry.x87_results > 2
        || (target.value ().abi () == target::ABI::X86Windows && entry.x87_results))
      return Error{ErrorCode::InvalidArgument,
                   "invalid native boundary target or x87 result count"};
  std::vector<std::unique_ptr<Descriptor>> descriptors;
  std::vector<std::vector<std::uint8_t>> buffers;
  std::vector<std::size_t> sizes;
  descriptors.reserve (targets.size ());
  buffers.reserve (targets.size ());
  sizes.reserve (targets.size ());
  for (auto &entry : targets) {
    auto descriptor = std::make_unique<Descriptor> (
      Descriptor{entry.address, leave_.address (), entry.x87_results});
    auto bytes
      = codegen::make_native_boundary_gate ((target).value (),
                                            reinterpret_cast<std::uintptr_t> (descriptor.get ()),
                                            enter_.address ());
    if (!(bytes).has_value ()) return bytes.error ();
    sizes.push_back (bytes.value ().size ());
    buffers.push_back (std::move ((bytes).value ()));
    descriptors.push_back (std::move (descriptor));
  }
  auto reserved = memory_.reserve (sizes);
  if (!(reserved).has_value ()) return reserved.error ();
  std::vector<std::span<const std::uint8_t>> views;
  views.reserve (buffers.size ());
  for (auto &buffer : buffers) views.emplace_back (buffer);
  auto code = reserved.value ().publish (views);
  if (!(code).has_value ()) return code.error ();
  std::vector<std::uintptr_t> addresses;
  addresses.reserve (code.value ().size ());
  for (std::size_t n = 0; n < code.value ().size (); ++n) {
    addresses.push_back (((code).value ())[n].address ());
    entries_.push_back (std::move (((code).value ())[n]));
    descriptors_.push_back (std::move (descriptors[n]));
  }
  ++batches_;
  return addresses;
}
std::size_t NativeBoundary::code_bytes () const {
  auto size = recovery_.code_bytes () + enter_.bytes ().size () + leave_.bytes ().size ();
  for (auto &entry : entries_) size += entry.bytes ().size ();
  return size;
}
}  // namespace mirnext::execution::detail
