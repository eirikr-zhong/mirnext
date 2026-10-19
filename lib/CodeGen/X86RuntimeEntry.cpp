/* Licensed under the MIT License. Immutable x86 lazy gates and shared resolver. */
#include "mirnext/CodeGen/RuntimeEntry.hpp"
#include "mirnext/CodeGen/BlockEntry.hpp"
#include "mirnext/CodeGen/InvocationEntry.hpp"
#include "mirnext/CodeGen/NativeBoundary.hpp"
#include "mirnext/CodeGen/Detail/X86RuntimeEntry.hpp"
namespace mirnext::codegen {
namespace detail {
Result<X86ResolverFrame> x86_resolver_frame(target::ABI abi) {
  using R = target::X86Register;
  if (abi != target::ABI::X86SysV && abi != target::ABI::X86Windows)
    return Error{ErrorCode::InvalidArgument,"invalid x86 lazy resolver ABI"};
  const std::size_t shadow = abi == target::ABI::X86Windows ? 32 : 0;
  X86ResolverFrame frame{abi == target::ABI::X86Windows ? R::CX : R::DI,shadow,shadow,0,0,{}};
  auto offset = shadow + 512; // FXSAVE64: complete XMM, x87 and control state.
  unsigned saved = 0;
  for (unsigned n = 0; n < 16; ++n) {
    if (n == unsigned(R::SP) || n == unsigned(R::R10) || n == unsigned(R::R11)) continue;
    frame.saves[saved++] = {static_cast<R>(n),offset}; offset += 8;
  }
  // After the gate's push, SP is 16-byte aligned. The callback gets its own
  // shadow space and cannot overwrite any saved MIR register or the metadata.
  frame.frame_bytes = (offset+15)&~std::size_t(15);
  frame.metadata_offset = frame.frame_bytes;
  return frame;
}
Result<X86BlockResolverFrame> x86_block_resolver_frame(target::ABI abi) {
  auto frame = x86_resolver_frame(abi); if (!frame) return frame.error();
  // Exact flags-stack pointer in the last slot, inside the aligned frame.
  frame->metadata_offset = frame->shadow_bytes+512+frame->saves.size()*8;
  frame->frame_bytes = (frame->metadata_offset+8+15)&~std::size_t(15);
  return X86BlockResolverFrame{*frame,abi == target::ABI::X86SysV ? 128u : 0u};
}
Result<X86InvocationFrame> x86_invocation_frame(target::ABI abi) {
  using R = target::X86Register;
  if (abi != target::ABI::X86SysV && abi != target::ABI::X86Windows)
    return Error{ErrorCode::InvalidArgument,"invalid x86 invocation checkpoint ABI"};
  X86InvocationFrame f{};
  bool windows = abi == target::ABI::X86Windows;
  f.parameters = windows ? std::array{R::CX,R::DX,R::R8,R::R9} : std::array{R::DI,R::SI,R::DX,R::CX};
  // BP is reserved for frame formation and deliberately absent from the
  // allocator's callee-save query. It is still nonvolatile at the host ABI.
  for (unsigned n = 0; n < 16; ++n) if (n == unsigned(R::BP) || (n != unsigned(R::SP) && target::x86_callee_saved_register(abi,static_cast<R>(n))))
    f.preserved[f.preserved_count++] = static_cast<R>(n);
  f.shadow_bytes = windows ? 32 : 0; f.frame_bytes = f.shadow_bytes+8;
  f.checkpoint_offset = f.shadow_bytes; f.red_zone_bytes = windows ? 0 : 128;
  return f;
}
Result<X86BoundaryFrame> x86_boundary_frame(target::ABI abi) {
  auto f = x86_invocation_frame(abi); if (!f) return f.error();
  return X86BoundaryFrame{f->parameters[0],f->parameters[1],f->shadow_bytes,
    f->shadow_bytes+sizeof(NativeInvocationState),f->red_zone_bytes};
}
}
namespace {
struct Code {
  std::vector<std::uint8_t> bytes;
  void emit(std::initializer_list<std::uint8_t> values) { bytes.insert(bytes.end(),values); }
  void integer(std::uint64_t value,unsigned size) { for (unsigned n = 0; n < size; ++n) bytes.push_back(std::uint8_t(value>>(8*n))); }
  void address(unsigned reg,std::uintptr_t value) { emit({std::uint8_t(0x48|(reg>>3)),std::uint8_t(0xb8|(reg&7))}); integer(value,8); }
  void memory(std::uint8_t opcode,unsigned reg,std::size_t offset) {
    emit({std::uint8_t(0x48|((reg>>3)<<2)),opcode,std::uint8_t(0x84|((reg&7)<<3)),0x24}); integer(offset,4);
  }
  void fx(bool restore,std::size_t offset) { emit({0x48,0x0f,0xae,std::uint8_t(restore ? 0x8c : 0x84),0x24}); integer(offset,4); }
  void stack(bool restore,std::size_t size) { emit({0x48,0x81,std::uint8_t(restore ? 0xc4 : 0xec)}); integer(size,4); }
  std::size_t branch(std::uint8_t opcode) { emit({opcode,0}); return bytes.size()-1; }
  void bind(std::size_t displacement) { bytes[displacement] = std::uint8_t(bytes.size()-displacement-1); }
  void move(unsigned destination,unsigned source) {
    emit({std::uint8_t(0x48|((source>>3)<<2)|(destination>>3)),0x89,std::uint8_t(0xc0|((source&7)<<3)|(destination&7))});
  }
  void state(std::uint8_t opcode,unsigned reg,std::size_t offset) {
    emit({std::uint8_t(0x49|((reg>>3)<<2)),opcode,std::uint8_t(0x83|((reg&7)<<3))}); integer(offset,4); // [r11+disp32]
  }
  void state_fx(bool restore) { emit({0x49,0x0f,0xae,std::uint8_t(restore ? 0x8b : 0x83)}); integer(800,4); }
};
}
Result<std::vector<std::uint8_t>> detail::make_x86_lazy_entry(const target::TargetMachine &target,LazyEntryAddresses a) {
  if (target.architecture() != target::Architecture::X86_64 || !a.destination || a.destination%alignof(std::uintptr_t)
      || !a.descriptor || !a.resolver || a.x87_results > 2 || (target.abi() == target::ABI::X86Windows && a.x87_results))
    return Error{ErrorCode::InvalidArgument,"invalid x86 lazy entry target or addresses"};
  Code c; c.bytes.reserve(lazy_entry_bytes);
  c.address(11,a.destination); c.emit({0x4d,0x8b,0x1b}); // mov r11,[r11]
  c.emit({0x4d,0x85,0xdb}); const auto warm = c.branch(0x75); // test r11; jnz warm
  c.address(10,a.descriptor);
  c.emit({0x6a,std::uint8_t(a.x87_results)}); // cold-only signature metadata
  c.address(11,a.resolver); c.emit({0x41,0xff,0xe3}); // jmp r11
  c.bind(warm); c.emit({0x41,0xff,0xe3});
  // Immutable entries share a resolver. Warm calls do not touch SP, AX/AL or
  // any argument/bound register; R10/R11 are reserved by the machine target.
  if (c.bytes.size() > lazy_entry_bytes) return Error{ErrorCode::InvalidArgument,"x86 lazy entry exceeds its fixed slot"};
  c.bytes.resize(lazy_entry_bytes,0x90); return std::move(c.bytes);
}
Result<std::vector<std::uint8_t>> detail::make_x86_lazy_resolver(const target::TargetMachine &target,std::uintptr_t callback) {
  if (target.architecture() != target::Architecture::X86_64 || !callback)
    return Error{ErrorCode::InvalidArgument,"invalid x86 lazy resolver target or callback"};
  auto frame = x86_resolver_frame(target.abi()); if (!frame) return frame.error();
  Code c; c.bytes.reserve(384); c.stack(false,frame->frame_bytes);
  for (auto &save : frame->saves) c.memory(0x89,unsigned(save.reg),save.offset);
  c.fx(false,frame->fx_offset);
  c.emit({0x4c,0x89,std::uint8_t(0xd0|unsigned(frame->argument))}); // descriptor r10 -> ABI first argument
  c.address(11,callback); c.emit({0x41,0xff,0xd3,0x49,0x89,0xc3}); // call r11; mov r11,rax
  c.fx(true,frame->fx_offset);
  for (auto &save : frame->saves) c.memory(0x8b,unsigned(save.reg),save.offset);
  c.memory(0x8b,10,frame->metadata_offset);
  c.stack(true,frame->frame_bytes+8); // discard metadata; restore original call arguments
  c.emit({0x4d,0x85,0xdb}); const auto resolved = c.branch(0x75);
  // The C++ callback has reported the failure to ExecutionInvocation. Zero the
  // ABI result banks and return without executing a partial/unpublished body.
  c.emit({0x31,0xc0,0x31,0xd2,0x66,0x0f,0xef,0xc0,0x66,0x0f,0xef,0xc9});
  c.emit({0x45,0x85,0xd2}); const auto no_x87 = c.branch(0x74);
  c.emit({0xd9,0xee,0x41,0x83,0xfa,0x01}); const auto one_x87 = c.branch(0x74);
  c.emit({0xd9,0xee}); c.bind(no_x87); c.bind(one_x87); c.emit({0xc3});
  c.bind(resolved); c.emit({0x41,0xff,0xe3}); return std::move(c.bytes);
}
Result<std::vector<std::uint8_t>> detail::make_x86_block_entry(const target::TargetMachine &target,BlockEntryAddresses a) {
  if (target.architecture() != target::Architecture::X86_64 || !a.destination || a.destination%alignof(std::uintptr_t)
      || !a.descriptor || !a.resolver)
    return Error{ErrorCode::InvalidArgument,"invalid x86 block entry target or addresses"};
  Code c; c.bytes.reserve(block_entry_bytes);
  c.address(11,a.destination); c.emit({0x41,0xff,0x23}); // jmp [r11], flags unchanged
  c.bytes.resize(block_entry_cold_offset,0x90);
  c.address(10,a.descriptor); c.address(11,a.resolver); c.emit({0x41,0xff,0xe3});
  c.bytes.resize(block_entry_bytes,0x90); return std::move(c.bytes);
}
Result<std::vector<std::uint8_t>> detail::make_x86_block_resolver(const target::TargetMachine &target,
    std::uintptr_t callback,std::uintptr_t failure) {
  if (target.architecture() != target::Architecture::X86_64 || !callback || !failure)
    return Error{ErrorCode::InvalidArgument,"invalid x86 block resolver target callback or failure continuation"};
  auto layout = x86_block_resolver_frame(target.abi()); if (!layout) return layout.error(); auto &frame = layout->saved;
  Code c; c.bytes.reserve(384);
  // LEA preserves the incoming flags and protects live SysV red-zone storage
  // before the first push. Blocks need not have ordinary function-entry SP.
  if (layout->red_zone_bytes) c.memory(0x8d,4,std::size_t(0)-layout->red_zone_bytes);
  c.emit({0x9c,0x49,0x89,0xe3,0x48,0x83,0xe4,0xf0}); // pushfq; mov r11,rsp; and rsp,-16
  c.stack(false,frame.frame_bytes); c.memory(0x89,11,frame.metadata_offset);
  for (auto &save : frame.saves) c.memory(0x89,unsigned(save.reg),save.offset);
  c.fx(false,frame.fx_offset);
  c.emit({0x4c,0x89,std::uint8_t(0xd0|unsigned(frame.argument))});
  c.address(11,callback); c.emit({0x41,0xff,0xd3,0x49,0x89,0xc3,0x4d,0x85,0xdb});
  auto resolved = c.branch(0x75); c.address(11,failure); c.bind(resolved);
  c.fx(true,frame.fx_offset);
  for (auto &save : frame.saves) c.memory(0x8b,unsigned(save.reg),save.offset);
  c.memory(0x8b,4,frame.metadata_offset); c.emit({0x9d}); // mov rsp,[rsp+metadata]; popfq
  if (layout->red_zone_bytes) c.memory(0x8d,4,layout->red_zone_bytes);
  c.emit({0x41,0xff,0xe3}); return std::move(c.bytes);
}
Result<InvocationEntryCode> detail::make_x86_invocation_entries(const target::TargetMachine &target,std::uintptr_t callback) {
  if (target.architecture() != target::Architecture::X86_64 || !callback)
    return Error{ErrorCode::InvalidArgument,"invalid x86 invocation checkpoint callback"};
  auto layout = x86_invocation_frame(target.abi()); if (!layout) return layout.error(); auto &f = *layout;
  Code call; call.bytes.reserve(320); call.move(11,unsigned(f.parameters[3]));
  for (unsigned n = 0; n < f.preserved_count; ++n) call.state(0x89,unsigned(f.preserved[n]),unsigned(f.preserved[n])*8);
  call.move(10,4); call.state(0x89,10,768); call.memory(0x8b,10,0); call.state(0x89,10,776);
  call.state_fx(false); call.move(10,unsigned(f.parameters[0]));
  // Checked adapters return only void/integer/double. Raw ABI boundaries set
  // this field from their declared signature instead.
  call.emit({0x49,0xc7,0x83}); call.integer(1312,4); call.integer(0,4);
  call.stack(false,f.frame_bytes); call.memory(0x89,11,f.checkpoint_offset);
  call.move(unsigned(f.parameters[0]),unsigned(f.parameters[1])); call.move(unsigned(f.parameters[1]),unsigned(f.parameters[2]));
  call.emit({0x41,0xff,0xd2}); // call r10
  call.memory(0x8b,11,f.checkpoint_offset);
  if (target.abi() == target::ABI::X86Windows) for (unsigned n = 6; n < 16; ++n) {
    // Preserve full nonvolatile XMM values without restoring result XMM0 or
    // discarding normal floating-environment changes.
    call.emit({0xf3,std::uint8_t(0x41|((n>>3)<<2)),0x0f,0x6f,std::uint8_t(0x83|((n&7)<<3))}); call.integer(800+160+n*16,4);
  }
  for (unsigned n = 0; n < f.preserved_count; ++n) call.state(0x8b,unsigned(f.preserved[n]),unsigned(f.preserved[n])*8);
  call.state(0x8b,4,768); call.emit({0xc3});
  Code recovery;
  if (f.red_zone_bytes) recovery.memory(0x8d,4,std::size_t(0)-f.red_zone_bytes);
  recovery.emit({0x48,0x83,0xe4,0xf0}); // align arbitrary block SP
  if (f.shadow_bytes) recovery.stack(false,f.shadow_bytes);
  recovery.address(11,callback); recovery.emit({0x41,0xff,0xd3}); recovery.move(11,0);
  recovery.emit({0x4d,0x85,0xdb}); auto present = recovery.branch(0x75); recovery.emit({0x0f,0x0b}); recovery.bind(present);
  recovery.state_fx(true);
  for (unsigned n = 0; n < f.preserved_count; ++n) recovery.state(0x8b,unsigned(f.preserved[n]),unsigned(f.preserved[n])*8);
  recovery.emit({0x31,0xc0,0x31,0xd2,0x66,0x0f,0xef,0xc0,0x66,0x0f,0xef,0xc9});
  recovery.state(0x8b,10,1312); recovery.emit({0x45,0x85,0xd2}); auto no_x87 = recovery.branch(0x74);
  recovery.emit({0xd9,0xee,0x41,0x83,0xfa,0x01}); auto one_x87 = recovery.branch(0x74);
  recovery.emit({0xd9,0xee}); recovery.bind(no_x87); recovery.bind(one_x87);
  recovery.state(0x8b,4,768); recovery.emit({0xc3});
  return InvocationEntryCode{std::move(call.bytes),std::move(recovery.bytes)};
}
Result<std::vector<std::uint8_t>> detail::make_x86_native_boundary_gate(const target::TargetMachine &target,std::uintptr_t descriptor,std::uintptr_t enter) {
  if (target.architecture() != target::Architecture::X86_64 || !descriptor || !enter)
    return Error{ErrorCode::InvalidArgument,"invalid x86 native boundary descriptor or entry"};
  Code c; c.address(10,descriptor); c.address(11,enter); c.emit({0x41,0xff,0xe3}); c.bytes.resize(native_boundary_gate_bytes,0x90);
  return std::move(c.bytes);
}
Result<NativeBoundaryCode> detail::make_x86_native_boundary_entries(const target::TargetMachine &target,std::uintptr_t enter,std::uintptr_t leave) {
  if (target.architecture() != target::Architecture::X86_64 || !enter || !leave)
    return Error{ErrorCode::InvalidArgument,"invalid x86 native boundary callback"};
  auto layout = x86_boundary_frame(target.abi()); if (!layout) return layout.error(); auto &f = *layout;
  Code c;
  if (f.red_zone_bytes) c.memory(0x8d,4,std::size_t(0)-f.red_zone_bytes);
  c.emit({0x9c,0x49,0x89,0xe3,0x48,0x83,0xe4,0xf0}); // flags SP -> r11; align callback frame
  c.stack(false,f.frame_bytes);
  for (unsigned n = 0; n < 16; ++n) if (n != 4 && n != 11) c.memory(0x89,n,f.checkpoint_offset+n*8);
  c.state(0x8d,10,f.red_zone_bytes+8); c.memory(0x89,10,f.checkpoint_offset+768);
  c.emit({0x4d,0x8b,0x12}); c.memory(0x89,10,f.checkpoint_offset+776); // original return PC
  c.state(0x8b,10,0); c.memory(0x89,10,f.checkpoint_offset+248); // incoming flags, GP31 slot
  c.fx(false,f.checkpoint_offset+800);
  c.memory(0x8d,unsigned(f.first_parameter),f.checkpoint_offset);
  c.memory(0x8b,unsigned(f.second_parameter),f.checkpoint_offset+80); // descriptor saved from r10
  c.address(11,enter); c.emit({0x41,0xff,0xd3}); c.move(11,0);
  c.emit({0x4d,0x85,0xdb}); auto entered = c.branch(0x75); c.emit({0x0f,0x0b}); c.bind(entered);
  c.state_fx(true); c.state(0x8b,10,248); c.emit({0x41,0x52,0x9d});
  for (unsigned n = 0; n < 16; ++n) if (n != 4 && n != 10 && n != 11) c.state(0x8b,n,n*8);
  c.state(0x8b,4,768); c.state(0x8b,10,776); c.memory(0x89,10,0); // replace only this call's return PC
  c.state(0x8b,10,80); c.emit({0x41,0xff,0xe2}); // original argument stack, target r10
  auto outgoing = x86_block_resolver_frame(target.abi()); if (!outgoing) return outgoing.error(); auto &saved = outgoing->saved;
  Code out;
  if (outgoing->red_zone_bytes) out.memory(0x8d,4,std::size_t(0)-outgoing->red_zone_bytes);
  out.emit({0x9c,0x49,0x89,0xe3,0x48,0x83,0xe4,0xf0}); out.stack(false,saved.frame_bytes); out.memory(0x89,11,saved.metadata_offset);
  for (auto &reg : saved.saves) out.memory(0x89,unsigned(reg.reg),reg.offset);
  out.fx(false,saved.fx_offset); out.address(11,leave); out.emit({0x41,0xff,0xd3}); out.move(11,0);
  out.emit({0x4d,0x85,0xdb}); auto left = out.branch(0x75); out.emit({0x0f,0x0b}); out.bind(left);
  out.fx(true,saved.fx_offset);
  for (auto &reg : saved.saves) out.memory(0x8b,unsigned(reg.reg),reg.offset);
  out.memory(0x8b,4,saved.metadata_offset); out.emit({0x9d});
  if (outgoing->red_zone_bytes) out.memory(0x8d,4,outgoing->red_zone_bytes);
  out.emit({0x41,0xff,0xe3});
  return NativeBoundaryCode{std::move(c.bytes),std::move(out.bytes)};
}
}
