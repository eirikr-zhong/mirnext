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
