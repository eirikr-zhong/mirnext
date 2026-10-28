/* Licensed under the MIT License. Shared symbol and data image for execution engines. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/Execution/detail/ProgramImage.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <new>
namespace mirnext::execution {
namespace {
Error invalid (std::string message) { return {ErrorCode::InvalidOperand, std::move (message)}; }
Result<std::size_t> data_size (const ir::GlobalData &data, const target::TargetMachine &target) {
  auto &value = data.initializer ();
  if (auto zero = std::get_if<ir::ZeroData> (&value)) return zero->size;
  ir::Type type = ir::Type::pointer ();
  std::size_t count = 1;
  if (auto initial = std::get_if<ir::InitializedData> (&value)) {
    type = initial->type;
    count = initial->values.size ();
  } else if (auto expression = std::get_if<ir::ExpressionData> (&value))
    type = ir::detail::NodeAccess::function (*(expression))->signature ().results[0];
  auto layout = target.layout (type);
  if (!(layout).has_value ()) return layout.error ();
  if (count > SIZE_MAX / layout.value ().size) return invalid ("data element size overflow");
  return count * layout.value ().size;
}
template <class T>
void store (std::byte *destination, const T &value) {
  std::memcpy (destination, &value, sizeof value);
}
void store_integer (std::byte *destination, ir::Type type, std::uint64_t value) {
  using K = ir::Type::Kind;
  switch (type.kind ()) {
  case K::I8:
  case K::U8: store (destination, static_cast<std::uint8_t> (value)); break;
  case K::I16:
  case K::U16: store (destination, static_cast<std::uint16_t> (value)); break;
  case K::I32:
  case K::U32: store (destination, static_cast<std::uint32_t> (value)); break;
  default: store (destination, value); break;
  }
}
bool jump_return (const ir::Function &f) {
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == ir::Opcode::JRet) return true;
  return false;
}
bool compatible_type (ir::Type a, ir::Type b) {
  return a == b
         || ((a.kind () == ir::Type::Kind::Bool || a.kind () == ir::Type::Kind::Pointer)
             && (b.kind () == ir::Type::Kind::I64 || b.kind () == ir::Type::Kind::U64));
}
}  // namespace
Result<std::vector<DataSectionLayout>> layout_module_data (const ir::Module &module,
                                                           const target::TargetMachine &target) {
  auto checked = module.verify ();
  if (!(checked).has_value ()) return checked.error ();
  std::vector<DataSectionLayout> result;
  bool active = false;
  for (auto &item : ir::detail::NodeAccess::items (module)) {
    auto entry = std::get_if<const ir::GlobalData *> (&item);
    if (!entry) {
      active = false;
      continue;
    }
    auto &data = **entry;
    auto size = data_size (data, target);
    if (!(size).has_value ()) return size.error ();
    if (!active || !data.symbol ().name ().empty ()) {
      result.emplace_back ();
      active = true;
    }
    auto &section = result.back ();
    if ((size).value () > SIZE_MAX - section.size) return invalid ("data section size overflow");
    section.items.push_back ({data, section.size, (size).value ()});
    section.size += (size).value ();
  }
  for (auto &section : result) {
    if (section.size > SIZE_MAX - 7) return invalid ("data section alignment overflow");
    section.size = (section.size + 7) / 8 * 8;
  }
  return result;
}
namespace detail {
Error ProgramImage::error (const ir::Symbol *symbol, std::string_view phase, Error e) const {
  e.message
    = (symbol ? std::string (symbol->module ().name ()) + '/' + std::string (symbol->name ())
              : std::string ("program"))
      + " [" + std::string (target.name ()) + "/link:" + std::string (phase) + "]: " + e.message;
  return e;
}
Result<std::size_t> ProgramImage::find (std::string_view module, std::string_view name) const {
  auto m = context.find_module (module).get ();
  auto s = m ? m->find_symbol (name).get () : nullptr;
  if (!s)
    return Error{ErrorCode::UnknownName,
                 std::string (module) + '/' + std::string (name) + ": unknown linked symbol"};
  return identities.at (s);
}
Result<void> ProgramImage::check_import (std::size_t index) const {
  auto &import = *bindings[index].source;
  auto &definition = binding (index);
  if (import.signature ().get ()
      && (definition.kind != Kind::Function || !definition.signature
          || *definition.signature != *import.signature ().get ()))
    return error (&import, "symbols", invalid ("import function signature mismatch"));
  if (auto variable = import.variable ().get ()) {
    if (definition.kind != Kind::Data || (variable->writable () && !definition.writable))
      return error (&import, "symbols", invalid ("import data kind or write permission mismatch"));
    auto layout = target.layout (variable->type ());
    if (!(layout).has_value ()) return layout.error ();
    if (definition.size < layout.value ().size)
      return error (&import, "symbols", invalid ("imported variable exceeds external storage"));
  }
  return {};
}
Result<void> ProgramImage::symbols (const LinkOptions &options, bool native) {
  for (auto &module : context.modules ())
    for (auto &symbol : module.symbols ()) {
      auto index = bindings.size ();
      identities.emplace (&symbol, index);
      Binding entry{&symbol};
      if (symbol.signature ().get ()) entry.signature = *symbol.signature ().get ();
      if (symbol.function ().get ()) {
        entry.kind = Kind::Function;
        entry.jump = jump_return (*symbol.function ().get ());
      }
      if (symbol.data ().get ()) {
        entry.kind = Kind::Data;
        entry.writable = symbol.data ().get ()->writable ();
        auto size = data_size (*symbol.data ().get (), target);
        if (!(size).has_value ()) return error (&symbol, "layout", size.error ());
        entry.size = (size).value ();
      }
      bindings.push_back (std::move (entry));
      if (symbol.exported ()) exports.emplace (symbol.name (), index);
    }
  std::unordered_map<std::string, ExternalSymbol> externals;
  for (std::size_t n = 0; n < bindings.size (); ++n) {
    auto &entry = bindings[n];
    auto &symbol = *entry.source;
    if (symbol.kind () != ir::Symbol::Kind::Import) continue;
    if (auto exported = exports.find (symbol.name ()); exported != exports.end ())
      entry.alias = exported->second;
    else {
      auto found = externals.find (std::string (symbol.name ()));
      if (found == externals.end ()) {
        if (!options.resolve)
          return error (&symbol, "symbols", {ErrorCode::UnknownName, "unresolved import"});
        auto resolved = options.resolve (
          {symbol.module ().name (), symbol.name (), symbol.signature (), symbol.variable ()});
        if (!(resolved).has_value ()) return error (&symbol, "symbols", resolved.error ());
        ++statistics.external_resolutions;
        if ((!resolved.value ().address
             && (native || !resolved.value ().interpret || !resolved.value ().signature))
            || bool (resolved.value ().signature) == bool (resolved.value ().data))
          return error (&symbol, "symbols",
                        invalid (
                          "external binding requires an execution entry and exactly one kind"));
        if (resolved.value ().signature) {
          auto valid = resolved.value ().signature->verify ();
          if (!(valid).has_value ()) return error (&symbol, "symbols", valid.error ());
        }
        if (resolved.value ().data
            && (resolved.value ().jump_return || resolved.value ().interpret
                || (resolved.value ().data->scalar_type
                    && (!resolved.value ().data->scalar_type->valid ()
                        || !resolved.value ().data->scalar_type->is_scalar ()))
                || resolved.value ().data->size > UINTPTR_MAX - resolved.value ().address))
          return error (&symbol, "symbols", invalid ("invalid external data binding"));
        found
          = externals.emplace (std::string (symbol.name ()), std::move ((resolved).value ())).first;
      }
      auto &external = found->second;
      entry.address = external.address;
      entry.signature = external.signature;
      entry.jump = external.jump_return;
      entry.interpret = external.interpret;
      entry.kind = external.signature ? Kind::Function : Kind::Data;
      if (external.data) {
        entry.size = external.data->size;
        entry.writable = external.data->writable;
        if (symbol.variable ().get () && external.data->scalar_type
            && !compatible_type (symbol.variable ().get ()->type (), *external.data->scalar_type))
          return error (&symbol, "symbols", invalid ("external variable type mismatch"));
      }
    }
    auto checked = check_import (n);
    if (!(checked).has_value ()) return checked;
  }
  // Untyped imports still have a call-site prototype. Validate the resolved
  // definition and special calling convention before any native code exists.
  for (auto &module : context.modules ())
    for (auto &f : module.functions ())
      for (auto &b : f.blocks ())
        for (auto &i : b.instructions ()) {
          if (ir::instruction_descriptor (i.opcode ()).get ()->count != ir::OperandCount::Call)
            continue;
          auto ref = ir::detail::operand_access (i.operands ()[1]).get<ir::SymbolReference> ();
          if (!ref) continue;
          auto &callee = binding (identities.at (ir::detail::NodeAccess::symbol (*(ref))));
          auto prototype
            = ir::detail::operand_access (i.operands ()[0]).get<ir::SymbolReference> ();
          if (!prototype || !ir::detail::NodeAccess::symbol (*(prototype))->signature ().get ()
              || callee.kind != Kind::Function || !callee.signature
              || *callee.signature
                   != *ir::detail::NodeAccess::symbol (*(prototype))->signature ().get ()
              || callee.jump != (i.opcode () == ir::Opcode::JCall))
            return error (&f.symbol (), "symbols",
                          invalid ("resolved call signature, symbol kind or convention mismatch"));
        }
  return {};
}
Result<void> ProgramImage::data () {
  for (auto &module : context.modules ()) {
    auto layouts = layout_module_data (module, target);
    if (!(layouts).has_value ()) return error (nullptr, "layout", layouts.error ());
    for (auto &layout : (layouts).value ()) {
      auto size = std::max (std::size_t{8}, layout.size);
      auto data = std::unique_ptr<std::byte[]> (new (std::nothrow) std::byte[size]{});
      if (!data) return error (nullptr, "data", invalid ("unable to allocate data section"));
      auto base = data.get ();
      sections.push_back ({std::move (data), size});
      ++statistics.sections;
      for (auto &item : layout.items) {
        auto &entry = bindings[identities.at (&item.data ().symbol ())];
        entry.address = reinterpret_cast<std::uintptr_t> (base + item.offset);
        if (auto initial = std::get_if<ir::InitializedData> (&item.data ().initializer ())) {
          auto width = target.layout (initial->type);
          if (!(width).has_value ()) return width.error ();
          auto at = base + item.offset;
          for (auto &v : initial->values) {
            if (auto value = ir::detail::operand_access (v).get<std::int64_t> ())
              store_integer (at, initial->type, std::bit_cast<std::uint64_t> (*value));
            else if (auto value = ir::detail::operand_access (v).get<std::uint64_t> ())
              store_integer (at, initial->type, *value);
            else if (auto value = ir::detail::operand_access (v).get<float> ())
              store (at, *value);
            else if (auto value = ir::detail::operand_access (v).get<double> ())
              store (at, *value);
            else
              store (at, *ir::detail::operand_access (v).get<long double> ());
            at += width.value ().size;
          }
        }
      }
    }
  }
  return {};
}
Result<void> ProgramImage::references (
  const std::function<Result<std::uint64_t> (const ir::Label &)> &label_address) {
  for (auto &module : context.modules ())
    for (auto &item : ir::detail::NodeAccess::items (module))
      if (auto data = std::get_if<const ir::GlobalData *> (&item)) {
        auto destination
          = reinterpret_cast<std::byte *> (bindings[identities.at (&(*data)->symbol ())].address);
        if (auto ref = std::get_if<ir::SymbolReference> (&(*data)->initializer ())) {
          auto &entry = binding (identities.at (ir::detail::NodeAccess::symbol (*(ref))));
          if (!entry.address)
            return error (&(*data)->symbol (), "relocate", invalid ("unresolved data reference"));
          store (destination, entry.address + std::bit_cast<std::uint64_t> (ref->displacement));
        } else if (auto ref = std::get_if<ir::LabelReference> (&(*data)->initializer ())) {
          auto address = label_address (*ir::detail::NodeAccess::label (*(ref)));
          if (!(address).has_value ())
            return error (&(*data)->symbol (), "relocate", address.error ());
          if (ir::detail::NodeAccess::base (*(ref))) {
            auto base = label_address (*ir::detail::NodeAccess::base (*(ref)));
            if (!(base).has_value ()) return base.error ();
            (address).value () -= (base).value ();
          }
          store (destination,
                 (address).value () + std::bit_cast<std::uint64_t> (ref->displacement));
        }
      }
  return {};
}
}  // namespace detail
}  // namespace mirnext::execution
