/* Licensed under the MIT License. Admission errors never execute foreign code. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "NativeHost.hpp"
using namespace mirnext;
TEST_CASE("native acceptance checks target host architecture OS and translation independently") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  auto host = test::detect_native_host(); REQUIRE(host); REQUIRE(test::check_native_host(*host,*target));
  REQUIRE(test::check_native_host(*host,*target,target->name()));
  auto wrong = *host; wrong.translated = true;
  auto rejected = test::check_native_host(wrong,*target); CHECK_FALSE(rejected);
  CHECK(rejected.error().message.find("translated") != std::string::npos);
  wrong = *host; wrong.architecture = host->architecture == "aarch64" ? "x86_64" : "aarch64";
  rejected = test::check_native_host(wrong,*target); CHECK_FALSE(rejected);
  CHECK(rejected.error().message.find("architecture") != std::string::npos);
  wrong = *host; wrong.system = "other"; CHECK_FALSE(test::check_native_host(wrong,*target));
  CHECK_FALSE(test::check_native_host(*host,*target,"not-a-target"));
}
