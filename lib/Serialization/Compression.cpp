/* Licensed under the MIT License. MIR compression algorithm derived from MIR,
   Copyright 2018-2024 Vladimir Makarov. */
#include "mirnext/Serialization/Compression.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace mirnext::serialization {
namespace {
constexpr std::uint32_t block_size = 1u << 18, table_size = block_size / 4;
Error invalid (std::string message) {
  return {ErrorCode::InvalidOperand, "binary-compression: " + std::move (message)};
}
std::uint64_t multiply (std::uint64_t value, std::uint64_t constant) {
  auto middle = std::uint64_t (std::uint32_t (value)) * (constant >> 32)
                + (value >> 32) * std::uint32_t (constant);
  return (value >> 32) * (constant >> 32) + (middle >> 32)
         + std::uint64_t (std::uint32_t (value)) * std::uint32_t (constant) + (middle << 32);
}
std::uint64_t hash (std::span<const std::byte> bytes, std::uint64_t seed) {
  constexpr std::uint64_t p1 = 0x65862b62bdf5ef4dull, p2 = 0x288eea216831e6a7ull;
  auto part = [] (std::span<const std::byte> s) {
    std::uint64_t value = 0;
    for (auto b : s) value = (value >> 8) | (std::uint64_t (std::to_integer<unsigned> (b)) << 56);
    return value;
  };
  auto result = seed + bytes.size ();
  while (bytes.size () >= 16) {
    result ^= multiply (part (bytes.first (8)), p1);
    result ^= multiply (part (bytes.subspan (8, 8)), p2);
    result ^= multiply (result, p1);
    bytes = bytes.subspan (16);
  }
  if (bytes.size () >= 8) {
    result ^= multiply (part (bytes.first (8)), p1);
    bytes = bytes.subspan (8);
  }
  if (!bytes.empty ()) result ^= multiply (part (bytes), p2);
  result ^= multiply (result, p1);
  return result ^ multiply (result, p2);
}
void byte (std::vector<std::byte> &out, unsigned value) {
  out.push_back (static_cast<std::byte> (value));
}
void integer (std::vector<std::byte> &out, std::uint32_t value) {
  unsigned count = 1;
  while (count <= 4 && value >= (1u << (7 * count))) ++count;
  byte (out, (1u << (8 - count)) | ((value >> ((count - 1) * 8)) & 255));
  for (unsigned n = 2; n <= count; ++n) byte (out, value >> ((count - n) * 8));
}
unsigned integer_size (std::uint32_t value) {
  return value < (1u << 7) ? 1 : value < (1u << 14) ? 2 : value < (1u << 21) ? 3 : 4;
}
struct Entry {
  std::uint32_t position = 0, number = 0, next = UINT32_MAX, head = UINT32_MAX;
};
class Compressor {
 public:
  std::vector<std::byte> run (std::span<const std::byte> bytes) {
    output_ = {std::byte{'M'}, std::byte{'I'}, std::byte{'R'}};
    std::uint64_t checksum = 42;
    while (!bytes.empty ()) {
      auto block = bytes.first (std::min<std::size_t> (block_size, bytes.size ()));
      checksum = hash (block, checksum);
      encode (block);
      bytes = bytes.subspan (block.size ());
    }
    byte (output_, 0);
    for (unsigned n = 0; n < 8; ++n) byte (output_, checksum >> (n * 8));
    return std::move (output_);
  }

 private:
  std::vector<Entry> table_ = std::vector<Entry> (table_size);
  std::vector<std::byte> output_, literal_;
  std::uint32_t free_ = 0, number_ = 0;
  void flush (unsigned reference) {
    auto length = static_cast<unsigned> (literal_.size ());
    if (!length && !reference) return;
    byte (output_, (std::min (length, 7u) << 5) | reference);
    if (length >= 7) integer (output_, length);
    output_.insert (output_.end (), literal_.begin (), literal_.end ());
    literal_.clear ();
  }
  void insert (std::span<const std::byte> block, std::uint32_t position) {
    auto number = number_++;
    if (position + 4 > block.size ()) return;
    auto key = hash (block.subspan (position, 4), 24) % table_size;
    auto current = free_;
    if (current != UINT32_MAX)
      free_ = table_[current].next;
    else {
      std::uint32_t previous = UINT32_MAX;
      current = table_[key].head;
      while (current != UINT32_MAX && table_[current].next != UINT32_MAX) {
        previous = current;
        current = table_[current].next;
      }
      if (current == UINT32_MAX) return;
      if (previous != UINT32_MAX)
        table_[previous].next = table_[current].next;
      else
        table_[key].head = table_[current].next;
    }
    table_[current].position = position;
    table_[current].number = number;
    table_[current].next = table_[key].head;
    table_[key].head = current;
  }
  void encode (std::span<const std::byte> block) {
    for (std::uint32_t n = 0; n < table_size; ++n) {
      table_[n].next = n + 1;
      table_[n].head = UINT32_MAX;
    }
    table_.back ().next = UINT32_MAX;
    free_ = number_ = 0;
    for (std::uint32_t position = 0; position < block.size ();) {
      std::uint32_t best_length = 0, best_size = 0, best_number = 0;
      if (position + 4 <= block.size ()) {
        auto key = hash (block.subspan (position, 4), 24) % table_size;
        for (auto current = table_[key].head; current != UINT32_MAX;
             current = table_[current].next) {
          auto &entry = table_[current];
          auto bound = std::min<std::size_t> (block.size () - position, position - entry.position);
          unsigned length = 0;
          while (length < bound && block[entry.position + length] == block[position + length])
            ++length;
          if (length < 4) continue;
          auto offset = number_ - entry.number,
               size = integer_size (offset) + (length - 3 < 31 ? 0 : integer_size (length - 3));
          if (!best_length || best_length + size < length + best_size) {
            best_length = length;
            best_size = size;
            best_number = entry.number;
          }
        }
      }
      if (!best_length) {
        if (literal_.size () == 2047) flush (0);
        literal_.push_back (block[position]);
        insert (block, position++);
      } else {
        auto length = best_length - 3;
        flush (std::min (length, 31u));
        if (length >= 31) integer (output_, length);
        integer (output_, number_ - best_number);
        insert (block, position);
        position += best_length;
      }
    }
    flush (0);
  }
};
class Decompressor {
 public:
  Decompressor (std::span<const std::byte> input, std::size_t maximum)
    : input_ (input), maximum_ (maximum) {}
  Result<std::vector<std::byte>> run () {
    constexpr std::array prefix{std::byte{'M'}, std::byte{'I'}, std::byte{'R'}};
    if (input_.size () < prefix.size ()
        || !std::equal (prefix.begin (), prefix.end (), input_.begin ()))
      return invalid ("invalid prefix");
    input_ = input_.subspan (3);
    std::uint64_t checksum = 42;
    std::size_t produced = 0, count = 0;
    for (;;) {
      auto tag = get ();
      if (!(tag).has_value ()) return tag.error ();
      if (!(tag).value ()) {
        if (input_.size () != 8) return invalid ("missing checksum or trailing bytes");
        if (produced) checksum = hash (std::span (block_).first (produced), checksum);
        std::uint64_t expected = 0;
        for (unsigned n = 0; n < 8; ++n)
          expected |= std::uint64_t (std::to_integer<unsigned> (input_[n])) << (n * 8);
        if (expected != checksum) return invalid ("checksum mismatch");
        auto appended = append (produced);
        if (!(appended).has_value ()) return appended.error ();
        return std::move (output_);
      }
      unsigned literals = (tag).value () >> 5;
      if (literals == 7) {
        auto value = number ();
        if (!(value).has_value ()) return value.error ();
        literals = (value).value ();
      }
      if (literals > 2047 || literals > block_size - produced || literals > input_.size ())
        return invalid ("invalid literal length");
      if (literals) {
        std::memcpy (block_.data () + produced, input_.data (), literals);
        input_ = input_.subspan (literals);
        for (unsigned n = 0; n < literals; ++n) indices_[count++] = produced++;
      }
      unsigned length = (tag).value () & 31;
      if (length) {
        if (length == 31) {
          auto value = number ();
          if (!(value).has_value ()) return value.error ();
          length = (value).value ();
        }
        if (!length || length > block_size - produced || length + 3 > block_size - produced)
          return invalid ("invalid reference length");
        length += 3;
        auto offset = number ();
        if (!(offset).has_value ()) return offset.error ();
        if (!(offset).value () || (offset).value () > count)
          return invalid ("invalid dictionary reference");
        auto source = indices_[count - (offset).value ()];
        if (source > produced || length > produced - source)
          return invalid ("overlapping or uninitialized reference");
        std::memcpy (block_.data () + produced, block_.data () + source, length);
        indices_[count++] = produced;
        produced += length;
      }
      if (produced == block_size) {
        checksum = hash (block_, checksum);
        auto appended = append (produced);
        if (!(appended).has_value ()) return appended.error ();
        produced = count = 0;
      }
    }
  }

 private:
  std::span<const std::byte> input_;
  std::size_t maximum_;
  std::vector<std::byte> output_, block_ = std::vector<std::byte> (block_size);
  std::vector<std::uint32_t> indices_ = std::vector<std::uint32_t> (block_size);
  Result<unsigned> get () {
    if (input_.empty ()) return invalid ("truncated input");
    auto result = std::to_integer<unsigned> (input_[0]);
    input_ = input_.subspan (1);
    return result;
  }
  Result<std::uint32_t> number () {
    auto first = get ();
    if (!(first).has_value ()) return first.error ();
    unsigned count = 1, value = (first).value ();
    while (count <= 4 && (value >> (8 - count)) != 1) ++count;
    if (count > 4) return invalid ("invalid integer tag");
    value &= 255u >> count;
    for (unsigned n = 1; n < count; ++n) {
      auto next = get ();
      if (!(next).has_value ()) return next.error ();
      value = value * 256 + (next).value ();
    }
    return value;
  }
  Result<void> append (std::size_t size) {
    if (size > maximum_ - output_.size ()) return invalid ("decompressed byte limit exceeded");
    output_.insert (output_.end (), block_.begin (), block_.begin () + size);
    return {};
  }
};
}  // namespace
Result<std::vector<std::byte>> compress (std::span<const std::byte> input) {
  return Compressor ().run (input);
}
Result<std::vector<std::byte>> decompress (std::span<const std::byte> input, std::size_t maximum) {
  return Decompressor (input, maximum).run ();
}
}  // namespace mirnext::serialization
