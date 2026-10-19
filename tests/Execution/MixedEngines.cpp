/* Licensed under the MIT License. One symbol image across per-definition engines. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "NativeTarget.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include "mirnext/Execution/detail/Recovery.hpp"
#include "../IR/Calls.hpp"
#include "../IR/Variables.hpp"
#include <algorithm>
#include <array>
#include <cstring>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
constexpr std::array engines{ExecutionEngine::Interpreter, ExecutionEngine::EagerJIT,
                             ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning};
auto parse (std::string_view text) {
  auto context = serialization::parse_text (test::native_hardware_text (text));
  INFO ((context ? "" : context.error ().message));
  REQUIRE (context);
  return std::move (*context);
}
ExecutionSession make (const Context &c, ExecutionOptions options) {
  auto result = ExecutionSession::create (c, std::move (options));
  INFO ((result ? "" : result.error ().message));
  REQUIRE (result);
  return std::move (*result);
}
FunctionHandle find (const ExecutionSession &s, std::string_view name,
                     std::string_view module = "m") {
  auto result = s.find_function (module, name);
  REQUIRE (result);
  return std::move (*result);
}
std::int64_t value (const FunctionHandle &f, std::span<const std::int64_t> args = {}) {
  auto result = f.call_i64 (args);
  INFO ((result ? "" : result.error ().message));
  REQUIRE (result);
  return *result;
}
}  // namespace
TEST_CASE (
  "interpreted and native definitions exchange block return-block variadic and scalar calls at "
  "O0-O3") {
  for (auto native :
       {ExecutionEngine::EagerJIT, ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning})
    for (bool interpreted_caller : {false, true})
      for (unsigned level = 0; level < 4; ++level)
        for (unsigned kind = 0; kind <= unsigned (test::CallExample::Inline); ++kind) {
          CAPTURE (native);
          CAPTURE (interpreted_caller);
          CAPTURE (level);
          CAPTURE (kind);
          auto c = test::call_example (static_cast<test::CallExample> (kind), true);
          ExecutionOptions
            options{.optimization
                    = transforms::OptimizationOptions{.level
                                                      = static_cast<transforms::OptimizationLevel> (
                                                        level)},
                    .select_engine = [=] (auto &f) -> Result<ExecutionEngine> {
                      return (f.symbol ().name () == "entry") == interpreted_caller
                               ? ExecutionEngine::Interpreter
                               : native;
                    }};
          if (kind == unsigned (test::CallExample::MultipleResults)
              && target::TargetMachine::native ()->abi () == target::ABI::X86Windows) {
            REQUIRE (c->verify ());
            auto failed = ExecutionSession::create (*c, std::move (options));
            REQUIRE_FALSE (failed);
            CHECK (failed.error ().message.find ("Windows does not support multiple return values")
                   != std::string::npos);
            continue;
          }
          auto s = make (*c, std::move (options));
          CHECK (s.mode () == ExecutionMode::Mixed);
          bool boundary = false;
          auto function = s.context ().find_module ("calls")->find_symbol ("entry")->function ();
          for (auto &block : function->blocks ())
            for (auto &i : block.instructions ())
              boundary |= i.opcode () == Opcode::Call && i.operands ()[1].get<const Register *> ();
          REQUIRE (boundary);
          CHECK (value (find (s, "entry", "calls")) == 42);
        }
}
TEST_CASE (
  "mixed calls transfer integer and floating hardware bindings before entering the other engine") {
  for (auto native :
       {ExecutionEngine::EagerJIT, ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning})
    for (bool interpreted_caller : {false, true})
      for (unsigned level = 0; level < 4; ++level) {
        CAPTURE (native);
        CAPTURE (interpreted_caller);
        CAPTURE (level);
        std::string text
          = "m:module\np:proto i64\ng:i64 0\nf:d 0.0\ncallee:func i64\nglobal "
            "i64:h:r19,d:v:v20\nlocal i64:a\nadd h,h,22\ndadd v,v,20.5\nmov a,g\nmov "
            "i64:0(a),h\nmov a,f\ndmov d:0(a),v\nret 0\nendfunc\ntarget:ref callee,0\ncaller:func "
            "i64\nglobal i64:h:r19,d:v:v20\nlocal i64:r,i64:a\nmov h,20\ndmov v,1.5\nmov "
            "a,target\nmov a,i64:0(a)\ncall p,a,r\nmov r,g\nmov r,i64:0(r)\nret "
            "r\nendfunc\nendmodule\n";
        if (target::TargetMachine::native ()->architecture () == target::Architecture::X86_64)
          for (auto at = text.find (":v20"); at != std::string::npos; at = text.find (":v20"))
            text.replace (at, 4, ":xmm3");
        auto c = parse (text);
        auto s
          = make (*c,
                  {.optimization
                   = transforms::OptimizationOptions{.level
                                                     = static_cast<transforms::OptimizationLevel> (
                                                       level)},
                   .select_engine = [=] (auto &f) -> Result<ExecutionEngine> {
                     return (f.symbol ().name () == "caller") == interpreted_caller
                              ? ExecutionEngine::Interpreter
                              : native;
                   }});
        CHECK (value (find (s, "caller")) == 42);
        auto data = s.find_data ("m", "f");
        REQUIRE (data);
        double number;
        std::memcpy (&number, data->bytes ().data (), 8);
        CHECK (number == 22.0);
        CHECK (value (find (s, "caller")) == 42);
      }
}
TEST_CASE (
  "mixed recursion shares interpreter budgets across native reentry and reports raw failures") {
  auto c = parse (
    "m:module\np:proto i64,i64:n\neven:func i64,i64:n\nlocal i64:r,i64:a\nbeq done,n,0\nsub "
    "n,n,1\nmov a,oddptr\nmov a,i64:0(a)\ncall p,a,r,n\nret r\ndone:ret 42\nendfunc\nodd:func "
    "i64,i64:n\nlocal i64:r,i64:a\nbeq done2,n,0\nsub n,n,1\nmov a,evenptr\nmov a,i64:0(a)\ncall "
    "p,a,r,n\nret r\ndone2:ret 42\nendfunc\nevenptr:ref even,0\noddptr:ref odd,0\nendmodule\n");
  for (auto native :
       {ExecutionEngine::EagerJIT, ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning})
    for (unsigned budget : {0u, 1u, 2u}) {
      CAPTURE (native);
      CAPTURE (budget);
      auto s
        = make (*c, {.instruction_limit = budget == 1 ? 12u : 0u,
                     .call_depth_limit = budget == 2 ? 4u : 0u,
                     .select_engine = [=] (auto &f) -> Result<ExecutionEngine> {
                       return f.symbol ().name () == "even" ? ExecutionEngine::Interpreter : native;
                     }});
      auto f = find (s, "odd");
      std::array<std::int64_t, 1> args{12};
      auto result = f.call_i64 (args);
      if (!budget) {
        REQUIRE (result);
        CHECK (*result == 42);
      } else {
        REQUIRE_FALSE (result);
        CHECK (result.error ().message.find (budget == 1 ? "instruction limit" : "depth limit")
               != std::string::npos);
      }
      args[0] = 0;
      CHECK (value (f, args) == 42);
      auto entry = f.native_entry ();
      REQUIRE (entry);
      ExecutionInvocation invocation;
      auto raw = reinterpret_cast<std::int64_t (*) (std::int64_t)> (entry->address ()) (12);
      if (!budget) {
        CHECK (raw == 42);
        CHECK (invocation.status ());
      } else
        CHECK_FALSE (invocation.status ());
    }
}
TEST_CASE (
  "mixed jump calls diagnose direct and indirect cross-domain continuations before invalid entry") {
  for (auto native :
       {ExecutionEngine::EagerJIT, ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning})
    for (bool indirect : {false, true})
      for (bool interpreted_caller : {false, true}) {
        CAPTURE (native);
        CAPTURE (indirect);
        CAPTURE (interpreted_caller);
        std::string text
          = "m:module\np:proto\ncallee:func\nglobal i64:continuation:r19\njret "
            "continuation\nendfunc\nslot:ref callee,0\ncaller:func i64\nglobal "
            "i64:continuation:r19\nlocal i64:a\nladdr continuation,done\n";
        text += indirect ? "mov a,slot\nmov a,i64:0(a)\njcall p,a\n" : "jcall p,callee\n";
        text += "ret -1\ndone:ret 42\nendfunc\nendmodule\n";
        auto c = parse (text);
        ExecutionOptions options{.select_engine = [=] (auto &f) -> Result<ExecutionEngine> {
          return (f.symbol ().name () == "caller") == interpreted_caller
                   ? ExecutionEngine::Interpreter
                   : native;
        }};
        auto s = ExecutionSession::create (*c, std::move (options));
        if (!indirect) {
          REQUIRE_FALSE (s);
          CHECK (s.error ().message.find ("JCALL/JRET cannot cross") != std::string::npos);
          continue;
        }
        INFO ((s ? "" : s.error ().message));
        REQUIRE (s);
        auto f = find (*s, "caller");
        auto failed = f.call_i64 ();
        REQUIRE_FALSE (failed);
        CHECK (failed.error ().message.find ("JCALL/JRET cannot cross") != std::string::npos);
        auto raw = f.native_entry ();
        REQUIRE (raw);
        // Raw MIR entries deliberately expose bound-register effects. Protect
        // the C++/ASan host frame before entering a function that binds r19/r12.
        auto recovery = execution::detail::NativeRecovery::create ();
        REQUIRE (recovery);
        auto rejected = recovery->invoke<std::int64_t> (raw->address ());
        REQUIRE_FALSE (rejected);
        CHECK (rejected.error ().message.find ("JCALL/JRET cannot cross") != std::string::npos);
      }
}
TEST_CASE (
  "per-definition engines share ordinary direct indirect and raw call contracts at O0-O3") {
  for (auto caller : engines)
    for (auto callee : engines)
      for (unsigned level = 0; level < 4; ++level) {
        CAPTURE (caller);
        CAPTURE (callee);
        CAPTURE (level);
        auto c = parse (
          "m:module\np:proto i64,i64:x\ncallee:func i64,i64:x\nlocal i64:r\nadd r,x,2\nret "
          "r\nendfunc\ntarget:ref callee,0\ncaller:func i64,i64:x\nlocal i64:r,i64:fp\nmov "
          "fp,target\nmov fp,i64:0(fp)\ncall p,fp,r,x\nret r\nendfunc\nendmodule\n");
        auto before = dump (*c);
        unsigned selections = 0;
        auto s
          = make (*c,
                  {.optimization
                   = transforms::OptimizationOptions{.level
                                                     = static_cast<transforms::OptimizationLevel> (
                                                       level)},
                   .select_engine = [&] (const Function &f) -> Result<ExecutionEngine> {
                     ++selections;
                     CHECK (
                       &f != c->find_module ("m")->find_symbol (f.symbol ().name ())->function ());
                     return f.symbol ().name () == "caller" ? caller : callee;
                   }});
        CHECK (selections == 2);
        CHECK (dump (*c) == before);
        c.reset ();
        auto a = find (s, "caller"), b = find (s, "callee");
        CHECK (a.engine () == caller);
        CHECK (b.engine () == callee);
        const bool mixed
          = (caller == ExecutionEngine::Interpreter) != (callee == ExecutionEngine::Interpreter);
        CHECK (s.mode ()
               == (mixed                                    ? ExecutionMode::Mixed
                   : caller == ExecutionEngine::Interpreter ? ExecutionMode::Interpreter
                                                            : ExecutionMode::JIT));
        if (s.interpreter_statistics ())
          CHECK (s.interpreter_statistics ()->functions
                 == unsigned (caller == ExecutionEngine::Interpreter)
                      + unsigned (callee == ExecutionEngine::Interpreter));
        CHECK (s.statistics ().functions == 2);
        CHECK (s.statistics ().compiled_functions
               == unsigned (caller == ExecutionEngine::EagerJIT)
                    + unsigned (callee == ExecutionEngine::EagerJIT));
        for (auto f : {a, b}) {
          if (f.engine () == ExecutionEngine::Interpreter)
            CHECK (f.compilation_state () == FunctionCompilationState::Interpreted);
          else if (f.engine () == ExecutionEngine::EagerJIT)
            REQUIRE (f.artifact ());
          else
            CHECK (f.compilation_state () == FunctionCompilationState::Uncompiled);
        }
        std::array<std::int64_t, 1> args{40};
        CHECK (value (a, args) == 42);
        if (callee == ExecutionEngine::LazyJIT || callee == ExecutionEngine::BlockVersioning)
          CHECK (b.compilation_state () != FunctionCompilationState::Uncompiled);
        CHECK (value (b, args) == 42);
        args[0] = 44;
        CHECK (value (a, args) == 46);
        auto count = s.statistics ().code_bytes;
        CHECK (s.statistics ().code_bytes == count);
        if (mixed || caller != ExecutionEngine::Interpreter) {
          auto entry = a.native_entry ();
          REQUIRE (entry);
          ExecutionInvocation invocation;
          CHECK (reinterpret_cast<std::int64_t (*) (std::int64_t)> (entry->address ()) (40) == 42);
          CHECK (invocation.status ());
          CHECK (a.native_entry ()->address () == entry->address ());
        }
        if (caller == ExecutionEngine::BlockVersioning) {
          REQUIRE (a.block_statistics ());
          CHECK (a.block_statistics ()->compiled_blocks > 0);
          CHECK_FALSE (a.artifact ());
        }
        if (callee == ExecutionEngine::BlockVersioning) {
          REQUIRE (b.block_statistics ());
          CHECK (b.block_statistics ()->compiled_blocks > 0);
          CHECK_FALSE (b.artifact ());
        }
      }
}
TEST_CASE (
  "engine selection is fallible runs before resolution or compilation and releases its captures") {
  auto c = parse ("m:module\nf:func i64\nret 42\nendfunc\nendmodule\n");
  unsigned observed = 0;
  for (bool invalid : {false, true}) {
    auto result
      = ExecutionSession::create (*c,
                                  {.link = {.compilation_observer = [&] (auto &) -> Result<void> {
                                     ++observed;
                                     return {};
                                   }},
                                   .select_engine = [&] (auto &) -> Result<ExecutionEngine> {
                                     if (invalid) return static_cast<ExecutionEngine> (99);
                                     return Error{ErrorCode::UnknownName, "selection failed"};
                                   }});
    REQUIRE_FALSE (result);
    CHECK (result.error ().message.find ("m/f [") != std::string::npos);
    CHECK (result.error ().message.find ("link:engine") != std::string::npos);
    CHECK (result.error ().message.find (invalid ? "invalid function execution engine"
                                                 : "selection failed")
           != std::string::npos);
    CHECK (observed == 0);
  }
  auto captured = std::make_shared<int> (42);
  std::weak_ptr<int> lifetime = captured;
  ExecutionOptions options;
  options.mode = ExecutionMode::Interpreter;
  options.select_engine = [keep = std::move (captured)] (auto &) -> Result<ExecutionEngine> {
    return ExecutionEngine::LazyJIT;
  };
  auto s = make (*c, std::move (options));
  options = {};
  CHECK (lifetime.expired ());
  CHECK (s.mode () == ExecutionMode::JIT);
  CHECK (value (find (s, "f")) == 42);
  auto invalid = ExecutionSession::create (*c, {.mode = ExecutionMode::Mixed});
  REQUIRE_FALSE (invalid);
}
TEST_CASE (
  "mixed execution preserves cross-module aliases function data globals and handle lifetime") {
  auto c = parse (
    "defs:module\nf:func i64,i64:x\nlocal i64:r\nadd r,x,2\nret r\nendfunc\nexport "
    "f\nendmodule\nm:module\nimport f\np:proto i64,i64:x\nfp:ref f,0\ng:i64 0\ncaller:func "
    "i64,i64:x\nlocal i64:r,i64:a\nmov a,fp\nmov a,i64:0(a)\ncall p,a,r,x\nmov a,g\nmov "
    "i64:0(a),r\nret r\nendfunc\nexport caller\nendmodule\n");
  std::optional<FunctionHandle> retained, alias;
  std::optional<LinkedData> global;
  {
    auto s = make (*c, {.select_engine = [] (auto &f) -> Result<ExecutionEngine> {
      return f.module ().name () == "defs" ? ExecutionEngine::Interpreter
                                           : ExecutionEngine::BlockVersioning;
    }});
    retained = find (s, "caller");
    alias = find (s, "f");
    auto definition = find (s, "f", "defs");
    CHECK (alias->engine () == definition.engine ());
    auto a = alias->native_entry (), b = definition.native_entry ();
    REQUIRE (a);
    REQUIRE (b);
    CHECK (a->address () == b->address ());
    auto g = s.find_data ("m", "g");
    REQUIRE (g);
    global = *g;
  }
  c.reset ();
  std::array<std::int64_t, 1> args{40};
  CHECK (value (*retained, args) == 42);
  CHECK (value (*alias, args) == 42);
  std::int64_t stored;
  std::memcpy (&stored, global->bytes ().data (), 8);
  CHECK (stored == 42);
}
TEST_CASE (
  "mixed expression initializers execute their selected definition once without host body copies") {
  for (auto engine : engines) {
    auto c = test::variable_example (test::VariableExample::Expression);
    unsigned bodies = 0;
    auto s
      = make (*c, {.link = {.compilation_observer = [&] (auto &event) -> Result<void> {
                     if (event.stage == codegen::CompilationStage::PrepareMachineIR
                         && event.function.module_name () != "$native-call"
                         && event.function.module_name () != "$interpreter-entry"
                         && event.function.module_name () != "$host")
                       ++bodies;
                     return {};
                   }},
                   .select_engine = [engine] (auto &f) -> Result<ExecutionEngine> {
                     return f.symbol ().name () == "entry" ? ExecutionEngine::Interpreter : engine;
                   }});
    CHECK (s.statistics ().expressions == 1);
    CHECK (value (find (s, "entry", "consumer")) == 42);
    CHECK (
      s.statistics ().functions
      == (engine == ExecutionEngine::Interpreter ? s.interpreter_statistics ()->functions : 2));
    CHECK (bodies == (engine == ExecutionEngine::Interpreter ? 0 : 1));
  }
}
TEST_CASE (
  "mixed callers resolve typed callbacks and native addresses with engine-specific precedence") {
  auto c = parse (R"(m:module
import host
p:proto i64,i64:x
interpreted:func i64,i64:x
local i64:r
call p,host,r,x
ret r
endfunc
native:func i64,i64:x
local i64:r
call p,host,r,x
ret r
endfunc
endmodule
)");
  for (auto native :
       {ExecutionEngine::EagerJIT, ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning})
    for (bool address : {false, true}) {
      CAPTURE (native);
      CAPTURE (address);
      unsigned calls = 0, exited = 0;
      bool fail = false;
      auto s
        = make (*c, {.link = {.resolve = [&] (auto &) -> Result<ExternalSymbol> {
                       auto host = +[] (std::int64_t x) { return x + 3; };
                       ExternalSymbol symbol{address ? reinterpret_cast<std::uintptr_t> (host) : 0,
                                             Signature{{Type::i64 ()}, {Type::i64 ()}},
                                             {}};
                       symbol.interpret = [&] (std::span<const ExecutionArgument> args)
                         -> Result<std::vector<ScalarValue>> {
                         struct Guard {
                           unsigned &count;
                           ~Guard () { ++count; }
                         } guard{exited};
                         ++calls;
                         if (fail) return Error{ErrorCode::InvalidArgument, "callback rejected"};
                         auto x = args[0].scalar ()->as_i64 ();
                         if (!x) return x.error ();
                         return std::vector<ScalarValue>{ScalarValue (*x + 2)};
                       };
                       return symbol;
                     }},
                     .select_engine = [=] (auto &f) -> Result<ExecutionEngine> {
                       return f.symbol ().name () == "interpreted" ? ExecutionEngine::Interpreter
                                                                   : native;
                     }});
      auto i = find (s, "interpreted"), n = find (s, "native");
      std::array<std::int64_t, 1> args{40};
      CHECK (value (i, args) == 42);
      CHECK (value (n, args) == (address ? 43 : 42));
      CHECK (calls == exited);
      CHECK (value (find (s, "host"), args) == 42);
      CHECK (calls == exited);
      if (!address) {
        fail = true;
        auto failed = n.call_i64 (args);
        REQUIRE_FALSE (failed);
        CHECK (failed.error ().message.find ("callback rejected") != std::string::npos);
        CHECK (calls == exited);
        fail = false;
        CHECK (value (n, args) == 42);
      }
    }
}
TEST_CASE ("an all-interpreted selection dispatches import handles without native services") {
  auto c = parse (
    "m:module\nimport host\np:proto i64,i64:x\nf:func i64,i64:x\nlocal i64:r\ncall p,host,r,x\nret "
    "r\nendfunc\nendmodule\n");
  for (bool typed : {false, true}) {
    auto s
      = make (*c, {.link = {.resolve = [=] (auto &) -> Result<ExternalSymbol> {
                     auto native = +[] (std::int64_t x) { return x + 2; };
                     ExternalSymbol symbol{typed ? 0 : reinterpret_cast<std::uintptr_t> (native),
                                           Signature{{Type::i64 ()}, {Type::i64 ()}},
                                           {}};
                     if (typed)
                       symbol.interpret = [] (auto args) -> Result<std::vector<ScalarValue>> {
                         return std::vector<ScalarValue>{
                           ScalarValue (*args[0].scalar ()->as_i64 () + 2)};
                       };
                     return symbol;
                   }},
                   .native_calls = false,
                   .select_engine = [] (auto &) -> Result<ExecutionEngine> {
                     return ExecutionEngine::Interpreter;
                   }});
    CHECK (s.mode () == ExecutionMode::Interpreter);
    std::array<std::int64_t, 1> args{40};
    for (auto f : {find (s, "host"), find (s, "f")}) {
      CHECK (f.mode () == ExecutionMode::Interpreter);
      if (typed)
        CHECK (value (f, args) == 42);
      else {
        auto failed = f.call_i64 (args);
        REQUIRE_FALSE (failed);
        CHECK (failed.error ().message.find ("native foreign calls are disabled")
               != std::string::npos);
      }
    }
    CHECK (s.host_entry_count () == 0);
    CHECK (s.statistics ().code_bytes == 0);
  }
}
TEST_CASE ("same-domain jump calls retain continuations and label relocations in mixed programs") {
  for (auto caller : engines)
    for (auto callee : engines) {
      if ((caller == ExecutionEngine::Interpreter) != (callee == ExecutionEngine::Interpreter))
        continue;
      for (bool indirect : {false, true})
        for (unsigned level = 0; level < 4; ++level) {
          CAPTURE (caller);
          CAPTURE (callee);
          CAPTURE (indirect);
          CAPTURE (level);
          std::string text
            = "m:module\np:proto\ncallee:func\nglobal i64:continuation:r19\njret "
              "continuation\nendfunc\nslot:ref callee,0\ncaller:func i64\nglobal "
              "i64:continuation:r19\nlocal i64:a\nstart:laddr continuation,done\n";
          text += indirect ? "mov a,slot\nmov a,i64:0(a)\njcall p,a\n" : "jcall p,callee\n";
          text += "ret -1\ndone:ret 42\nendfunc\naddress:lref done,0\nshifted:lref done,3\nbase:lref start,0\ndifference:lref done,start,-4\nother:func i64\nret 7\nendfunc\nendmodule\n";
          auto c = parse (text);
          auto s
            = make (*c,
                    {.optimization
                     = transforms::OptimizationOptions{.level = static_cast<
                                                         transforms::OptimizationLevel> (level)},
                     .select_engine = [=] (auto &f) -> Result<ExecutionEngine> {
                       if (f.symbol ().name () == "caller") return caller;
                       if (f.symbol ().name () == "callee") return callee;
                       return caller == ExecutionEngine::Interpreter ? ExecutionEngine::LazyJIT
                                                                     : ExecutionEngine::Interpreter;
                     }});
          CHECK (s.mode () == ExecutionMode::Mixed);
          CHECK (value (find (s, "caller")) == 42);
          auto read = [&] (std::string_view name) {
            auto data = s.find_data ("m", name);
            REQUIRE (data);
            std::uint64_t bits;
            std::memcpy (&bits, data->bytes ().data (), 8);
            return bits;
          };
          auto address = read ("address"), base = read ("base");
          CHECK (read ("shifted") == address + 3);
          CHECK (read ("difference") == address - base - 4);
          CHECK (value (find (s, "caller")) == 42);
        }
    }
}
TEST_CASE ("mixed ABI entries preserve all five by-value block classes") {
  for (unsigned category = 0; category < 5; ++category) {
    Context c;
    auto m = c.create_module ("m");
    REQUIRE (m);
    auto abi = static_cast<Type::BlockClass> (category);
    auto callee = (*m)->create_function ("callee", {{Type::i64 ()}, {Type::block (16, abi)}});
    REQUIRE (callee);
    auto body = (*callee)->create_block ();
    REQUIRE (body);
    IRBuilder b (**body);
    auto loaded = b.load (Memory{Type::i64 (), (*callee)->arguments ()[0]});
    REQUIRE (loaded);
    REQUIRE (b.store (Memory{Type::i64 (), (*callee)->arguments ()[0]}, 99));
    REQUIRE (b.ret (Operand (**loaded)));
    auto pointer
      = (*m)->create_variable ("pointer", Type::pointer (), Operand ((*callee)->symbol ()));
    REQUIRE (pointer);
    auto caller = (*m)->create_function ("caller", {{Type::i64 ()}, {}});
    REQUIRE (caller);
    auto entry = (*caller)->create_block ();
    REQUIRE (entry);
    IRBuilder a (**entry);
    auto storage = a.allocate_stack (std::uint64_t{16});
    REQUIRE (storage);
    REQUIRE (a.store (Memory{Type::i64 (), *storage}, 42));
    REQUIRE (a.store (Memory{Type::i64 (), *storage, nullptr, 8}, 7));
    auto target = a.load (**pointer);
    REQUIRE (target);
    auto called = a.call_indirect ((*callee)->symbol (), Operand (**target),
                                   {BlockArgument (**storage, 16, abi)});
    REQUIRE (called);
    auto original = a.load (Memory{Type::i64 (), *storage});
    REQUIRE (original);
    auto result = called->value ();
    REQUIRE (result);
    auto total = a.binary (Opcode::Add, Operand (**original), Operand (**result));
    REQUIRE (total);
    REQUIRE (a.ret (Operand (**total)));
    for (auto native :
         {ExecutionEngine::EagerJIT, ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning})
      for (bool interpreted_caller : {false, true})
        for (unsigned level = 0; level < 4; ++level) {
          CAPTURE (category);
          CAPTURE (native);
          CAPTURE (interpreted_caller);
          CAPTURE (level);
          auto s
            = make (c, {.optimization
                        = transforms::OptimizationOptions{.level = static_cast<
                                                            transforms::OptimizationLevel> (level)},
                        .select_engine = [=] (auto &f) -> Result<ExecutionEngine> {
                          return (f.symbol ().name () == "caller") == interpreted_caller
                                   ? ExecutionEngine::Interpreter
                                   : native;
                        }});
          CHECK (value (find (s, "caller")) == 84);
        }
  }
}
TEST_CASE ("seeded mixed-engine loops match unoptimized interpretation and global writes") {
  auto c = parse (R"(m:module
p:proto i64,i64:x
step:func i64,i64:x
local i64:r
lsh r,x,2
xor r,r,x
add r,r,17
ret r
endfunc
pointer:ref step,0
saved:i64 0
caller:func i64,i64:x
local i64:n,i64:total,i64:a,i64:r
and n,x,15
mov total,0
loop:beq done,n,0
mov a,pointer
mov a,i64:0(a)
call p,a,r,n
add total,total,r
sub n,n,1
jmp loop
done:mov a,saved
mov i64:0(a),total
add total,total,x
ret total
endfunc
endmodule
)");
  auto baseline = make (*c, {.mode = ExecutionMode::Interpreter});
  auto original = find (baseline, "caller");
  for (auto native :
       {ExecutionEngine::EagerJIT, ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning})
    for (bool interpreted_caller : {false, true})
      for (unsigned level = 0; level < 4; ++level) {
        CAPTURE (native);
        CAPTURE (interpreted_caller);
        CAPTURE (level);
        auto s
          = make (*c,
                  {.optimization
                   = transforms::OptimizationOptions{.level
                                                     = static_cast<transforms::OptimizationLevel> (
                                                       level)},
                   .select_engine = [=] (auto &f) -> Result<ExecutionEngine> {
                     return (f.symbol ().name () == "caller") == interpreted_caller
                              ? ExecutionEngine::Interpreter
                              : native;
                   }});
        auto f = find (s, "caller");
        auto expected = baseline.find_data ("m", "saved"), actual = s.find_data ("m", "saved");
        REQUIRE (expected);
        REQUIRE (actual);
        std::uint32_t seed = 0x193ab42;
        for (unsigned trial = 0; trial < 24; ++trial) {
          seed = seed * 1664525 + 1013904223;
          std::array<std::int64_t, 1> args{std::int64_t (seed % 10000)};
          CHECK (value (f, args) == value (original, args));
          CHECK (std::ranges::equal (expected->bytes (), actual->bytes ()));
        }
      }
}
TEST_CASE (
  "all four engines retain one symbol image through consumed products and repeated sessions") {
  auto c = parse (R"(m:module
p:proto i64,i64:x
interpreted:func i64,i64:x
local i64:r
add r,x,1
ret r
endfunc
ip:ref interpreted,0
block:func i64,i64:x
local i64:r,i64:a
mov a,ip
mov a,i64:0(a)
call p,a,r,x
add r,r,1
ret r
endfunc
bp:ref block,0
lazy:func i64,i64:x
local i64:r,i64:a
mov a,bp
mov a,i64:0(a)
call p,a,r,x
add r,r,1
ret r
endfunc
lp:ref lazy,0
eager:func i64,i64:x
local i64:r,i64:a
mov a,lp
mov a,i64:0(a)
call p,a,r,x
add r,r,1
ret r
endfunc
endmodule
)");
  auto select = [] (const Function &f) -> Result<ExecutionEngine> {
    if (f.symbol ().name () == "interpreted") return ExecutionEngine::Interpreter;
    if (f.symbol ().name () == "block") return ExecutionEngine::BlockVersioning;
    if (f.symbol ().name () == "lazy") return ExecutionEngine::LazyJIT;
    return ExecutionEngine::EagerJIT;
  };
  auto program = transforms::optimize (*c);
  REQUIRE (program);
  auto loaded = ExecutionSession::create (std::move (*program), {.select_engine = select});
  REQUIRE (loaded);
  CHECK_FALSE (program->verify ());
  auto second = make (*c, {.select_engine = select});
  c.reset ();
  for (auto *s : {&*loaded, &second}) {
    CHECK (s->mode () == ExecutionMode::Mixed);
    CHECK (s->statistics ().functions == 4);
    CHECK (s->statistics ().compiled_functions == 1);
    REQUIRE (s->interpreter_statistics ());
    CHECK (s->interpreter_statistics ()->functions == 1);
    auto lazy = find (*s, "lazy"), blocks = find (*s, "block");
    CHECK (lazy.compilation_state () == FunctionCompilationState::Uncompiled);
    REQUIRE (blocks.block_statistics ());
    CHECK (blocks.block_statistics ()->compiled_blocks == 0);
    auto f = find (*s, "eager");
    std::array<std::int64_t, 1> args{38};
    CHECK (value (f, args) == 42);
    CHECK (s->statistics ().compiled_functions == 3);
    REQUIRE (lazy.artifact ());
    CHECK (blocks.block_statistics ()->compiled_blocks > 0);
    args[0] = 40;
    CHECK (value (f, args) == 44);
    CHECK (s->statistics ().compiled_functions == 3);
  }
  CHECK_FALSE (loaded->owns (find (second, "eager")));
}
TEST_CASE ("mixed programs reject callback-only jump imports before publishing virtual addresses") {
  auto c = parse (
    "m:module\nimport host\np:proto\ninterpreted:func i64\njcall p,host\nret "
    "0\nendfunc\nnative:func i64\nret 7\nendfunc\nendmodule\n");
  for (auto native :
       {ExecutionEngine::EagerJIT, ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning}) {
    auto s
      = ExecutionSession::create (*c, {.link = {.resolve = [] (auto &) -> Result<ExternalSymbol> {
                                         ExternalSymbol symbol{0, Signature{}, {}, true};
                                         symbol.interpret
                                           = [] (auto) -> Result<std::vector<ScalarValue>> {
                                           return std::vector<ScalarValue>{};
                                         };
                                         return symbol;
                                       }},
                                       .select_engine = [=] (auto &f) -> Result<ExecutionEngine> {
                                         return f.symbol ().name () == "interpreted"
                                                  ? ExecutionEngine::Interpreter
                                                  : native;
                                       }});
    REQUIRE_FALSE (s);
    CHECK (
      s.error ().message.find ("callback-only jump-return import needs a native ABI implementation")
      != std::string::npos);
  }
}
