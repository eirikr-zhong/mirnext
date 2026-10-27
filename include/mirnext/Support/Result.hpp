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
template <class T>
class [[nodiscard]] Result {
 public:
  Result (T value) : storage_ (std::move (value)) {}
  Result (Error error) : storage_ (std::move (error)) {}

  bool has_value () const noexcept { return std::holds_alternative<T> (storage_); }
  bool has_error () const noexcept { return std::holds_alternative<Error> (storage_); }
  explicit operator bool () const noexcept { return has_value (); }

  T *value_ptr () noexcept { return std::get_if<T> (&storage_); }
  const T *value_ptr () const noexcept { return std::get_if<T> (&storage_); }
  Error *error_ptr () noexcept { return std::get_if<Error> (&storage_); }
  const Error *error_ptr () const noexcept { return std::get_if<Error> (&storage_); }

  Error &error () & noexcept {
    if (Error *error = std::get_if<Error> (&storage_)) return *error;
    std::abort ();
  }

  const Error &error () const & noexcept {
    if (const Error *error = std::get_if<Error> (&storage_)) return *error;
    std::abort ();
  }

  Error error () && noexcept {
    if (Error *error = std::get_if<Error> (&storage_)) return std::move (*error);
    std::abort ();
  }

  T &operator* () & noexcept {
    if (T *value = std::get_if<T> (&storage_)) return *value;
    std::abort ();
  }
  const T &operator* () const & noexcept {
    if (const T *value = std::get_if<T> (&storage_)) return *value;
    std::abort ();
  }
  T &&operator* () && noexcept {
    if (T *value = std::get_if<T> (&storage_)) return std::move (*value);
    std::abort ();
  }
  T *operator->() noexcept {
    if (T *value = std::get_if<T> (&storage_)) return value;
    std::abort ();
  }
  const T *operator->() const noexcept {
    if (const T *value = std::get_if<T> (&storage_)) return value;
    std::abort ();
  }

 private:
  std::variant<T, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result () = default;
  Result (Error error) : error_ (std::move (error)) {}

  bool has_value () const noexcept { return !error_.has_value (); }
  bool has_error () const noexcept { return error_.has_value (); }
  explicit operator bool () const noexcept { return has_value (); }

  Error *error_ptr () noexcept { return error_ ? &*error_ : nullptr; }
  const Error *error_ptr () const noexcept { return error_ ? &*error_ : nullptr; }

  Error &error () & noexcept {
    if (error_) return *error_;
    std::abort ();
  }

  const Error &error () const & noexcept {
    if (error_) return *error_;
    std::abort ();
  }

  Error error () && noexcept {
    if (error_) return std::move (*error_);
    std::abort ();
  }

 private:
  std::optional<Error> error_;
};

#define MIRNEXT_CONCAT_IMPL(lhs, rhs) lhs##rhs
#define MIRNEXT_CONCAT(lhs, rhs) MIRNEXT_CONCAT_IMPL (lhs, rhs)

// Bind a Result expression once. Successful lvalues retain their payload;
// failure transfers the first diagnostic to the caller (or explicit fatal sink).
#define MIRNEXT_DETAIL_RESULT_FAULT(result, local)                                        \
  do {                                                                                    \
    auto &&local = (result);                                                              \
    if (!local) {                                                                         \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " << local.error ().message << '\n'; \
      std::exit (EXIT_FAILURE);                                                           \
    }                                                                                     \
  } while (false)
#define MIRNEXT_RESULT_FAULT(result) \
  MIRNEXT_DETAIL_RESULT_FAULT (result, MIRNEXT_CONCAT (_mirnext_fault_, __LINE__))

#define MIRNEXT_DETAIL_RESULT_RET(result, local)   \
  do {                                             \
    auto &&local = (result);                       \
    if (!local) return std::move (local).error (); \
  } while (false)
#define MIRNEXT_RESULT_RET(result) \
  MIRNEXT_DETAIL_RESULT_RET (result, MIRNEXT_CONCAT (_mirnext_return_, __LINE__))

// Declares the identifier name in the caller's scope; use inside braces. Moving
// permits owning Context/session results and avoids copying other payloads.
// The identifier also names its temporary, so multiple uses on a line work
// without a translation-unit counter changing inline function definitions.
#define MIRNEXT_DETAIL_TRY_VALUE(name, expr, local) \
  auto local = (expr);                              \
  MIRNEXT_RESULT_RET (local);                       \
  auto name = *std::move (local)
#define MIRNEXT_TRY_VALUE(name, expr) \
  MIRNEXT_DETAIL_TRY_VALUE (name, expr, MIRNEXT_CONCAT (_mirnext_try_, name))

}  // namespace mirnext

#endif
