/* Licensed under the MIT License. Typed values at C++ execution boundaries. */
#pragma once
#include "mirnext/IR/IR.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <functional>
namespace mirnext::execution {
class ScalarValue {
public:
  static constexpr std::size_t storage_size = sizeof(long double) > 8 ? sizeof(long double) : 8;
  using Storage = std::array<std::byte,storage_size>;
  ScalarValue() = default;
  ScalarValue(std::int64_t v) : ScalarValue(ir::Type::i64(),v) {}
  ScalarValue(std::uint64_t v) : ScalarValue(ir::Type(ir::Type::Kind::U64),v) {}
  template<class T> requires (std::is_integral_v<T> && sizeof(T) <= 8)
  ScalarValue(T v) : ScalarValue(static_cast<std::conditional_t<std::is_signed_v<T>,std::int64_t,std::uint64_t>>(v)) {}
  ScalarValue(float v) : ScalarValue(ir::Type(ir::Type::Kind::F32),v) {}
  ScalarValue(double v) : ScalarValue(ir::Type(ir::Type::Kind::F64),v) {}
  ScalarValue(long double v) : ScalarValue(ir::Type(ir::Type::Kind::LongDouble),v) {}
  static ScalarValue pointer(std::uintptr_t v) { return ScalarValue(ir::Type::pointer(),std::uint64_t(v)); }
  static Result<ScalarValue> from_storage(ir::Type,std::span<const std::byte>);
  ir::Type type() const { return type_; }
  const Storage &storage() const { return storage_; }
  Result<std::int64_t> as_i64() const;
  Result<std::uint64_t> as_u64() const;
  Result<float> as_float() const;
  Result<double> as_double() const;
  Result<long double> as_long_double() const;
private:
  template<class T> ScalarValue(ir::Type type,T v) : type_(type) { std::memcpy(storage_.data(),&v,sizeof v); }
  ir::Type type_ = ir::Type::i64();
  alignas(long double) Storage storage_{};
};
struct BlockValue {
  ir::Type type;
  std::span<const std::byte> bytes;
  std::span<std::byte> writable;
};
class ExecutionArgument {
public:
  ExecutionArgument(ScalarValue v) : value_(std::move(v)) {}
  template<class T> requires std::is_arithmetic_v<T>
  ExecutionArgument(T v) : value_(ScalarValue(v)) {}
  static ExecutionArgument block(std::span<const std::byte> bytes,ir::Type::BlockClass abi = ir::Type::BlockClass::Default) {
    return ExecutionArgument(BlockValue{ir::Type::block(bytes.size(),abi),bytes,{}});
  }
  static ExecutionArgument return_buffer(std::span<std::byte> bytes) {
    return ExecutionArgument(BlockValue{ir::Type::return_block(bytes.size()),bytes,bytes});
  }
  const ScalarValue *scalar() const { return std::get_if<ScalarValue>(&value_); }
  const BlockValue *block() const { return std::get_if<BlockValue>(&value_); }
  ir::Type type() const { return scalar() ? scalar()->type() : block()->type; }
private:
  explicit ExecutionArgument(BlockValue v) : value_(v) {}
  std::variant<ScalarValue,BlockValue> value_;
};
using InterpreterHostFunction = std::function<Result<std::vector<ScalarValue>>(std::span<const ExecutionArgument>)>;
}
