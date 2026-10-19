/* Licensed under the MIT License. Native semantic coverage survives C-oracle retirement. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Execution/Session.hpp"
#include "SeededPrograms.hpp"
#include <algorithm>
using namespace mirnext; using namespace mirnext::execution;
TEST_CASE("explicit hardware copies have addressable storage independent of call side effects") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  std::string binding = target->architecture() == target::Architecture::AArch64 ? "r19" : "r12";
  for (auto address : {"addr", "addr8", "addr16", "addr32"}) {
    CAPTURE(address);
    auto invalid = serialization::parse_text("m:module\nentry:func i64\nglobal i64:bound:"+binding
      +"\nlocal p:slot\n"+address+" slot,bound\nret bound\nendfunc\nendmodule\n");
    REQUIRE_FALSE(invalid); CHECK(invalid.error().message.find("copy it to an ordinary local") != std::string::npos);
    auto parsed = serialization::parse_text("m:module\ngetter:func i64\nglobal i64:bound:"+binding
      +"\nret bound\nendfunc\np:proto i64\nentry:func i64,p:out,i64:input\nglobal i64:bound:"+binding
      +"\nlocal i64:copy,p:slot,p:callee,i64:r\nmov bound,input\nmov copy,bound\n"+address
      +" slot,copy\nmov i32:(slot),1\nmov callee,getter\ncall p,callee,r\nmov i64:(out),copy\nret r\nendfunc\nendmodule\n");
    INFO((parsed ? "" : parsed.error().message)); REQUIRE(parsed);
    for (unsigned level = 0; level < 4; ++level) for (unsigned engine = 0; engine < 7; ++engine) {
      if (engine == 0 && level) continue;
      CAPTURE(level); CAPTURE(engine); ExecutionOptions options;
      if (engine < 2) options.mode = ExecutionMode::Interpreter;
      if (engine) options.optimization = transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)};
      if (engine == 3) options.link.compilation = JITCompilation::Lazy;
      if (engine == 4) options.link.compilation = JITCompilation::BlockVersioning;
      if (engine >= 5) options.select_engine = [engine](const ir::Function &f) -> Result<ExecutionEngine> {
        return (f.symbol().name() == "entry") == (engine == 5) ? ExecutionEngine::Interpreter : ExecutionEngine::BlockVersioning;
      };
      auto session = ExecutionSession::create(**parsed, std::move(options)); INFO((session ? "" : session.error().message)); REQUIRE(session);
      auto entry = session->find_function("m", "entry"); REQUIRE(entry);
      for (auto input : {UINT64_C(0x123456789abcdef0), UINT64_C(0xfedcba9876543210)}) for (unsigned repeat = 0; repeat < 2; ++repeat) {
        std::uint64_t memory = 0;
        std::array arguments{ExecutionArgument(ScalarValue::pointer(reinterpret_cast<std::uintptr_t>(&memory))), ExecutionArgument(input)};
        auto result = entry->call(arguments); INFO((result ? "" : result.error().message)); REQUIRE(result); REQUIRE(result->size() == 1);
        auto value = (*result)[0].as_u64(); REQUIRE(value); CHECK(*value == input);
        CHECK(memory == ((input & UINT64_C(0xffffffff00000000)) | 1));
      }
    }
  }
}
TEST_CASE("seeded aliasing loops overflow local addresses and indirect calls agree across native engines") {
  auto target = target::TargetMachine::native(); REQUIRE(target);
  auto fixtures = test::seeded_programs(target->architecture() == target::Architecture::AArch64 ? "r19" : "r12");
  for (auto &fixture : fixtures) {
    CAPTURE(fixture.seed); INFO(fixture.text);
    auto parsed = serialization::parse_text(fixture.text); INFO((parsed ? "" : parsed.error().message)); REQUIRE(parsed);
    auto original = ir::dump(**parsed);
    for (unsigned level = 0; level < 4; ++level) {
      std::unique_ptr<ir::Context> decoded;
      if (level == 1) {
        auto text = serialization::write_text(**parsed); REQUIRE(text);
        auto read = serialization::parse_text(*text); REQUIRE(read); decoded = std::move(*read);
      } else if (level >= 2) {
        auto encoding = level == 2 ? serialization::BinaryEncoding::Raw : serialization::BinaryEncoding::Compressed;
        auto binary = serialization::write_binary(**parsed,encoding); REQUIRE(binary);
        auto read = serialization::read_binary(*binary,{encoding}); REQUIRE(read); decoded = std::move(*read);
      }
      auto &source = decoded ? *decoded : **parsed;
      for (unsigned engine = 0; engine < 7; ++engine) {
        CAPTURE(level); CAPTURE(engine);
        // The raw interpreter does not run optimization and needs only one iteration.
        if (engine == 0 && level != 0) continue;
        ExecutionOptions options;
        if (engine < 2) options.mode = ExecutionMode::Interpreter;
        if (engine != 0) options.optimization = transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)};
        if (engine == 3) options.link.compilation = JITCompilation::Lazy;
        if (engine == 4) options.link.compilation = JITCompilation::BlockVersioning;
        if (engine >= 5) options.select_engine = [engine](const ir::Function &f) -> Result<ExecutionEngine> {
          const bool entry = f.symbol().name() == "entry";
          return entry == (engine == 5) ? ExecutionEngine::Interpreter : ExecutionEngine::BlockVersioning;
        };
        auto loaded = ExecutionSession::create(source,std::move(options)); INFO((loaded ? "" : loaded.error().message)); REQUIRE(loaded);
        auto entry = loaded->find_function("main","entry"); REQUIRE(entry);
        for (auto input : {std::uint64_t{0},UINT64_C(0x7fffffffffffffff),UINT64_C(0x8000000000000000)}) for (unsigned alias : {0,2}) {
          CAPTURE(input); CAPTURE(alias);
          // Guard words make writes outside the declared memory visible too.
          std::array<std::uint64_t,10> memory; memory.front() = 0x123456789abcdef0ULL; memory.back() = ~memory.front();
          auto expected = fixture.initial_memory(); std::copy(expected.begin(),expected.end(),memory.begin()+1);
          for (unsigned repeat = 0; repeat < 2; ++repeat) {
            CAPTURE(repeat);
            const auto result = fixture.evaluate(input,expected,alias);
            std::array arguments{ExecutionArgument(input),
              ExecutionArgument(ScalarValue::pointer(reinterpret_cast<std::uintptr_t>(memory.data()+1))),
              ExecutionArgument(ScalarValue::pointer(reinterpret_cast<std::uintptr_t>(memory.data()+1+alias)))};
            auto actual = entry->call(arguments); INFO((actual ? "" : actual.error().message)); REQUIRE(actual); REQUIRE(actual->size() == 1);
            auto value = (*actual)[0].as_u64(); REQUIRE(value); CHECK(*value == result);
            CHECK(std::equal(expected.begin(),expected.end(),memory.begin()+1));
            CHECK(memory.front() == 0x123456789abcdef0ULL); CHECK(memory.back() == ~memory.front());
          }
        }
        CHECK(ir::dump(**parsed) == original);
      }
    }
  }
}
TEST_CASE("partial local writes preserve high bytes through natural and mixed-width address views") {
  for (auto type : {"i8","u8","i16","u16","i32","u32"}) for (bool mixed : {false,true}) for (bool direct : {false,true}) {
    CAPTURE(type); CAPTURE(mixed); CAPTURE(direct); std::string_view name(type);
    unsigned width = name.ends_with("8") ? 8 : name.ends_with("16") ? 16 : 32;
    auto narrow = width == 8 ? "addr8" : width == 16 ? "addr16" : "addr32";
    auto source = serialization::parse_text("m:module\nf:func i64\nlocal i64:v,p:p,p:q,i64:r\n"
      "mov v,1311768467463790320\n"+std::string(mixed ? narrow : "addr")+" p,v\n"+(direct ? "" : "addr q,v\n")+"mov "+type
      +":(p),1\n"+(direct ? "mov r,v\n" : "mov r,i64:(q)\n")+"ret r\nendfunc\nendmodule\n"); REQUIRE(source);
    auto expected = (UINT64_C(0x123456789abcdef0)&~((UINT64_C(1)<<width)-1))|1;
    for (unsigned level = 0; level < 4; ++level) for (unsigned engine = 0; engine < 3; ++engine) {
      CAPTURE(level); CAPTURE(engine); ExecutionOptions options;
      options.optimization = transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)};
      if (engine == 0) options.mode = ExecutionMode::Interpreter;
      if (engine == 2) options.link.compilation = JITCompilation::BlockVersioning;
      auto loaded = ExecutionSession::create(**source,std::move(options)); INFO((loaded ? "" : loaded.error().message)); REQUIRE(loaded);
      auto f = loaded->find_function("m","f"); REQUIRE(f); auto result = f->call_u64(); REQUIRE(result); CHECK(*result == expected);
    }
  }
}
