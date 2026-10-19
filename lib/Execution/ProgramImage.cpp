/* Licensed under the MIT License. Shared symbol and data image for execution engines. */
#include "mirnext/Execution/detail/ProgramImage.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <new>
namespace mirnext::execution {
namespace {
Error invalid(std::string message) { return {ErrorCode::InvalidOperand,std::move(message)}; }
Result<std::size_t> data_size(const ir::GlobalData &data,const target::TargetMachine &target) {
  auto &value = data.initializer();
  if (auto zero = std::get_if<ir::ZeroData>(&value)) return zero->size;
  ir::Type type = ir::Type::pointer(); std::size_t count = 1;
  if (auto initial = std::get_if<ir::InitializedData>(&value)) { type = initial->type; count = initial->values.size(); }
  else if (auto expression = std::get_if<ir::ExpressionData>(&value)) type = expression->function->signature().results[0];
  auto layout = target.layout(type); if (!layout) return layout.error();
  if (count > SIZE_MAX/layout->size) return invalid("data element size overflow"); return count * layout->size;
}
template<class T> void store(std::byte *destination,const T &value) { std::memcpy(destination,&value,sizeof value); }
void store_integer(std::byte *destination,ir::Type type,std::uint64_t value) {
  using K = ir::Type::Kind;
  switch (type.kind()) {
    case K::I8: case K::U8: store(destination,static_cast<std::uint8_t>(value)); break;
    case K::I16: case K::U16: store(destination,static_cast<std::uint16_t>(value)); break;
    case K::I32: case K::U32: store(destination,static_cast<std::uint32_t>(value)); break;
    default: store(destination,value); break;
  }
}
bool jump_return(const ir::Function &f) {
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) if (i.opcode() == ir::Opcode::JRet) return true; return false;
}
bool compatible_type(ir::Type a,ir::Type b) {
  return a == b || ((a.kind() == ir::Type::Kind::Bool || a.kind() == ir::Type::Kind::Pointer)
    && (b.kind() == ir::Type::Kind::I64 || b.kind() == ir::Type::Kind::U64));
}
}
Result<std::vector<DataSectionLayout>> layout_module_data(const ir::Module &module,const target::TargetMachine &target) {
  auto checked = module.verify(); if (!checked) return checked.error();
  std::vector<DataSectionLayout> result; bool active = false;
  for (auto &item : module.items()) {
    auto entry = std::get_if<const ir::GlobalData *>(&item); if (!entry) { active = false; continue; }
    auto &data = **entry; auto size = data_size(data,target); if (!size) return size.error();
    if (!active || !data.symbol().name().empty()) { result.emplace_back(); active = true; }
    auto &section = result.back(); if (*size > SIZE_MAX - section.size) return invalid("data section size overflow");
    section.items.push_back({&data,section.size,*size}); section.size += *size;
  }
  for (auto &section : result) {
    if (section.size > SIZE_MAX - 7) return invalid("data section alignment overflow"); section.size = (section.size + 7)/8*8;
  }
  return result;
}
namespace detail {
Error ProgramImage::error(const ir::Symbol *symbol,std::string_view phase,Error e) const {
    e.message = (symbol ? std::string(symbol->module().name()) + '/' + std::string(symbol->name()) : std::string("program"))
      + " [" + std::string(target.name()) + "/link:" + std::string(phase) + "]: " + e.message; return e;
  }
Result<std::size_t> ProgramImage::find(std::string_view module,std::string_view name) const {
    auto m = context.find_module(module); auto s = m ? m->find_symbol(name) : nullptr;
    if (!s) return Error{ErrorCode::UnknownName,std::string(module) + '/' + std::string(name) + ": unknown linked symbol"}; return identities.at(s);
  }
Result<void> ProgramImage::check_import(std::size_t index) const {
    auto &import = *bindings[index].source; auto &definition = binding(index);
    if (import.signature() && (definition.kind != Kind::Function || !definition.signature || *definition.signature != *import.signature()))
      return error(&import,"symbols",invalid("import function signature mismatch"));
    if (auto variable = import.variable()) {
      if (definition.kind != Kind::Data || (variable->writable() && !definition.writable)) return error(&import,"symbols",invalid("import data kind or write permission mismatch"));
      auto layout = target.layout(variable->type()); if (!layout) return layout.error();
      if (definition.size < layout->size) return error(&import,"symbols",invalid("imported variable exceeds external storage"));
    }
    return {};
  }
Result<void> ProgramImage::symbols(const LinkOptions &options,bool native) {
    for (auto &module : context.modules()) for (auto &symbol : module.symbols()) {
      auto index = bindings.size(); identities.emplace(&symbol,index); Binding entry{&symbol};
      if (symbol.signature()) entry.signature = *symbol.signature();
      if (symbol.function()) { entry.kind = Kind::Function; entry.jump = jump_return(*symbol.function()); }
      if (symbol.data()) { entry.kind = Kind::Data; entry.writable = symbol.data()->writable(); auto size = data_size(*symbol.data(),target); if (!size) return error(&symbol,"layout",size.error()); entry.size = *size; }
      bindings.push_back(std::move(entry)); if (symbol.exported()) exports.emplace(symbol.name(),index);
    }
    std::unordered_map<std::string,ExternalSymbol> externals;
    for (std::size_t n = 0; n < bindings.size(); ++n) {
      auto &entry = bindings[n]; auto &symbol = *entry.source; if (symbol.kind() != ir::Symbol::Kind::Import) continue;
      if (auto exported = exports.find(symbol.name()); exported != exports.end()) entry.alias = exported->second;
      else {
        auto found = externals.find(std::string(symbol.name()));
        if (found == externals.end()) {
          if (!options.resolve) return error(&symbol,"symbols",{ErrorCode::UnknownName,"unresolved import"});
          auto resolved = options.resolve({symbol.module().name(),symbol.name(),symbol.signature(),symbol.variable()});
          if (!resolved) return error(&symbol,"symbols",resolved.error()); ++statistics.external_resolutions;
          if ((!resolved->address && (native || !resolved->interpret || !resolved->signature))
              || bool(resolved->signature) == bool(resolved->data)) return error(&symbol,"symbols",invalid("external binding requires an execution entry and exactly one kind"));
          if (resolved->signature) { auto valid = resolved->signature->verify(); if (!valid) return error(&symbol,"symbols",valid.error()); }
          if (resolved->data && (resolved->jump_return || resolved->interpret || (resolved->data->scalar_type && (!resolved->data->scalar_type->valid() || !resolved->data->scalar_type->is_scalar()))
              || resolved->data->size > UINTPTR_MAX - resolved->address)) return error(&symbol,"symbols",invalid("invalid external data binding"));
          found = externals.emplace(std::string(symbol.name()),std::move(*resolved)).first;
        }
        auto &external = found->second; entry.address = external.address; entry.signature = external.signature; entry.jump = external.jump_return; entry.interpret = external.interpret;
        entry.kind = external.signature ? Kind::Function : Kind::Data;
        if (external.data) {
          entry.size = external.data->size; entry.writable = external.data->writable;
          if (symbol.variable() && external.data->scalar_type && !compatible_type(symbol.variable()->type(),*external.data->scalar_type))
            return error(&symbol,"symbols",invalid("external variable type mismatch"));
        }
      }
      auto checked = check_import(n); if (!checked) return checked;
    }
    // Untyped imports still have a call-site prototype. Validate the resolved
    // definition and special calling convention before any native code exists.
    for (auto &module : context.modules()) for (auto &f : module.functions()) for (auto &b : f.blocks()) for (auto &i : b.instructions()) {
      if (ir::instruction_descriptor(i.opcode())->count != ir::OperandCount::Call) continue;
      auto ref = i.operands()[1].get<ir::SymbolReference>(); if (!ref) continue;
      auto &callee = binding(identities.at(ref->symbol)); auto prototype = i.operands()[0].get<ir::SymbolReference>();
      if (!prototype || !prototype->symbol->signature() || callee.kind != Kind::Function || !callee.signature
          || *callee.signature != *prototype->symbol->signature() || callee.jump != (i.opcode() == ir::Opcode::JCall))
        return error(&f.symbol(),"symbols",invalid("resolved call signature, symbol kind or convention mismatch"));
    }
    return {};
  }
Result<void> ProgramImage::data() {
    for (auto &module : context.modules()) {
      auto layouts = layout_module_data(module,target); if (!layouts) return error(nullptr,"layout",layouts.error());
      for (auto &layout : *layouts) {
        auto size = std::max(std::size_t{8},layout.size); auto data = std::unique_ptr<std::byte[]>(new(std::nothrow) std::byte[size]{});
        if (!data) return error(nullptr,"data",invalid("unable to allocate data section")); auto base = data.get(); sections.push_back({std::move(data),size}); ++statistics.sections;
        for (auto &item : layout.items) {
          auto &entry = bindings[identities.at(&item.data->symbol())]; entry.address = reinterpret_cast<std::uintptr_t>(base + item.offset);
          if (auto initial = std::get_if<ir::InitializedData>(&item.data->initializer())) {
            auto width = target.layout(initial->type); if (!width) return width.error(); auto at = base + item.offset;
            for (auto &v : initial->values) {
              if (auto value = v.get<std::int64_t>()) store_integer(at,initial->type,std::bit_cast<std::uint64_t>(*value));
              else if (auto value = v.get<std::uint64_t>()) store_integer(at,initial->type,*value);
              else if (auto value = v.get<float>()) store(at,*value); else if (auto value = v.get<double>()) store(at,*value); else store(at,*v.get<long double>());
              at += width->size;
            }
          }
        }
      }
    }
    return {};
  }
Result<void> ProgramImage::references(const std::function<Result<std::uint64_t>(const ir::Label &)> &label_address) {
    for (auto &module : context.modules()) for (auto &item : module.items()) if (auto data = std::get_if<const ir::GlobalData *>(&item)) {
      auto destination = reinterpret_cast<std::byte *>(bindings[identities.at(&(*data)->symbol())].address);
      if (auto ref = std::get_if<ir::SymbolReference>(&(*data)->initializer())) {
        auto &entry = binding(identities.at(ref->symbol)); if (!entry.address) return error(&(*data)->symbol(),"relocate",invalid("unresolved data reference"));
        store(destination,entry.address + std::bit_cast<std::uint64_t>(ref->displacement));
      } else if (auto ref = std::get_if<ir::LabelReference>(&(*data)->initializer())) {
        auto address = label_address(*ref->label); if (!address) return error(&(*data)->symbol(),"relocate",address.error());
        if (ref->base) { auto base = label_address(*ref->base); if (!base) return base.error(); *address -= *base; }
        store(destination,*address + std::bit_cast<std::uint64_t>(ref->displacement));
      }
    }
    return {};
  }
}
}
