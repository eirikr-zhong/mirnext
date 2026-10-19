/* Licensed under the MIT License. Pure x86 lazy resolver frame facts. */
#pragma once
#include "mirnext/Target/X86ABI.hpp"
namespace mirnext::codegen {
struct LazyEntryAddresses;
struct BlockEntryAddresses;
struct InvocationEntryCode;
struct NativeBoundaryCode;
namespace detail {
struct X86ResolverSave { target::X86Register reg; std::size_t offset; };
struct X86ResolverFrame {
  target::X86Register argument;
  std::size_t shadow_bytes, fx_offset, frame_bytes, metadata_offset;
  std::array<X86ResolverSave,13> saves;
};
// The cold gate pushes the x87 result count. The resolver restores the original
// call stack before entering the body (or returning after a compilation error).
// These layout facts can be tested without generating foreign machine code.
Result<X86ResolverFrame> x86_resolver_frame(target::ABI);
struct X86BlockResolverFrame {
  X86ResolverFrame saved;
  std::size_t red_zone_bytes;
};
// The exact incoming SP and RFLAGS are saved below the protected red zone.
// Metadata belongs inside the aligned callback frame, unlike a function gate.
Result<X86BlockResolverFrame> x86_block_resolver_frame(target::ABI);
struct X86InvocationFrame {
  std::array<target::X86Register,4> parameters;
  std::array<target::X86Register,8> preserved;
  unsigned preserved_count;
  std::size_t shadow_bytes,frame_bytes,checkpoint_offset,red_zone_bytes;
};
Result<X86InvocationFrame> x86_invocation_frame(target::ABI);
struct X86BoundaryFrame {
  target::X86Register first_parameter,second_parameter;
  std::size_t checkpoint_offset,frame_bytes,red_zone_bytes;
};
Result<X86BoundaryFrame> x86_boundary_frame(target::ABI);
Result<InvocationEntryCode> make_x86_invocation_entries(const target::TargetMachine &,std::uintptr_t);
Result<std::vector<std::uint8_t>> make_x86_lazy_entry(const target::TargetMachine &,LazyEntryAddresses);
Result<std::vector<std::uint8_t>> make_x86_lazy_resolver(const target::TargetMachine &,std::uintptr_t);
Result<std::vector<std::uint8_t>> make_x86_block_entry(const target::TargetMachine &,BlockEntryAddresses);
Result<std::vector<std::uint8_t>> make_x86_block_resolver(const target::TargetMachine &,std::uintptr_t,std::uintptr_t);
}
}
