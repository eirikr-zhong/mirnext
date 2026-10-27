/* Licensed under the MIT License. Loop, overflow and addressed-label regression fixtures. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <fstream>
#include <iterator>
using namespace mirnext;
using namespace mirnext::execution;

TEST_CASE (
  "retained MIR loop overflow and addressed-label regressions run through every native engine") {
  for (auto name : {"loop.mir", "multiply-overflow-identity.mir", "empty-addressed-label.mir"}) {
    CAPTURE (name);
    auto path = std::string (MIRNEXT_TEST_SOURCE_DIR) + "/tests/Fixtures/" + name;
    std::ifstream file (path);
    REQUIRE (file);
    std::string text ((std::istreambuf_iterator<char> (file)), {});
    auto context = serialization::parse_text (text);
    REQUIRE (context);
    for (unsigned level = 0; level != 4; ++level)
      for (unsigned engine = 0; engine != 5; ++engine) {
        if (!engine && level) continue;
        CAPTURE (level);
        CAPTURE (engine);
        ExecutionOptions options;
        if (engine < 2) options.mode = ExecutionMode::Interpreter;
        if (engine)
          options.optimization = transforms::OptimizationOptions{
            .level = static_cast<transforms::OptimizationLevel> (level)};
        if (engine == 3) options.link.compilation = JITCompilation::Lazy;
        if (engine == 4) options.link.compilation = JITCompilation::BlockVersioning;
        auto session = ExecutionSession::create (**context, options);
        INFO ((session ? "" : session.error ().message));
        REQUIRE (session);
        auto function = session->find_function ((*context)->modules ().begin ()->name (), "entry");
        REQUIRE (function);
        for (unsigned repeat = 0; repeat != 2; ++repeat) {
          auto result = function->call ();
          INFO ((result ? "" : result.error ().message));
          REQUIRE (result);
          REQUIRE (result->size () == 1);
          auto value = result->front ().as_i64 ();
          REQUIRE (value);
          CHECK (*value == 42);
        }
      }
  }
}

#if defined(__aarch64__) && defined(__linux__)
namespace {
__attribute__ ((noinline)) void clobber_vector_upper_halves () {
  // AAPCS64 requires the compiler to save d8-d15, but permits these writes
  // to destroy their upper halves. This also reproduces with Debug helpers.
  asm volatile (
    "movi v8.16b, #0\n\t"
    "movi v9.16b, #0\n\t"
    "movi v10.16b, #0\n\t"
    "movi v11.16b, #0\n\t"
    "movi v12.16b, #0\n\t"
    "movi v13.16b, #0\n\t"
    "movi v14.16b, #0\n\t"
    "movi v15.16b, #0"
    :
    :
    : "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15");
}
}  // namespace
TEST_CASE ("quad precision values survive calls that preserve only SIMD lower halves") {
  auto context = serialization::parse_text (
    "m:module\nimport clobber\np:proto\nf:func ld,ld:x\ncall p,clobber\nret "
    "x\nendfunc\nendmodule\n");
  REQUIRE (context);
  for (unsigned level = 0; level < 4; ++level)
    for (auto engine :
         {JITCompilation::Eager, JITCompilation::Lazy, JITCompilation::BlockVersioning}) {
      CAPTURE (level);
      CAPTURE (static_cast<unsigned> (engine));
      ExecutionOptions options;
      options.optimization = transforms::OptimizationOptions{
        .level = static_cast<transforms::OptimizationLevel> (level)};
      options.link.compilation = engine;
      options.link.resolve = [] (const ImportRequest &) -> Result<ExternalSymbol> {
        return ExternalSymbol{reinterpret_cast<std::uintptr_t> (&clobber_vector_upper_halves),
                              ir::Signature{}};
      };
      auto session = ExecutionSession::create (**context, options);
      REQUIRE (session);
      auto function = session->find_function ("m", "f");
      REQUIRE (function);
      for (long double expected : {1.25L, -10.333333333333333333333333333333L}) {
        std::array arguments{ExecutionArgument (expected)};
        auto result = function->call (arguments);
        INFO ((result ? "" : result.error ().message));
        REQUIRE (result);
        REQUIRE (result->size () == 1);
        auto actual = result->front ().as_long_double ();
        REQUIRE (actual);
        CHECK (*actual == expected);
      }
    }
}
#endif
