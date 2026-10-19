/* Licensed under the MIT License. Checked scalar value access. */
#include "mirnext/Execution/detail/Values.hpp"
namespace mirnext::execution {
namespace {
Error invalid() { return {ErrorCode::InvalidArgument,"execution scalar type/storage mismatch"}; }
template<class T> T read(const ScalarValue::Storage &s) { T value; std::memcpy(&value,s.data(),sizeof value); return value; }
}
Result<ScalarValue> ScalarValue::from_storage(ir::Type type,std::span<const std::byte> bytes) {
  if (!type.valid() || !type.is_scalar() || bytes.size() < storage_size) return invalid();
  ScalarValue result; result.type_ = type; std::memcpy(result.storage_.data(),bytes.data(),storage_size); return result;
}
Result<std::int64_t> ScalarValue::as_i64() const { if (!type_.is_integer()) return invalid(); return read<std::int64_t>(storage_); }
Result<std::uint64_t> ScalarValue::as_u64() const { if (!type_.is_integer()) return invalid(); return read<std::uint64_t>(storage_); }
Result<float> ScalarValue::as_float() const { if (type_.kind() != ir::Type::Kind::F32) return invalid(); return read<float>(storage_); }
Result<double> ScalarValue::as_double() const { if (type_.kind() != ir::Type::Kind::F64) return invalid(); return read<double>(storage_); }
Result<long double> ScalarValue::as_long_double() const { if (type_.kind() != ir::Type::Kind::LongDouble) return invalid(); return read<long double>(storage_); }
}

namespace mirnext::execution::detail {
namespace { Error invalid(std::string reason) { return {ErrorCode::InvalidOperand,std::move(reason)}; } }
Result<void> check_argument_count(const ir::Signature &sig,std::size_t count) {
  if (count < sig.parameters.size() || (!sig.variadic && count != sig.parameters.size()))
    return invalid("call argument count mismatch");
  return {};
}
Result<ExecutionArgument> normalize_argument(ir::Type type,const ExecutionArgument &argument,bool unnamed) {
  if (type.is_block()) {
    auto block = argument.block();
    if (!type.valid() || !block || block->type != type || block->bytes.size() != type.block_size()
        || (type.kind() == ir::Type::Kind::ReturnBlock && block->writable.size() != type.block_size()))
      return invalid("block argument size, ABI classification or mutability mismatch");
    if (!type.block_size() || (unnamed && type.kind() == ir::Type::Kind::ReturnBlock))
      return invalid("invalid variadic block argument");
    return argument;
  }
  auto scalar = argument.scalar(); if (!scalar) return invalid("scalar parameter received a block");
  if (unnamed && scalar->type().kind() == ir::Type::Kind::F32) return invalid("float variadic argument must be promoted to double");
  auto value = normalize_scalar(type,*scalar); if (!value) return value.error(); return ExecutionArgument(*value);
}
Result<NormalizedArguments> normalize_arguments(const ir::Signature &sig,std::span<const ExecutionArgument> arguments,bool copy_blocks) {
  auto count = check_argument_count(sig,arguments.size()); if (!count) return count.error();
  NormalizedArguments normalized; normalized.values.reserve(arguments.size());
  for (std::size_t n = 0; n < arguments.size(); ++n) {
    auto type = n < sig.parameters.size() ? sig.parameters[n] : arguments[n].type();
    auto value = normalize_argument(type,arguments[n],n >= sig.parameters.size()); if (!value) return value.error();
    if (auto block = value->block(); copy_blocks && block && type.kind() != ir::Type::Kind::ReturnBlock) {
      auto &copy = normalized.blocks.emplace_back(block->bytes.begin(),block->bytes.end());
      normalized.values.push_back(ExecutionArgument::block(copy,type.block_class()));
    } else normalized.values.push_back(std::move(*value));
  }
  return normalized;
}
Result<ScalarValue> normalize_scalar(ir::Type type,const ScalarValue &value) {
  if (!type.is_scalar() || !value.type().is_scalar()) return invalid("invalid scalar type");
  using K = ir::Type::Kind;
  if (type.is_float()) {
    if (type == value.type()) return value;
    // The verifier and maintained ABIs share these scalar representations on
    // hosts where long double is double. Preserve bits instead of converting.
    if constexpr (sizeof(long double) == sizeof(double)) {
      if ((type.kind() == K::LongDouble && value.type().kind() == K::F64)
          || (type.kind() == K::F64 && value.type().kind() == K::LongDouble))
        return ScalarValue::from_storage(type,value.storage());
    }
    return invalid("floating scalar type mismatch");
  }
  if (!value.type().is_integer()) return invalid("integer scalar type mismatch"); auto bits = read<std::uint64_t>(value.storage());
  ScalarValue normalized = value;
  switch (type.kind()) {
    case K::I8: normalized = ScalarValue(std::int64_t(std::bit_cast<std::int8_t>(std::uint8_t(bits)))); break;
    case K::I16: normalized = ScalarValue(std::int64_t(std::bit_cast<std::int16_t>(std::uint16_t(bits)))); break;
    case K::I32: normalized = ScalarValue(std::int64_t(std::bit_cast<std::int32_t>(std::uint32_t(bits)))); break;
    case K::U8: normalized = ScalarValue(std::uint64_t(std::uint8_t(bits))); break;
    case K::U16: normalized = ScalarValue(std::uint64_t(std::uint16_t(bits))); break;
    case K::U32: normalized = ScalarValue(std::uint64_t(std::uint32_t(bits))); break;
    default: break;
  }
  return ScalarValue::from_storage(type,normalized.storage());
}
}
