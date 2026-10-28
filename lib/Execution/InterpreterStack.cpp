/* Licensed under the MIT License. Rewindable interpreter stack and ABI va lists. */
#include "mirnext/Execution/detail/LoadedProgram.hpp"
#include "mirnext/Execution/Builtins.hpp"
#include <algorithm>
#include <new>
namespace mirnext::execution::detail {
namespace {
Error invalid (std::string reason) { return {ErrorCode::InvalidOperand, std::move (reason)}; }
}  // namespace
Result<std::uintptr_t> InterpreterStack::allocate (std::size_t size) {
  if (size > SIZE_MAX - 15) return invalid ("stack allocation size overflow");
  auto rounded = (size + 15) / 16 * 16;
  if (segments.empty () || segments.back ().cursor < rounded) {
    auto capacity = std::max (std::size_t{4096}, rounded);
    auto bytes = std::unique_ptr<std::byte[]> (new (std::nothrow) std::byte[capacity]);
    if (!bytes) return invalid ("unable to allocate interpreter stack");
    segments.push_back ({std::move (bytes), capacity, capacity});
  }
  auto &s = segments.back ();
  s.cursor -= rounded;
  return reinterpret_cast<std::uintptr_t> (s.data.get () + s.cursor);
}
Result<std::uintptr_t> InterpreterStack::save () {
  if (segments.empty ()) {
    auto space = allocate (0);
    if (!(space).has_value ()) return space.error ();
    segments.back ().cursor = segments.back ().size;
  }
  auto &s = segments.back ();
  auto address = reinterpret_cast<std::uintptr_t> (s.data.get () + s.cursor);
  marks.insert_or_assign (address, Mark{segments.size (), s.cursor});
  return address;
}
Result<void> InterpreterStack::restore (std::uintptr_t address) {
  auto found = marks.find (address);
  if (found == marks.end ())
    return invalid ("stack restore does not identify an active frame mark");
  auto mark = found->second;
  for (auto at = marks.begin (); at != marks.end ();) {
    auto &m = at->second;
    if (m.segments > mark.segments || (m.segments == mark.segments && m.cursor < mark.cursor))
      at = marks.erase (at);
    else
      ++at;
  }
  segments.resize (mark.segments);
  segments.back ().cursor = mark.cursor;
  return {};
}
Result<void> InterpreterFrame::start_variadics (std::uintptr_t destination) {
  auto &state = invocation.state;
  auto abi = state.target.abi ();
  auto size = variadic_state_size (abi);
  auto writable = state.writable (destination, size);
  if (!(writable).has_value ()) return writable;
  if (!body.source->signature ().variadic) return invalid ("va_start in non-variadic function");
  if (!native_va.empty ())
    return initialize_variadic_state (abi, {reinterpret_cast<std::byte *> (destination), size},
                                      native_va);
  // Place all unnamed values in a synthetic stack area. Exhausting the saved
  // register banks yields a normal native va_list for each maintained ABI.
  if (variadics.empty ()) {
    auto fixed = body.source->signature ().parameters.size ();
    std::vector<std::size_t> offsets;
    auto capacity = std::size_t{0};
    for (std::size_t n = fixed; n < arguments.size (); ++n) {
      auto type = arguments[n].type ();
      auto layout = type.is_block ()
                      ? Result<target::TypeLayout> (target::TypeLayout{type.block_size (), 8})
                      : state.target.layout (type);
      if (!(layout).has_value ()) return layout.error ();
      bool indirect = type.is_block () && abi != target::ABI::X86SysV
                      && type.block_size () > (abi == target::ABI::X86Windows ? 8u : 16u);
      auto alignment = type.kind () == ir::Type::Kind::LongDouble
                           && abi != target::ABI::AArch64MacOS && abi != target::ABI::X86Windows
                         ? layout.value ().alignment
                         : 8;
      if (capacity > SIZE_MAX - alignment) return invalid ("variadic area size overflow");
      capacity = (capacity + alignment - 1) / alignment * alignment;
      offsets.push_back (capacity);
      auto bytes = indirect ? 8 : type.is_block () ? type.block_size () : layout.value ().size;
      if (bytes > SIZE_MAX - 7 || capacity > SIZE_MAX - (bytes + 7) / 8 * 8)
        return invalid ("variadic area size overflow");
      capacity += (bytes + 7) / 8 * 8;
    }
    variadics.resize (
      std::max (std::size_t{16}, capacity));  // non-null even for no unnamed arguments
    for (std::size_t n = fixed; n < arguments.size (); ++n) {
      auto address = reinterpret_cast<std::uintptr_t> (variadics.data () + offsets[n - fixed]);
      auto &argument = arguments[n];
      if (auto block = argument.block ().get ()) {
        bool indirect = abi != target::ABI::X86SysV
                        && block->type.block_size () > (abi == target::ABI::X86Windows ? 8u : 16u);
        if (indirect) {
          auto value = reinterpret_cast<std::uintptr_t> (block->bytes.data ());
          std::memcpy (reinterpret_cast<std::byte *> (address), &value, 8);
        } else
          std::memcpy (reinterpret_cast<std::byte *> (address), block->bytes.data (),
                       block->bytes.size ());
      } else {
        auto stored = store_scalar (argument.type (), address, *argument.scalar ().get ());
        if (!(stored).has_value ()) return stored;
      }
    }
    invocation.variadic_areas.emplace_back (variadics.data (), capacity);
  }
  auto stack_address = reinterpret_cast<std::uintptr_t> (variadics.data ());
  std::array<std::byte, 32> initial{};
  if (abi == target::ABI::X86SysV) {
    struct List {
      std::uint32_t gp, fp;
      std::uintptr_t stack, registers;
    } list{48, 176, stack_address, 0};
    std::memcpy (initial.data (), &list, sizeof list);
  } else if (abi == target::ABI::AArch64Linux) {
    struct List {
      std::uintptr_t stack, gp, fp;
      std::int32_t go, fo;
    } list{stack_address, 0, 0, 0, 0};
    std::memcpy (initial.data (), &list, sizeof list);
  } else
    std::memcpy (initial.data (), &stack_address, 8);
  return initialize_variadic_state (abi, {reinterpret_cast<std::byte *> (destination), size},
                                    initial);
}
}  // namespace mirnext::execution::detail
