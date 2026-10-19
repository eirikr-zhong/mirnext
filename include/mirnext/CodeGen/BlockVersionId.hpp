/* Licensed under the MIT License. Independently owned block-version identity. */
#pragma once
#include <memory>
#include <cstddef>
#include <utility>
namespace mirnext::codegen {
namespace detail { struct BBVState; }
// Copies keep only the identity alive, never the machine body or runtime state.
// An ordinal is diagnostic metadata, not a way to manufacture a reference.
class BlockVersionId {
public:
  std::size_t index() const { return index_; }
  friend bool operator==(const BlockVersionId &,const BlockVersionId &) = default;
  bool same_program(const BlockVersionId &other) const { return owner_ == other.owner_; }
private:
  friend struct detail::BBVState;
  BlockVersionId(std::shared_ptr<const char> owner,std::size_t index) : owner_(std::move(owner)),index_(index) {}
  std::shared_ptr<const char> owner_;
  std::size_t index_;
};
}
