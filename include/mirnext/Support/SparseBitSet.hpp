/* Licensed under the MIT License. Sorted nonzero words keep small dataflow facts compact. */
#pragma once
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mirnext {
class SparseBitSet {
public:
  bool operator==(const SparseBitSet &) const = default;
  bool empty() const { return words_.empty(); }
  void clear() { words_.clear(); }
  void erase(std::size_t bit) {
    auto word = std::lower_bound(words_.begin(), words_.end(), bit / 64, [](auto &word, auto index) { return word.index < index; });
    if (word == words_.end() || word->index != bit / 64) return;
    word->bits &= ~(std::uint64_t{1} << (bit % 64)); if (!word->bits) words_.erase(word);
  }
  void intersect_with(const SparseBitSet &other) {
    if (this == &other) return;
    auto b = other.words_.begin(); std::size_t kept = 0;
    for (auto word : words_) {
      while (b != other.words_.end() && b->index < word.index) ++b;
      if (b != other.words_.end() && b->index == word.index && (word.bits &= b->bits)) words_[kept++] = word;
    }
    words_.resize(kept);
  }
  template<class Predicate> void erase_if(Predicate predicate) {
    std::size_t kept = 0;
    for (auto word : words_) {
      for (auto bits = word.bits; bits; bits &= bits - 1) {
        auto position = std::countr_zero(bits);
        if (predicate(word.index * 64 + position)) word.bits &= ~(std::uint64_t{1} << position);
      }
      if (word.bits) words_[kept++] = word;
    }
    words_.resize(kept);
  }
  bool contains(std::size_t bit) const {
    auto word = std::lower_bound(words_.begin(), words_.end(), bit / 64, [](auto &word, auto index) { return word.index < index; });
    return word != words_.end() && word->index == bit / 64 && (word->bits & (std::uint64_t{1} << (bit % 64)));
  }
  void insert(std::size_t bit) {
    auto word = std::lower_bound(words_.begin(), words_.end(), bit / 64, [](auto &word, auto index) { return word.index < index; });
    if (word == words_.end() || word->index != bit / 64) word = words_.insert(word, {bit / 64, 0});
    word->bits |= std::uint64_t{1} << (bit % 64);
  }
  bool union_with(const SparseBitSet &other) {
    if (this == &other || other.empty()) return false;
    if (empty()) { words_ = other.words_; return true; }
    auto existing = words_.cbegin(); bool gains = false;
    for (auto word : other.words_) {
      while (existing != words_.cend() && existing->index < word.index) ++existing;
      if (existing == words_.cend() || existing->index != word.index || (word.bits & ~existing->bits)) { gains = true; break; }
    }
    if (!gains) return false;
    std::vector<Word> merged; merged.reserve(words_.size() + other.words_.size());
    auto a = words_.cbegin(), b = other.words_.cbegin(); bool changed = false;
    while (a != words_.end() || b != other.words_.end()) {
      if (b == other.words_.end() || (a != words_.end() && a->index < b->index)) merged.push_back(*a++);
      else if (a == words_.end() || b->index < a->index) { merged.push_back(*b++); changed = true; }
      else { auto bits = a->bits | b->bits; changed |= bits != a->bits; merged.push_back({a->index, bits}); ++a; ++b; }
    }
    if (changed) words_.swap(merged);
    return changed;
  }
  void subtract(const SparseBitSet &other) {
    auto b = other.words_.begin(); std::size_t kept = 0;
    for (auto word : words_) {
      while (b != other.words_.end() && b->index < word.index) ++b;
      if (b != other.words_.end() && b->index == word.index) word.bits &= ~b->bits;
      if (word.bits) words_[kept++] = word;
    }
    words_.resize(kept);
  }
  template<class Visitor> void for_each(Visitor visitor) const {
    for (auto word : words_) for (auto bits = word.bits; bits; bits &= bits - 1)
      visitor(word.index * 64 + std::countr_zero(bits));
  }
  std::size_t word_count() const { return words_.size(); }
private:
  struct Word { std::size_t index; std::uint64_t bits; bool operator==(const Word &) const = default; };
  std::vector<Word> words_;
};
} // namespace mirnext
