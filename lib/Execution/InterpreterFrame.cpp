/* Licensed under the MIT License. Typed frame storage and memory operations. */
#include "mirnext/Execution/detail/LoadedProgram.hpp"
#include <bit>
namespace mirnext::execution::detail {
namespace {
Error invalid(std::string reason) { return {ErrorCode::InvalidOperand,std::move(reason)}; }
template<class T> T raw(const ScalarValue &value) { T result; std::memcpy(&result,value.storage().data(),sizeof result); return result; }
template<class T> T load(std::uintptr_t address) { T value; std::memcpy(&value,reinterpret_cast<const std::byte *>(address),sizeof value); return value; }
template<class T> void store(std::uintptr_t address,T value) { std::memcpy(reinterpret_cast<std::byte *>(address),&value,sizeof value); }
}
Result<ScalarValue> load_scalar(ir::Type type,std::uintptr_t address) {
  if (!address) return invalid("null scalar memory read");
  using K = ir::Type::Kind;
  switch (type.kind()) {
    case K::I8: return ScalarValue(std::int64_t(load<std::int8_t>(address)));
    case K::I16: return ScalarValue(std::int64_t(load<std::int16_t>(address)));
    case K::I32: return ScalarValue(std::int64_t(load<std::int32_t>(address)));
    case K::U8: return ScalarValue(std::uint64_t(load<std::uint8_t>(address)));
    case K::U16: return ScalarValue(std::uint64_t(load<std::uint16_t>(address)));
    case K::U32: return ScalarValue(std::uint64_t(load<std::uint32_t>(address)));
    case K::Bool: case K::I64: case K::U64: case K::Pointer: return ScalarValue(load<std::uint64_t>(address));
    case K::F32: return ScalarValue(load<float>(address)); case K::F64: return ScalarValue(load<double>(address));
    case K::LongDouble: return ScalarValue(load<long double>(address));
    default: return invalid("non-scalar memory read");
  }
}
Result<void> store_scalar(ir::Type type,std::uintptr_t address,const ScalarValue &value) {
  if (!address) return invalid("null scalar memory write"); using K = ir::Type::Kind;
  switch (type.kind()) {
    case K::I8: case K::U8: store(address,std::uint8_t(raw<std::uint64_t>(value))); break;
    case K::I16: case K::U16: store(address,std::uint16_t(raw<std::uint64_t>(value))); break;
    case K::I32: case K::U32: store(address,std::uint32_t(raw<std::uint64_t>(value))); break;
    case K::Bool: case K::I64: case K::U64: case K::Pointer: store(address,raw<std::uint64_t>(value)); break;
    case K::F32: store(address,raw<float>(value)); break; case K::F64: store(address,raw<double>(value)); break;
    case K::LongDouble: store(address,raw<long double>(value)); break;
    default: return invalid("non-scalar memory write");
  } return {};
}
InterpreterFrame::InterpreterFrame(InterpreterInvocation &i,InterpreterBody &b)
  : invocation(i),body(b),locals(b.registers.size()),region_depth(i.variadic_areas.size()),native_depth(i.native_variadics.size()) {
  registers.reserve(locals.size());
  for (std::size_t n = 0; n < locals.size(); ++n) registers.push_back(b.hardware[n] == no_slot ? &locals[n] : &i.state.hardware[b.hardware[n]]);
}
InterpreterFrame::~InterpreterFrame() { invocation.variadic_areas.resize(region_depth); invocation.native_variadics.resize(native_depth); }
Result<void> InterpreterFrame::initialize(std::span<const ExecutionArgument> input,std::span<const std::byte> native) {
  const bool variadic = body.source->signature().variadic;
  if (!native.empty()) {
    if (!variadic) return invalid("native variadic state in fixed function");
    native_va = native; invocation.native_variadics.push_back(native);
  }
  if (variadic) arguments.reserve(input.size());
  for (std::size_t n = 0; n < input.size(); ++n) {
    auto argument = input[n];
    if (auto block = argument.block()) {
      auto bytes = block->bytes;
      if (block->type.kind() != ir::Type::Kind::ReturnBlock) {
        auto allocation = stack.allocate(bytes.size()); if (!allocation) return allocation.error();
        std::memcpy(reinterpret_cast<std::byte *>(*allocation),bytes.data(),bytes.size());
        bytes = {reinterpret_cast<const std::byte *>(*allocation),bytes.size()};
        argument = ExecutionArgument::block(bytes,block->type.block_class());
      }
      if (n < body.arguments.size()) *registers[body.arguments[n]] = slot(reinterpret_cast<std::uintptr_t>(bytes.data()));
    } else if (n < body.arguments.size()) registers[body.arguments[n]]->bytes = argument.scalar()->storage();
    if (variadic) arguments.push_back(argument);
  }
  return {};
}
Result<std::uintptr_t> InterpreterFrame::address(const PreparedOperand &p) const {
  auto mem = p.source->get<ir::Memory>(); if (!mem) return invalid("operand is not memory");
  auto bits = std::bit_cast<std::uint64_t>(mem->displacement);
  if (p.base != no_slot) bits += read_slot<std::uint64_t>(*registers[p.base]);
  if (p.index != no_slot) bits += read_slot<std::uint64_t>(*registers[p.index])*mem->scale;
  return static_cast<std::uintptr_t>(bits);
}
Result<ScalarValue> InterpreterFrame::read(const PreparedOperand &p,ir::Type type) {
  auto &o = *p.source;
  if (p.reg != no_slot) return ScalarValue::from_storage(type,registers[p.reg]->bytes);
  if (auto memory = o.get<ir::Memory>()) {
    auto a = address(p); if (!a) return a.error();
    // Undef va-list operands denote their address, rather than a load.
    if (memory->type.kind() == ir::Type::Kind::Undef) return ScalarValue::pointer(*a);
    return load_scalar(memory->type,*a);
  }
  if (auto v = o.get<std::int64_t>()) return ScalarValue(*v); if (auto v = o.get<std::uint64_t>()) return ScalarValue(*v);
  if (auto v = o.get<float>()) return ScalarValue(*v); if (auto v = o.get<double>()) return ScalarValue(*v); if (auto v = o.get<long double>()) return ScalarValue(*v);
  if (auto v = o.get<std::string>()) return ScalarValue::pointer(reinterpret_cast<std::uintptr_t>(v->c_str()));
  if (auto v = o.get<ir::SymbolReference>()) return ScalarValue::pointer(invocation.state.binding(p.binding).address+std::bit_cast<std::uint64_t>(v->displacement));
  return invalid("operand is not a scalar value");
}
Result<void> InterpreterFrame::write(const PreparedOperand &p,const ScalarValue &value) {
  if (p.reg != no_slot) { registers[p.reg]->bytes = value.storage(); return {}; }
  if (auto mem = p.source->get<ir::Memory>()) {
    auto a = address(p); if (!a) return a.error(); auto layout = invocation.state.target.layout(mem->type); if (!layout) return layout.error();
    auto writable = invocation.state.writable(*a,layout->size); if (!writable) return writable;
    return store_scalar(mem->type,*a,value);
  }
  return invalid("operand is not a scalar destination");
}
}
