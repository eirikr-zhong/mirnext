/* Licensed under the MIT License. No-exceptions error propagation and ownership. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Support/Result.hpp"
#include <memory>
#include <type_traits>
using namespace mirnext;
namespace {
Error failure () { return {ErrorCode::UnknownName, "first diagnostic"}; }
struct Owned {
  int *destroyed;
  explicit Owned (int &count) : destroyed (&count) {}
  ~Owned () { ++*destroyed; }
  Owned (const Owned &) = delete;
  Owned &operator= (const Owned &) = delete;
};
static_assert (std::is_move_constructible_v<Result<std::unique_ptr<Owned>>>);
static_assert (!std::is_copy_constructible_v<Result<std::unique_ptr<Owned>>>);
}  // namespace
TEST_CASE ("Result propagation evaluates success and failure expressions once") {
  for (bool failed : {false, true}) {
    int calls = 0, completed = 0;
    auto produce = [&] () -> Result<int> {
      ++calls;
      if (failed) return failure ();
      return 42;
    };
    auto propagate = [&] () -> Result<void> {
      MIRNEXT_RESULT_RET (produce ());
      ++completed;
      return {};
    };
    auto result = propagate ();
    CHECK (calls == 1);
    CHECK (completed == !failed);
    CHECK (bool ((result).has_value ()) == !failed);
    if (failed) {
      CHECK (result.error ().code == ErrorCode::UnknownName);
      CHECK (result.error ().message == "first diagnostic");
    }
  }
}
TEST_CASE ("Result propagation supports void temporaries and const lvalue diagnostics") {
  int calls = 0;
  auto produce = [&] () -> Result<void> {
    if (++calls == 1) return failure ();
    return {};
  };
  auto propagate = [&] () -> Result<int> {
    MIRNEXT_RESULT_RET (produce ());
    return 42;
  };
  auto result = propagate ();
  REQUIRE_FALSE ((result).has_value ());
  CHECK (calls == 1);
  CHECK (result.error ().message == "first diagnostic");
  const Result<void> failed = failure ();
  auto copy_error = [&] () -> Result<void> {
    MIRNEXT_RESULT_RET (failed);
    return {};
  };
  auto copied = copy_error ();
  REQUIRE_FALSE ((copied).has_value ());
  CHECK (copied.error ().code == failed.error ().code);
  CHECK (copied.error ().message == failed.error ().message);
}
TEST_CASE ("successful Result lvalues keep move-only payloads when only checking status") {
  Result<std::unique_ptr<int>> owned = std::make_unique<int> (42);
  auto address = owned.value ().get ();
  auto check = [&] () -> Result<void> {
    MIRNEXT_RESULT_RET (owned);
    MIRNEXT_RESULT_FAULT (owned);
    return {};
  };
  REQUIRE ((check ()).has_value ());
  CHECK (owned.value ().get () == address);
  CHECK (*(owned).value () == 42);
}
TEST_CASE (
  "Result value extraction transfers sole ownership without copying or early destruction") {
  int destroyed = 0, calls = 0;
  auto produce = [&] () -> Result<std::unique_ptr<Owned>> {
    ++calls;
    return std::make_unique<Owned> (destroyed);
  };
  auto transfer = [&] () -> Result<std::unique_ptr<Owned>> {
    MIRNEXT_TRY_VALUE (owned, produce ());
    CHECK (destroyed == 0);
    return std::move (owned);
  };
  {
    auto result = transfer ();
    REQUIRE ((result).has_value ());
    REQUIRE (result.value ().get ());
    CHECK (calls == 1);
    CHECK (destroyed == 0);
  }
  CHECK (destroyed == 1);
}
TEST_CASE ("Result extraction propagates a move-only factory failure without using its value") {
  int calls = 0, completed = 0;
  auto produce = [&] () -> Result<std::unique_ptr<int>> {
    ++calls;
    return failure ();
  };
  auto extract = [&] () -> Result<int> {
    MIRNEXT_TRY_VALUE (owned, produce ());
    ++completed;
    return *owned;
  };
  auto result = extract ();
  REQUIRE_FALSE ((result).has_value ());
  CHECK (calls == 1);
  CHECK (completed == 0);
  CHECK (result.error ().message == "first diagnostic");
  CHECK (result.error ().code == ErrorCode::UnknownName);
}
TEST_CASE ("Result helpers allow nested expressions and multiple extractions on one source line") {
  auto produce = [] (int value) -> Result<int> { return value; };
  auto chained = [&] () -> Result<int> {
    MIRNEXT_TRY_VALUE (left, produce (20));
    MIRNEXT_TRY_VALUE (right, produce (22));
    return left + right;
  };
  auto result = chained ();
  REQUIRE ((result).has_value ());
  CHECK ((result).value () == 42);
  int checks = 0;
  auto checked = [&] () -> Result<void> {
    if (++checks)
      MIRNEXT_RESULT_RET (produce (1));
    else
      return failure ();
    return {};
  };
  REQUIRE ((checked ()).has_value ());
  CHECK (checks == 1);
}

static_assert (std::is_constructible_v<Result<const int &>, int &>);
static_assert (std::is_constructible_v<Result<const int &>, const int &>);
static_assert (!std::is_constructible_v<Result<const int &>, int &&>);
static_assert (!std::is_constructible_v<Result<const int &>, const int &&>);
static_assert (!std::is_constructible_v<Result<int &>, const int &>);
static_assert (!std::is_constructible_v<OptionalRef<const int>, int &&>);
static_assert (!std::is_constructible_v<OptionalRef<const int>, const int &&>);
static_assert (std::is_constructible_v<OptionalRef<const int>, const int &>);
TEST_CASE ("borrowed results retain identity and constness without owning values") {
  int original = 41;
  Result<int &> borrowed = original;
  REQUIRE ((borrowed).has_value ());
  CHECK (&(borrowed).value () == &original);
  (borrowed).value () = 42;
  CHECK (original == 42);
  const Result<int &> copied = borrowed;
  CHECK (&(copied).value () == &original);
  Result<const int &> readonly = original;
  CHECK (&(readonly).value () == &original);
  Result<int &> failed = failure ();
  CHECK_FALSE ((failed).has_value ());
  CHECK (failed.has_error ());
  CHECK (failed.error ().message == "first diagnostic");
  auto propagated = [&] () -> Result<int &> {
    MIRNEXT_RESULT_RET (failed);
    return original;
  }();
  CHECK_FALSE ((propagated).has_value ());
  CHECK (propagated.error ().code == ErrorCode::UnknownName);
}
TEST_CASE ("optional references distinguish absence from error results") {
  int original = 42;
  OptionalRef<int> absent;
  CHECK_FALSE ((absent).has_value ());
  CHECK (absent.get () == nullptr);
  OptionalRef<int> mutable_view = original;
  OptionalRef<const int> readonly = mutable_view;
  CHECK ((readonly).has_value ());
  CHECK (&(readonly).value () == &original);
  CHECK (mutable_view.get () == &original);
  (mutable_view).value () = 43;
  CHECK ((readonly).value () == 43);
}
