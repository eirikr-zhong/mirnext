/* Licensed under the MIT License. Checked, repeatable BBV relocation. */
#include "mirnext/CodeGen/Detail/CompiledBlock.hpp"
#include "mirnext/CodeGen/Detail/AArch64Patterns.hpp"
#include <algorithm>
#include <bit>
namespace mirnext::codegen {
namespace {
Error invalid(const CompiledBlock &block,std::string why) {
  return {ErrorCode::InvalidOperand,std::string(block.module_name())+'/'+std::string(block.name())
    +" [bbv-relocate #"+std::to_string(block.version().index())+"]: "+std::move(why)};
}
std::uint64_t read(std::span<const std::uint8_t> b,std::size_t at,unsigned size) {
  std::uint64_t v = 0; for (unsigned n = 0; n < size; ++n) v |= std::uint64_t(b[at+n]) << (8*n); return v;
}
void write(std::span<std::uint8_t> b,std::size_t at,std::uint64_t v,unsigned size) {
  for (unsigned n = 0; n < size; ++n,v >>= 8) b[at+n] = v & 255;
}
// Unsigned subtraction avoids overflowing signed host arithmetic near either
// end of the address space. The result is representable in the requested range.
Result<std::int64_t> difference(std::uint64_t target,std::uint64_t origin,std::int64_t low,std::int64_t high) {
  if (target >= origin) { auto d = target-origin; if (d <= std::uint64_t(high)) return static_cast<std::int64_t>(d); }
  else { auto d = origin-target; if (d <= std::uint64_t(-(low+1))+1) return -static_cast<std::int64_t>(d); }
  return Error{ErrorCode::InvalidOperand,"successor displacement out of range"};
}
}
Result<void> CompiledBlock::verify() const {
  auto valid = code_.verify(); if (!valid) return valid;
  const bool arm = code_.architecture() == target::Architecture::AArch64;
  std::vector<std::pair<std::size_t,std::size_t>> occupied;
  for (auto &r : code_.relocations()) occupied.emplace_back(r.offset,r.offset+(r.kind == CodeRelocation::Kind::AArch64WideMove ? 4 : 8));
  for (auto &c : code_.constant_pool()) occupied.emplace_back(c.reference_offset,c.reference_offset+4);
  for (auto &r : successors_) {
    using K = BlockRelocation::Kind; const auto size = r.kind == K::AbsoluteAddress ? 8u : 4u;
    if (!version_.same_program(r.target)) return invalid(*this,"foreign successor identity");
    if (r.offset > code_.bytes().size() || code_.bytes().size()-r.offset < size || (arm && r.offset%4)) return invalid(*this,"invalid successor offset");
    if (r.kind == K::AbsoluteAddress) {
      if (r.offset%8 || r.next_instruction || read(code_.bytes(),r.offset,8)) return invalid(*this,"invalid absolute successor");
    } else if (r.kind == K::X86Relative32) {
      if (arm || r.next_instruction < r.offset+4 || r.next_instruction > code_.bytes().size() || read(code_.bytes(),r.offset,4)) return invalid(*this,"invalid x86 successor");
    } else {
      if (!arm || r.next_instruction) return invalid(*this,"invalid aarch64 successor");
      const auto word = read(code_.bytes(),r.offset,4);
      if (r.kind == K::AArch64Branch26) {
        if ((word & 0x7c000000) != 0x14000000 || (word & 0x3ffffff)) return invalid(*this,"invalid branch26 opcode");
      } else if (r.kind == K::AArch64Address21) {
        if ((word & 0x9f000000) != 0x10000000 || (word & 0x60ffffe0)) return invalid(*this,"invalid adr opcode");
      } else if (r.kind == K::AArch64Branch19) {
        if (((word & 0xff000010) != 0x54000000 && (word & 0x7e000000) != 0x34000000) || (word & 0xffffe0)) return invalid(*this,"invalid branch19 opcode");
        // The far conditional expansion replaces the NOP two words later.
        if (code_.bytes().size()-r.offset < 12 || read(code_.bytes(),r.offset+8,4) != 0xd503201f) return invalid(*this,"missing conditional expansion slot");
        occupied.emplace_back(r.offset+8,r.offset+12);
      } else return invalid(*this,"unknown successor relocation");
    }
    if (!code_.constant_pool().empty() && r.offset >= code_.constant_pool().front().offset) return invalid(*this,"successor inside constant pool");
    occupied.emplace_back(r.offset,r.offset+size);
  }
  std::sort(occupied.begin(),occupied.end());
  for (std::size_t n = 1; n < occupied.size(); ++n) if (occupied[n].first < occupied[n-1].second) return invalid(*this,"overlapping successor relocation");
  return {};
}
Result<std::vector<std::uint8_t>> CompiledBlock::relocate(std::uint64_t base,const BlockAddressResolver &resolve,const CodeSymbolResolver &symbols) const {
  auto valid = verify(); if (!valid) return valid.error();
  auto bytes = code_.relocate(base,symbols); if (!bytes) return bytes.error();
  // All successors have already been checked against this program identity.
  // Cache by its typed ordinal so large switches remain linear rather than
  // repeatedly scanning the already resolved targets.
  std::unordered_map<std::size_t,std::uint64_t> addresses;
  for (auto &r : successors_) {
    if (!resolve) return invalid(*this,"unresolved block successor");
    auto cached = addresses.find(r.target.index());
    std::uint64_t address;
    if (cached != addresses.end()) address = cached->second;
    else { auto value = resolve(r.target); if (!value) { auto e = invalid(*this,value.error().message); e.code = value.error().code; return e; } address = *value; addresses.emplace(r.target.index(),address); }
    using K = BlockRelocation::Kind;
    if (code_.architecture() == target::Architecture::AArch64 && address%4) return invalid(*this,"unaligned block successor");
    if (r.kind == K::AbsoluteAddress) { write(*bytes,r.offset,address,8); continue; }
    auto origin = base+(r.kind == K::X86Relative32 ? r.next_instruction : r.offset);
    auto range = r.kind == K::AArch64Address21 ? 20u : r.kind == K::X86Relative32 ? 31u : 27u;
    auto delta = difference(address,origin,-(std::int64_t{1} << range),(std::int64_t{1} << range)-1);
    if (!delta) return invalid(*this,delta.error().message);
    if (r.kind == K::X86Relative32) { write(*bytes,r.offset,static_cast<std::uint64_t>(*delta),4); continue; }
    auto word = read(*bytes,r.offset,4);
    if (r.kind == K::AArch64Address21) {
      auto value = static_cast<std::uint32_t>(*delta)&0x1fffff;
      write(*bytes,r.offset,word | ((value&3)<<29) | ((value>>2)<<5),4); continue;
    }
    if (r.kind == K::AArch64Branch19 && (*delta < -(1<<20) || *delta >= (1<<20))) {
      // Same layout as MIR: conditional skips the fallthrough jump and lands
      // on the reserved NOP, replaced by a full-range branch to its successor.
      auto branch = aarch64_branch_displacement(*delta-8,false); if (!branch) return invalid(*this,branch.error().message);
      write(*bytes,r.offset,word | (2<<5),4); write(*bytes,r.offset+8,0x14000000 | *branch,4);
    } else {
      auto bits = aarch64_branch_displacement(*delta,r.kind == K::AArch64Branch19); if (!bits) return invalid(*this,bits.error().message);
      write(*bytes,r.offset,word | *bits,4);
    }
  }
  return bytes;
}
Result<CompiledBlock> encode_bbv(BBVProgram &program,const BBVVersion &version,MachineEncodingOptions options) {
  auto plan = program.specialize(version); if (!plan) return plan.error();
  // Preparation owns an immutable, already verified body. Check its phase in
  // constant time instead of rewalking an entire function for each tiny block.
  const auto &prepared = program.preparation().prepared();
  if (!prepared.frame().current(prepared.function()) || !prepared.function().state().frame_formed)
    return Error{ErrorCode::InvalidOperand,"bbv-encode: stale machine preparation"};
  switch (program.preparation().function().target().architecture()) {
  case target::Architecture::AArch64: return detail::encode_aarch64_block(program.preparation(),version,std::move(options));
  case target::Architecture::X86_64: return detail::encode_x86_block(program.preparation(),version,std::move(options));
  default: return Error{ErrorCode::InvalidOperand,"bbv-encode: unsupported target"};
  }
}
}
