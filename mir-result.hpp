/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#ifndef MIRNEXT_RESULT_HPP
#define MIRNEXT_RESULT_HPP

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace mirnext {

enum class ErrorCode { InvalidArgument, DuplicateName, UnknownName, NoInsertPoint, InvalidOperand };

struct Error {
  ErrorCode code;
  std::string message;
};

// Result<T> is the no-exceptions error carrier used by the C++ API.
template <class T> class [[nodiscard]] Result {
public:
  Result(T value) : storage_(std::move(value)) {}
  Result(Error error) : storage_(std::move(error)) {}

  bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
  bool has_error() const noexcept { return std::holds_alternative<Error>(storage_); }
  explicit operator bool() const noexcept { return has_value(); }

  T *value_ptr() noexcept { return std::get_if<T>(&storage_); }
  const T *value_ptr() const noexcept { return std::get_if<T>(&storage_); }
  Error *error_ptr() noexcept { return std::get_if<Error>(&storage_); }
  const Error *error_ptr() const noexcept { return std::get_if<Error>(&storage_); }

  Error &error() & noexcept {
    if (Error *error = std::get_if<Error>(&storage_)) return *error;
    std::abort();
  }

  const Error &error() const & noexcept {
    if (const Error *error = std::get_if<Error>(&storage_)) return *error;
    std::abort();
  }

  Error error() && noexcept {
    if (Error *error = std::get_if<Error>(&storage_)) return std::move(*error);
    std::abort();
  }

  T operator*() & noexcept { return take_value(); }
  T operator*() && noexcept { return take_value(); }
  T *operator->() noexcept {
    if (T *value = std::get_if<T>(&storage_)) return value;
    std::abort();
  }
  const T *operator->() const noexcept {
    if (const T *value = std::get_if<T>(&storage_)) return value;
    std::abort();
  }

private:
  T take_value() noexcept {
    if (T *value = std::get_if<T>(&storage_)) return std::move(*value);
    std::abort();
  }

  std::variant<T, Error> storage_;
};

template <> class [[nodiscard]] Result<void> {
public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}

  bool has_value() const noexcept { return !error_.has_value(); }
  bool has_error() const noexcept { return error_.has_value(); }
  explicit operator bool() const noexcept { return has_value(); }

  Error *error_ptr() noexcept { return error_ ? &*error_ : nullptr; }
  const Error *error_ptr() const noexcept { return error_ ? &*error_ : nullptr; }

  Error &error() & noexcept {
    if (error_) return *error_;
    std::abort();
  }

  const Error &error() const & noexcept {
    if (error_) return *error_;
    std::abort();
  }

  Error error() && noexcept {
    if (error_) return std::move(*error_);
    std::abort();
  }

private:
  std::optional<Error> error_;
};

#define MIRNEXT_CONCAT_IMPL(lhs, rhs) lhs##rhs
#define MIRNEXT_CONCAT(lhs, rhs) MIRNEXT_CONCAT_IMPL(lhs, rhs)

#define MIRNEXT_RESULT_FAULT(result)                                  \
  do {                                                                \
    if (!(result)) {                                                  \
      std::cerr << __FILE__ << ':' << __LINE__ << ": "                \
                << (result).error().message << '\n';                  \
      std::exit(EXIT_FAILURE);                                        \
    }                                                                 \
  } while (false)

#define MIRNEXT_RESULT_RET(result)                                    \
  do {                                                                \
    if (!(result)) return std::move(result).error();                  \
  } while (false)

#define MIRNEXT_TRY_VALUE(name, expr)                                             \
  auto MIRNEXT_CONCAT(_mirnext_try_result_, __LINE__) = (expr);                   \
  MIRNEXT_RESULT_RET(MIRNEXT_CONCAT(_mirnext_try_result_, __LINE__));             \
  auto name = *MIRNEXT_CONCAT(_mirnext_try_result_, __LINE__)

} // namespace mirnext

#endif
