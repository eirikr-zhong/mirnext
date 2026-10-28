/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#ifndef MIRNEXT_RESULT_HPP
#define MIRNEXT_RESULT_HPP

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <type_traits>
#include <memory>
#include <variant>

namespace mirnext {

enum class ErrorCode { InvalidArgument, DuplicateName, UnknownName, NoInsertPoint, InvalidOperand };

struct Error {
  ErrorCode code;
  std::string message;
};

// A nullable borrowed reference. It never owns or extends the referent's lifetime.
template <class T>
class OptionalRef {
 public:
  OptionalRef () noexcept = default;
  OptionalRef (std::nullopt_t) noexcept {}
  template <class U>
    requires (std::is_lvalue_reference_v<U &&>
              && std::is_convertible_v<std::remove_reference_t<U> *, T *>)
  OptionalRef (U &&value) noexcept : value_ (std::addressof (value)) {}
  template <class U>
    requires std::is_convertible_v<U *, T *>
  OptionalRef (OptionalRef<U> value) noexcept : value_ (value.get ()) {}
  bool has_value () const noexcept { return value_ != nullptr; }
  T &value () const noexcept {
    if (!value_) std::abort ();
    return *value_;
  }
  T *get () const noexcept { return value_; }
  friend bool operator== (OptionalRef a, OptionalRef b) noexcept { return a.value_ == b.value_; }

 private:
  T *value_ = nullptr;
};

namespace detail {
template <class T>
OptionalRef<T> borrow_optional (T *value) noexcept {
  return value ? OptionalRef<T> (*value) : OptionalRef<T>{};
}
}  // namespace detail

// Result<T> is the no-exceptions error carrier used by the C++ API.
template <class T>
class [[nodiscard]] Result {
 public:
  using value_type = T;
  Result (T value) : storage_ (std::move (value)) {}
  template <class... Args>
    requires std::is_constructible_v<T, Args...>
  explicit Result (std::in_place_t, Args &&...args)
    : storage_ (std::in_place_type<T>, std::forward<Args> (args)...) {}
  Result (Error error) : storage_ (std::move (error)) {}

  bool has_value () const noexcept { return std::holds_alternative<T> (storage_); }
  bool has_error () const noexcept { return std::holds_alternative<Error> (storage_); }

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

  T &value () & noexcept {
    if (T *value = std::get_if<T> (&storage_)) return *value;
    std::abort ();
  }
  const T &value () const & noexcept {
    if (const T *value = std::get_if<T> (&storage_)) return *value;
    std::abort ();
  }
  T &&value () && noexcept {
    if (T *value = std::get_if<T> (&storage_)) return std::move (*value);
    std::abort ();
  }

 private:
  std::variant<T, Error> storage_;
};

// Successful reference results cannot be empty or bind a temporary.
template <class T>
class [[nodiscard]] Result<T &> {
 public:
  using value_type = T &;
  template <class U>
    requires (std::is_lvalue_reference_v<U &&>
              && std::is_convertible_v<std::remove_reference_t<U> *, T *>)
  Result (U &&value) noexcept : storage_ (std::addressof (value)) {}
  Result (Error error) : storage_ (std::move (error)) {}
  bool has_value () const noexcept { return std::holds_alternative<T *> (storage_); }
  bool has_error () const noexcept { return !has_value (); }
  Error &error () & noexcept {
    if (auto p = std::get_if<Error> (&storage_)) return *p;
    std::abort ();
  }
  const Error &error () const & noexcept {
    if (auto p = std::get_if<Error> (&storage_)) return *p;
    std::abort ();
  }
  Error error () && noexcept {
    if (auto p = std::get_if<Error> (&storage_)) return std::move (*p);
    std::abort ();
  }
  T &value () const noexcept {
    if (auto p = std::get_if<T *> (&storage_)) return **p;
    std::abort ();
  }

 private:
  std::variant<T *, Error> storage_;
};

template <>
class [[nodiscard]] Result<void> {
 public:
  Result () = default;
  Result (Error error) : error_ (std::move (error)) {}

  bool has_value () const noexcept { return !error_.has_value (); }
  bool has_error () const noexcept { return error_.has_value (); }

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
    if (!local.has_value ()) {                                                            \
      std::cerr << __FILE__ << ':' << __LINE__ << ": " << local.error ().message << '\n'; \
      std::exit (EXIT_FAILURE);                                                           \
    }                                                                                     \
  } while (false)
#define MIRNEXT_RESULT_FAULT(result) \
  MIRNEXT_DETAIL_RESULT_FAULT (result, MIRNEXT_CONCAT (_mirnext_fault_, __LINE__))

#define MIRNEXT_DETAIL_RESULT_RET(result, local)                \
  do {                                                          \
    auto &&local = (result);                                    \
    if (!local.has_value ()) return std::move (local).error (); \
  } while (false)
#define MIRNEXT_RESULT_RET(result) \
  MIRNEXT_DETAIL_RESULT_RET (result, MIRNEXT_CONCAT (_mirnext_return_, __LINE__))

// Declares the identifier name in the caller's scope; use inside braces. Moving
// permits owning Program/session results and avoids copying other payloads.
// The identifier also names its temporary, so multiple uses on a line work
// without a translation-unit counter changing inline function definitions.
#define MIRNEXT_DETAIL_TRY_VALUE(name, expr, local) \
  auto local = (expr);                              \
  MIRNEXT_RESULT_RET (local);                       \
  typename std::remove_cvref_t<decltype (local)>::value_type name = std::move (local).value ()
#define MIRNEXT_TRY_VALUE(name, expr) \
  MIRNEXT_DETAIL_TRY_VALUE (name, expr, MIRNEXT_CONCAT (_mirnext_try_, name))

}  // namespace mirnext

#endif
