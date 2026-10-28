/* Licensed under the MIT License. Platform runtime acceptance tests. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "NativeHost.hpp"
#ifdef MIRNEXT_TEST_IDENTITY_HEADER
#include MIRNEXT_TEST_IDENTITY_HEADER
#endif
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
namespace {
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
Error invalid (std::string message) { return {ErrorCode::InvalidArgument, std::move (message)}; }
struct Scene {
  std::string mode;
  unsigned level;
  std::size_t checked_calls = 0, raw_calls = 0, preparations = 0, legalizations = 0, frames = 0,
              encoded_blocks = 0;
  std::size_t adapter_preparations = 0, adapter_legalizations = 0, adapter_frames = 0;
  std::uint64_t fingerprint = 0;
};
struct Report {
  std::string status = "running", diagnostic, target, expected, linkage;
  test::NativeHost host;
  std::vector<Scene> scenes;
};
void quoted (std::ostream &out, std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\')
      out << '\\' << char (c);
    else if (c < 32)
      out << "\\u00" << hex[c >> 4] << hex[c & 15];
    else
      out << char (c);
  }
  out << '"';
}
void json (std::ostream &out, const Report &r) {
  out << "{\n  \"schema\": 3, \"status\": ";
  quoted (out, r.status);
  out << ",\n  \"source_fingerprint\": \"" << MIRNEXT_TEST_SOURCE_FINGERPRINT
      << "\", \"configuration\": \"" << MIRNEXT_TEST_CONFIGURATION << "\", \"linkage\": ";
  quoted (out, r.linkage);
  out << ",\n  \"target\": ";
  quoted (out, r.target);
  out << ", \"expected_target\": ";
  quoted (out, r.expected);
  out << ",\n  \"host_system\": ";
  quoted (out, r.host.system);
  out << ", \"host_architecture\": ";
  quoted (out, r.host.architecture);
  out << ", \"translated\": " << (r.host.translated ? "true" : "false");
  out << ",\n  \"diagnostic\": ";
  quoted (out, r.diagnostic);
  out << ",\n  \"completed_scenes\": " << r.scenes.size () << ",\n  \"scenes\": [";
  bool first = true;
  for (auto &s : r.scenes) {
    if (!first) out << ',';
    first = false;
    out << "\n    {\"mode\": ";
    quoted (out, s.mode);
    out << ", \"level\": " << s.level << ", \"checked_calls\": " << s.checked_calls
        << ", \"raw_calls\": " << s.raw_calls << ", \"preparations\": " << s.preparations
        << ", \"legalizations\": " << s.legalizations << ", \"frames\": " << s.frames
        << ", \"encoded_blocks\": " << s.encoded_blocks
        << ", \"adapter_preparations\": " << s.adapter_preparations
        << ", \"adapter_legalizations\": " << s.adapter_legalizations
        << ", \"adapter_frames\": " << s.adapter_frames
        << ", \"input_fingerprint\": " << s.fingerprint << '}';
  }
  out << "\n  ]\n}\n";
}
Result<void> save (std::string_view path, const Report &report) {
  if (path.empty ()) return {};
  std::ofstream out (std::string (path), std::ios::trunc);
  json (out, report);
  out.close ();
  if (!out) return invalid ("unable to write native acceptance report");
  return {};
}
std::int64_t host_add (std::int64_t value) { return value + 5; }
Result<Program> fixture () {
  auto c = std::make_unique<Program> ();
#define GET(name, expression) \
  auto name = (expression);   \
  if (!name.has_value ()) return name.error ()
#define CHECK(expression)                               \
  do {                                                  \
    auto checked = (expression);                        \
    if (!checked.has_value ()) return checked.error (); \
  } while (false)
  GET (provider, mirnext::ir::detail::access (*(c)).create_module ("provider"));
  const Signature signature{{Type::i64 ()}, {Type::i64 ()}};
  GET (scale,
       mirnext::ir::detail::access (((provider)).value ()).create_function ("scale", signature));
  GET (scale_body, mirnext::ir::detail::access (((scale)).value ()).create_block ());
  InstructionBuilder sb ((scale_body).value ());
  CHECK (sb.ret (
    sb.expression (*mirnext::ir::detail::ReferenceAccess::pointers (scale.value ().arguments ())[0])
    * 3));
  CHECK (
    mirnext::ir::detail::access (((provider)).value ()).export_symbol (scale.value ().symbol ()));
  GET (module, mirnext::ir::detail::access (*(c)).create_module ("application"));
  GET (import, mirnext::ir::detail::access (((module)).value ()).import ("scale", signature));
  GET (prototype,
       mirnext::ir::detail::access (((module)).value ()).prototype ("scale_signature", signature));
  GET (host, mirnext::ir::detail::access (((module)).value ()).import ("host_add", signature));
  GET (total, mirnext::ir::detail::access (((module)).value ())
                .create_variable ("total", Type::i64 (), 0));
  GET (function,
       mirnext::ir::detail::access (((module)).value ()).create_function ("entry", signature));
  GET (entry, mirnext::ir::detail::access (((function)).value ()).create_block ());
  GET (header, mirnext::ir::detail::access (((function)).value ()).create_block ());
  GET (body, mirnext::ir::detail::access (((function)).value ()).create_block ());
  GET (exit, mirnext::ir::detail::access (((function)).value ()).create_block ());
  GET (loop, mirnext::ir::detail::access (((function)).value ()).create_label ((header).value ()));
  GET (done, mirnext::ir::detail::access (((function)).value ()).create_label ((exit).value ()));
  GET (index, mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ()));
  GET (sum, mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ()));
  InstructionBuilder begin ((entry).value ());
  CHECK (begin.assign ((index).value (), Operand (0)));
  CHECK (begin.assign ((sum).value (), Operand (0)));
  InstructionBuilder condition ((header).value ());
  CHECK (condition.branch (Comparison::GreaterEqual, condition.expression ((index).value ()),
                           condition.expression (*mirnext::ir::detail::ReferenceAccess::pointers (
                             function.value ().arguments ())[0]),
                           (done).value ()));
  InstructionBuilder step ((body).value ());
  GET (address, step.address_of ((import).value ()));
  GET (call, step.call_indirect ((prototype).value (), Operand ((address).value ()),
                                 {Operand ((index).value ())}));
  GET (value, call.value ().value ());
  CHECK (step.assign ((sum).value (),
                      step.expression ((sum).value ()) + step.expression ((value).value ())));
  CHECK (step.assign ((index).value (), step.expression ((index).value ()) + 1));
  CHECK (step.jump ((loop).value ()));
  InstructionBuilder end ((exit).value ());
  CHECK (end.store ((total).value (), Operand ((sum).value ())));
  GET (result, end.call ((host).value (), {Operand ((sum).value ())}));
  GET (answer, result.value ().value ());
  CHECK (end.ret (Operand ((answer).value ())));
  CHECK (c->verify ());
  return std::move (*c);
}
Result<Scene> execute (std::string mode, unsigned level, std::string_view target) {
  Scene scene{std::move (mode), level};
  GET (source, fixture ());
  auto original = dump ((source).value ());
  GET (binary, serialization::write_binary ((source).value ()));
  scene.fingerprint = 14695981039346656037ULL;
  for (auto byte : (binary).value ())
    scene.fingerprint
      = (scene.fingerprint ^ std::to_integer<unsigned char> (byte)) * 1099511628211ULL;
  auto interpreted = scene.mode == "original-interpreter" || scene.mode == "optimized-interpreter";
  ExecutionOptions options;
  if (scene.mode != "original-interpreter")
    options.optimization = transforms::OptimizationOptions{
      .level = static_cast<transforms::OptimizationLevel> (level)};
  if (interpreted) {
    options.mode = ExecutionMode::Interpreter;
    options.native_entries = true;
  }
  if (scene.mode == "lazy" || scene.mode == "mixed")
    options.link.compilation = JITCompilation::Lazy;
  if (scene.mode == "bbv") options.link.compilation = JITCompilation::BlockVersioning;
  if (scene.mode == "mixed")
    options.select_engine = [] (const Function &f) -> Result<ExecutionEngine> {
      return f.symbol ().name () == "scale" ? ExecutionEngine::Interpreter
                                            : ExecutionEngine::LazyJIT;
    };
  options.link.resolve = [] (const ImportRequest &request) -> Result<ExternalSymbol> {
    if (request.name != "host_add") return invalid ("unexpected acceptance import");
    ExternalSymbol external;
    external.address = reinterpret_cast<std::uintptr_t> (&host_add);
    external.signature = Signature{{Type::i64 ()}, {Type::i64 ()}};
    external.interpret
      = [] (std::span<const ExecutionArgument> args) -> Result<std::vector<ScalarValue>> {
      if (args.size () != 1 || !args[0].scalar ().get ())
        return invalid ("invalid acceptance callback arguments");
      GET (value, args[0].scalar ().value ().as_i64 ());
      return std::vector<ScalarValue>{ScalarValue (host_add ((value).value ()))};
    };
    return external;
  };
  options.link.compilation_observer = [&] (const codegen::CompilationSnapshot &s) -> Result<void> {
    if (s.function.target ().name () != target) return invalid ("machine pipeline target mismatch");
    bool body
      = s.function.module_name () == "application" || s.function.module_name () == "provider";
    (body ? scene.preparations : scene.adapter_preparations)
      += s.stage == codegen::CompilationStage::PrepareMachineIR;
    (body ? scene.legalizations : scene.adapter_legalizations)
      += s.stage == codegen::CompilationStage::LegalizeOperands;
    (body ? scene.frames : scene.adapter_frames) += s.stage == codegen::CompilationStage::Frame;
    return {};
  };
  options.link.block_versioning.observer = [&] (const BlockCompilationSnapshot &) -> Result<void> {
    ++scene.encoded_blocks;
    return {};
  };
  struct Handles {
    FunctionHandle function;
    mirnext::execution::DataHandle data;
    NativeEntry native;
  };
  auto load = [&] () -> Result<Handles> {
    GET (session, ExecutionSession::create ((source).value (), std::move (options)));
    if (dump ((source).value ()) != original) return invalid ("session mutated source IR");
    GET (function, session.value ().find_function ("application", "entry"));
    GET (data, session.value ().find_data ("application", "total"));
    GET (native, session.value ().native_entry ((function).value ()));
    auto expected = interpreted             ? ExecutionEngine::Interpreter
                    : scene.mode == "bbv"   ? ExecutionEngine::BlockVersioning
                    : scene.mode == "eager" ? ExecutionEngine::EagerJIT
                                            : ExecutionEngine::LazyJIT;
    if (function.value ().engine () != expected)
      return invalid ("execution engine selection mismatch");
    GET (helper, session.value ().find_function ("provider", "scale"));
    if (helper.value ().engine ()
        != (scene.mode == "mixed" ? ExecutionEngine::Interpreter : expected))
      return invalid ("callee engine selection mismatch");
    return Handles{std::move ((function).value ()), std::move ((data).value ()),
                   std::move ((native).value ())};
  };
  GET (handles, load ());
  (source).value () = ir::Program{};  // Handles alone now own all IR, data and code.
  auto check_data = [&] (std::int64_t n) -> Result<void> {
    auto bytes = handles.value ().data.bytes ();
    if (bytes.size () != sizeof (std::int64_t))
      return invalid ("acceptance global layout mismatch");
    std::int64_t actual;
    std::memcpy (&actual, bytes.data (), sizeof actual);
    if (actual != 3 * n * (n - 1) / 2) return invalid ("acceptance global write mismatch");
    return {};
  };
  for (std::int64_t n : {4, 0, 9, 2, 4}) {
    std::array args{n};
    GET (value, handles.value ().function.call_i64 (args));
    if ((value).value () != 3 * n * (n - 1) / 2 + 5)
      return invalid ("acceptance checked call result mismatch");
    CHECK (check_data (n));
    ++scene.checked_calls;
  }
  ExecutionInvocation invocation;
  auto raw = reinterpret_cast<std::int64_t (*) (std::int64_t)> (handles.value ().native.address ());
  if (raw (3) != 14) return invalid ("acceptance native ABI result mismatch");
  CHECK (invocation.status ());
  CHECK (check_data (3));
  ++scene.raw_calls;
  if (interpreted && (scene.preparations || scene.frames || scene.encoded_blocks))
    return invalid ("interpreter unexpectedly compiled a function body");
  if (!interpreted && (!scene.preparations || !scene.legalizations || !scene.frames))
    return invalid ("native pipeline stages were not observed");
  if (scene.mode == "bbv" && !scene.encoded_blocks)
    return invalid ("BBV did not encode reached blocks");
  if (!scene.adapter_preparations || scene.adapter_legalizations != scene.adapter_preparations
      || scene.adapter_frames != scene.adapter_preparations)
    return invalid ("native adapters did not complete the machine pipeline");
  return scene;
#undef CHECK
#undef GET
}
}  // namespace
int mirnext_native_acceptance (int argc, char **argv, std::string_view linkage) {
  Report report;
  report.linkage = linkage;
  std::string path;
  for (int n = 1; n < argc; ++n) {
    std::string_view option (argv[n]);
    if ((option != "--expected" && option != "--report") || n + 1 == argc) {
      std::cerr << "usage: native-acceptance [--expected target] [--report path]\n";
      return 2;
    }
    (option == "--expected" ? report.expected : path) = argv[++n];
  }
  auto failed = [&] (Error error) {
    report.status = "failed";
    report.diagnostic = std::move (error.message);
    auto saved = save (path, report);
    if (!(saved).has_value ()) std::cerr << saved.error ().message << '\n';
    json (std::cerr, report);
    return 2;
  };
  // Replace a previous pass receipt before any code can fail or crash.
  auto saved = save (path, report);
  if (!(saved).has_value ()) return failed (saved.error ());
  auto host = mirnext::test::detect_native_host ();
  if (!(host).has_value ()) return failed (host.error ());
  report.host = std::move ((host).value ());
  auto target = mirnext::target::TargetMachine::native ();
  if (!(target).has_value ()) return failed (target.error ());
  report.target = target.value ().name ();
  auto admitted
    = mirnext::test::check_native_host (report.host, (target).value (), report.expected);
  if (!(admitted).has_value ()) return failed (admitted.error ());
  auto run = [&] (std::string mode, unsigned level) -> Result<void> {
    auto result = execute (std::move (mode), level, report.target);
    if (!(result).has_value ())
      return invalid ("scene " + std::to_string (report.scenes.size ()) + ": "
                      + result.error ().message);
    if (!report.scenes.empty ()
        && result.value ().fingerprint != report.scenes.front ().fingerprint)
      return invalid ("acceptance input changed between scenes");
    report.scenes.push_back (std::move ((result).value ()));
    return save (path, report);
  };
  auto original = run ("original-interpreter", 0);
  if (!(original).has_value ()) return failed (original.error ());
  for (unsigned level = 0; level != 4; ++level)
    for (auto mode : {"optimized-interpreter", "eager", "lazy", "bbv", "mixed"}) {
      auto result = run (mode, level);
      if (!(result).has_value ()) return failed (result.error ());
    }
  report.status = "passed";
  saved = save (path, report);
  if (!(saved).has_value ()) return failed (saved.error ());
  json (std::cout, report);
  return 0;
}
