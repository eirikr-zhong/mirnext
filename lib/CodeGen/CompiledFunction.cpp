/* Licensed under the MIT License. Checked byte and address fixups. */
#include "mirnext/CodeGen/Detail/CompiledFunction.hpp"
#include <algorithm>
#include <bit>
namespace mirnext::codegen {
Result<CompiledFunction> detail::ArtifactBuilder::finish (CompiledFunctionData data) {
  CompiledFunction result (std::move (data.module_), std::move (data.name_), data.architecture_,
                           std::move (data.frame_));
  result.bytes_ = std::move (data.bytes_);
  result.relocations_ = std::move (data.relocations_);
  result.constants_ = std::move (data.constants_);
  result.labels_.reserve (data.labels_.size ());
  for (auto &label : data.labels_)
    result.labels_.push_back ({CompiledLabel (result.labels_.size (), result.identity_),
                               std::move (label.name), label.offset, label.source_ordinal});
  auto valid = result.verify ();
  if (!valid) return valid.error ();
  return result;
}
namespace {
Error invalid (const CompiledFunction &f, std::string why) {
  return {ErrorCode::InvalidOperand, std::string (f.module_name ()) + '/' + std::string (f.name ())
                                       + " [relocate]: " + std::move (why)};
}
std::uint64_t read (std::span<const std::uint8_t> b, std::size_t at, unsigned size) {
  std::uint64_t v = 0;
  for (unsigned n = 0; n < size; ++n) v |= std::uint64_t (b[at + n]) << (8 * n);
  return v;
}
void write (std::span<std::uint8_t> b, std::size_t at, std::uint64_t v, unsigned size) {
  for (unsigned n = 0; n < size; ++n, v >>= 8) b[at + n] = v & 255;
}
}  // namespace
Result<void> CompiledFunction::verify () const {
  const bool arm = architecture_ == target::Architecture::AArch64;
  if (!arm && architecture_ != target::Architecture::X86_64)
    return invalid (*this, "unsupported code target");
  if (bytes_.empty () || bytes_.size () % 16) return invalid (*this, "empty or unaligned code");
  std::unordered_set<std::size_t> source_labels;
  std::unordered_set<std::string_view> names;
  for (std::size_t n = 0; n < labels_.size (); ++n) {
    auto &l = labels_[n];
    if (l.label.owner_ != identity_ || l.label.index_ != n || l.offset > bytes_.size ()
        || (arm && l.offset % 4))
      return invalid (*this, "invalid label offset or identity");
    if (l.source_ordinal && !source_labels.insert (*l.source_ordinal).second)
      return invalid (*this, "duplicate source label correspondence");
    if (!l.name.empty () && !names.insert (l.name).second)
      return invalid (*this, "duplicate compiled label name");
  }
  std::vector<std::pair<std::size_t, std::size_t>> occupied;
  for (auto &r : relocations_) {
    auto size = r.kind == CodeRelocation::Kind::AArch64WideMove ? 4u : 8u;
    if (r.offset > bytes_.size () || size > bytes_.size () - r.offset
        || ((arm || r.kind == CodeRelocation::Kind::LocalAddress) && r.offset % size))
      return invalid (*this, "relocation outside code or unaligned");
    occupied.emplace_back (r.offset, r.offset + size);
    if (r.kind == CodeRelocation::Kind::LocalAddress) {
      if (r.symbol || r.builtin || r.shift || r.displacement
          || read (bytes_, r.offset, 8) > bytes_.size ())
        return invalid (*this, "invalid local address relocation");
    } else if (r.kind == CodeRelocation::Kind::AArch64WideMove) {
      if (!arm || (bool (r.symbol) == bool (r.builtin)) || r.shift > 48 || r.shift % 16)
        return invalid (*this, "invalid symbol relocation");
      auto word = read (bytes_, r.offset, 4);
      auto code = word & 0xff800000;
      if ((code != 0xd2800000 && code != 0xf2800000) || ((word >> 21) & 3) != r.shift / 16)
        return invalid (*this, "invalid wide-move opcode");
    } else if (r.kind == CodeRelocation::Kind::AbsoluteSymbol) {
      if (arm || (bool (r.symbol) == bool (r.builtin)) || r.shift || read (bytes_, r.offset, 8))
        return invalid (*this, "invalid absolute symbol relocation");
    } else
      return invalid (*this, "unknown relocation kind");
  }
  std::sort (occupied.begin (), occupied.end ());
  for (std::size_t n = 1; n < occupied.size (); ++n)
    if (occupied[n].first < occupied[n - 1].second)
      return invalid (*this, "overlapping code relocations");
  if (arm && !constants_.empty ()) return invalid (*this, "x86 constant pool on aarch64");
  for (std::size_t n = 0; n < constants_.size (); ++n) {
    auto &c = constants_[n];
    const auto start = constants_[0].offset;
    if (c.offset % 16 || c.offset > bytes_.size () || bytes_.size () - c.offset < 16
        || c.offset != start + n * 16 || c.reference_offset > start
        || start - c.reference_offset < 4 || c.next_instruction > start
        || c.reference_offset + 4 > c.next_instruction || c.next_instruction > c.offset
        || c.offset - c.next_instruction > INT32_MAX
        || read (bytes_, c.reference_offset, 4) != c.offset - c.next_instruction
        || read (bytes_, c.offset + 8, 8))
      return invalid (*this, "invalid constant pool entry");
    if (c.call
        && (c.reference_offset < 2 || c.next_instruction != c.reference_offset + 4
            || bytes_[c.reference_offset - 2] != 0xff
            || (bytes_[c.reference_offset - 1] != 0x15 && bytes_[c.reference_offset - 1] != 0x25)))
      return invalid (*this, "invalid pool call instruction");
    auto overlap
      = std::lower_bound (occupied.begin (), occupied.end (), c.reference_offset,
                          [] (auto &range, std::size_t offset) { return range.second <= offset; });
    if (overlap != occupied.end () && overlap->first < c.reference_offset + 4)
      return invalid (*this, "relocation overlaps constant pool displacement");
  }
  if (!constants_.empty ()) {
    if (constants_.back ().offset + 16 != bytes_.size ())
      return invalid (*this, "incomplete constant pool");
    for (auto &l : labels_)
      if (l.offset > constants_[0].offset) return invalid (*this, "label inside constant pool");
    for (auto &r : relocations_) {
      if (r.offset < constants_[0].offset) {
        if (constants_[0].offset - r.offset < 8)
          return invalid (*this, "relocation crosses constant pool boundary");
      } else if (r.kind != CodeRelocation::Kind::AbsoluteSymbol
                 || (r.offset - constants_[0].offset) % 16)
        return invalid (*this, "relocation inside constant pool padding");
    }
  }
  return {};
}
Result<std::vector<std::uint8_t>> CompiledFunction::relocate (
  std::uint64_t base, const CodeSymbolResolver &resolve) const {
  auto valid = verify ();
  if (!valid) return valid.error ();
  if ((architecture_ == target::Architecture::AArch64 && base % 4)
      || bytes_.size () > UINT64_MAX - base)
    return invalid (*this, "unaligned or overflowing code base");
  auto bytes = bytes_;
  struct Resolved {
    const CodeRelocation *ref;
    std::uint64_t value;
  };
  std::vector<Resolved> addresses;
  for (auto &r : relocations_) {
    if (r.kind == CodeRelocation::Kind::LocalAddress) {
      write (bytes, r.offset, base + read (bytes_, r.offset, 8), 8);
      continue;
    }
    auto same = [&] (const Resolved &v) {
      return v.ref->symbol == r.symbol && v.ref->builtin == r.builtin;
    };
    auto found = std::find_if (addresses.begin (), addresses.end (), same);
    std::uint64_t address;
    if (found != addresses.end ())
      address = found->value;
    else {
      if (!resolve) return invalid (*this, "unresolved code symbol");
      auto value = resolve ({r.symbol ? &*r.symbol : nullptr, r.builtin});
      if (!value) {
        auto e = invalid (*this, value.error ().message);
        e.code = value.error ().code;
        return e;
      }
      address = *value;
      addresses.push_back ({&r, address});
    }
    address += std::bit_cast<std::uint64_t> (r.displacement);
    if (r.kind == CodeRelocation::Kind::AbsoluteSymbol) {
      write (bytes, r.offset, address, 8);
      continue;
    }
    auto word = read (bytes, r.offset, 4);
    word = (word & ~(std::uint64_t{65535} << 5)) | (((address >> r.shift) & 65535) << 5);
    write (bytes, r.offset, word, 4);
  }
  for (auto &c : constants_)
    if (c.call) {
      const auto difference = read (bytes, c.offset, 8) - (base + c.next_instruction);
      if (difference <= INT32_MAX || difference >= UINT64_MAX - std::uint64_t (INT32_MAX)) {
        bytes[c.reference_offset - 2] = 0x40;
        bytes[c.reference_offset - 1] = bytes_[c.reference_offset - 1] == 0x25 ? 0xe9 : 0xe8;
        write (bytes, c.reference_offset, difference, 4);
      }
    }
  return bytes;
}
Result<CompiledLabel> CompiledFunction::find_label (std::string_view name) const {
  if (name.empty ()) return invalid (*this, "empty label name");
  for (auto &l : labels_)
    if (l.name == name) return l.label;
  return invalid (*this, "unknown label: " + std::string (name));
}
Result<std::vector<std::uint64_t>> CompiledFunction::label_addresses (
  std::uint64_t base, std::span<const LabelAddress> refs) const {
  auto valid = verify ();
  if (!valid) return valid.error ();
  std::vector<std::uint64_t> values;
  values.reserve (refs.size ());
  auto offset = [&] (const CompiledLabel &label) -> Result<std::size_t> {
    if (label.owner_ != identity_ || label.index_ >= labels_.size ())
      return invalid (*this, "foreign compiled label");
    return labels_[label.index_].offset;
  };
  for (auto &r : refs) {
    auto label = offset (r.label);
    if (!label) return label.error ();
    auto value = *label + std::bit_cast<std::uint64_t> (r.displacement);
    if (r.relative_to) {
      auto other = offset (*r.relative_to);
      if (!other) return other.error ();
      value -= *other;
    } else
      value += base;
    values.push_back (value);
  }
  return values;
}
}  // namespace mirnext::codegen
