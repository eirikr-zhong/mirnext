/* Licensed under the MIT License. Admission errors never execute foreign code. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "NativeHost.hpp"
using namespace mirnext;
TEST_CASE ("native acceptance checks target host architecture OS and translation independently") {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto host = test::detect_native_host ();
  REQUIRE ((host).has_value ());
  REQUIRE ((test::check_native_host ((host).value (), (target).value ())).has_value ());
  REQUIRE ((test::check_native_host ((host).value (), (target).value (), target.value ().name ()))
             .has_value ());
  auto wrong = (host).value ();
  wrong.translated = true;
  auto rejected = test::check_native_host (wrong, (target).value ());
  CHECK_FALSE ((rejected).has_value ());
  CHECK (rejected.error ().message.find ("translated") != std::string::npos);
  wrong = (host).value ();
  wrong.architecture = host.value ().architecture == "aarch64" ? "x86_64" : "aarch64";
  rejected = test::check_native_host (wrong, (target).value ());
  CHECK_FALSE ((rejected).has_value ());
  CHECK (rejected.error ().message.find ("architecture") != std::string::npos);
  wrong = (host).value ();
  wrong.system = "other";
  CHECK_FALSE ((test::check_native_host (wrong, (target).value ())).has_value ());
  CHECK_FALSE (
    (test::check_native_host ((host).value (), (target).value (), "not-a-target")).has_value ());
}
