/* Licensed under the MIT License. Owning runtime acceptance, no C oracle. */
#include "NativeHost.hpp"
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
namespace {
using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::execution;
Error invalid(std::string message) { return {ErrorCode::InvalidArgument,std::move(message)}; }
struct Scene {
  std::string mode;
  unsigned level;
  std::size_t checked_calls = 0, raw_calls = 0, preparations = 0, legalizations = 0, frames = 0, encoded_blocks = 0;
  std::size_t adapter_preparations = 0, adapter_legalizations = 0, adapter_frames = 0;
  std::uint64_t fingerprint = 0;
};
struct Report {
  std::string status = "running", diagnostic, target, expected, linkage;
  test::NativeHost host;
  std::vector<Scene> scenes;
};
void quoted(std::ostream &out,std::string_view value) {
  constexpr char hex[] = "0123456789abcdef"; out << '"';
  for (unsigned char c : value) {
    if (c == '"' || c == '\\') out << '\\' << char(c);
    else if (c < 32) out << "\\u00" << hex[c>>4] << hex[c&15];
    else out << char(c);
  }
  out << '"';
}
void json(std::ostream &out,const Report &r) {
  out << "{\n  \"schema\": 2, \"status\": "; quoted(out,r.status);
  out << ",\n  \"source_fingerprint\": \"" << MIRNEXT_TEST_SOURCE_FINGERPRINT
      << "\", \"configuration\": \"" << MIRNEXT_TEST_CONFIGURATION << "\", \"c_reference\": "
      << (MIRNEXT_TEST_C_REFERENCE ? "true" : "false") << ", \"linkage\": "; quoted(out,r.linkage);
  out << ",\n  \"target\": "; quoted(out,r.target); out << ", \"expected_target\": "; quoted(out,r.expected);
  out << ",\n  \"host_system\": "; quoted(out,r.host.system); out << ", \"host_architecture\": "; quoted(out,r.host.architecture);
  out << ", \"translated\": " << (r.host.translated ? "true" : "false");
  out << ",\n  \"diagnostic\": "; quoted(out,r.diagnostic);
  out << ",\n  \"completed_scenes\": " << r.scenes.size() << ",\n  \"scenes\": [";
  bool first = true;
  for (auto &s : r.scenes) {
    if (!first) out << ','; first = false; out << "\n    {\"mode\": "; quoted(out,s.mode);
    out << ", \"level\": " << s.level << ", \"checked_calls\": " << s.checked_calls << ", \"raw_calls\": " << s.raw_calls
      << ", \"preparations\": " << s.preparations << ", \"legalizations\": " << s.legalizations << ", \"frames\": " << s.frames
      << ", \"encoded_blocks\": " << s.encoded_blocks << ", \"adapter_preparations\": " << s.adapter_preparations
      << ", \"adapter_legalizations\": " << s.adapter_legalizations << ", \"adapter_frames\": " << s.adapter_frames
      << ", \"input_fingerprint\": " << s.fingerprint << '}';
  }
  out << "\n  ]\n}\n";
}
Result<void> save(std::string_view path,const Report &report) {
  if (path.empty()) return {};
  std::ofstream out(std::string(path),std::ios::trunc); json(out,report); out.close();
  if (!out) return invalid("unable to write native acceptance report"); return {};
}
std::int64_t host_add(std::int64_t value) { return value+5; }
Result<std::unique_ptr<Context>> fixture() {
  auto c = std::make_unique<Context>();
#define GET(name, expression) auto name = (expression); if (!name) return name.error()
#define CHECK(expression) do { auto checked = (expression); if (!checked) return checked.error(); } while (false)
  GET(provider,c->create_module("provider")); const Signature signature{{Type::i64()},{Type::i64()}};
  GET(scale,(*provider)->create_function("scale",signature)); GET(scale_body,(*scale)->create_block());
  IRBuilder sb(**scale_body); CHECK(sb.ret(sb.expression(*(*scale)->arguments()[0])*3)); CHECK((*provider)->export_symbol((*scale)->symbol()));
  GET(module,c->create_module("application")); GET(import,(*module)->import("scale",signature));
  GET(prototype,(*module)->prototype("scale_signature",signature)); GET(host,(*module)->import("host_add",signature));
  GET(total,(*module)->create_variable("total",Type::i64(),0)); GET(function,(*module)->create_function("entry",signature));
  GET(entry,(*function)->create_block()); GET(header,(*function)->create_block()); GET(body,(*function)->create_block()); GET(exit,(*function)->create_block());
  GET(loop,(*function)->create_label(**header)); GET(done,(*function)->create_label(**exit));
  GET(index,(*function)->create_register(Type::i64())); GET(sum,(*function)->create_register(Type::i64()));
  IRBuilder begin(**entry); CHECK(begin.assign(**index,Operand(0))); CHECK(begin.assign(**sum,Operand(0)));
  IRBuilder condition(**header); CHECK(condition.branch(Comparison::GreaterEqual,condition.expression(**index),condition.expression(*(*function)->arguments()[0]),**done));
  IRBuilder step(**body); GET(address,step.address_of(**import));
  GET(call,step.call_indirect(**prototype,Operand(**address),{Operand(**index)})); GET(value,call->value());
  CHECK(step.assign(**sum,step.expression(**sum)+step.expression(**value))); CHECK(step.assign(**index,step.expression(**index)+1)); CHECK(step.jump(**loop));
  IRBuilder end(**exit); CHECK(end.store(**total,Operand(**sum))); GET(result,end.call(**host,{Operand(**sum)})); GET(answer,result->value()); CHECK(end.ret(Operand(**answer)));
  CHECK(c->verify()); return c;
}
Result<Scene> execute(std::string mode,unsigned level,std::string_view target) {
  Scene scene{std::move(mode),level}; GET(source,fixture()); auto original = dump(**source);
  GET(binary,serialization::write_binary(**source)); scene.fingerprint = 14695981039346656037ULL;
  for (auto byte : *binary) scene.fingerprint = (scene.fingerprint ^ std::to_integer<unsigned char>(byte))*1099511628211ULL;
  auto interpreted = scene.mode == "original-interpreter" || scene.mode == "optimized-interpreter";
  ExecutionOptions options;
  if (scene.mode != "original-interpreter") options.optimization = transforms::OptimizationOptions{.level=static_cast<transforms::OptimizationLevel>(level)};
  if (interpreted) { options.mode = ExecutionMode::Interpreter; options.native_entries = true; }
  if (scene.mode == "lazy" || scene.mode == "mixed") options.link.compilation = JITCompilation::Lazy;
  if (scene.mode == "bbv") options.link.compilation = JITCompilation::BlockVersioning;
  if (scene.mode == "mixed") options.select_engine = [](const Function &f) -> Result<ExecutionEngine> {
    return f.symbol().name() == "scale" ? ExecutionEngine::Interpreter : ExecutionEngine::LazyJIT;
  };
  options.link.resolve = [](const ImportRequest &request) -> Result<ExternalSymbol> {
    if (request.name != "host_add") return invalid("unexpected acceptance import");
    ExternalSymbol external; external.address = reinterpret_cast<std::uintptr_t>(&host_add);
    external.signature = Signature{{Type::i64()},{Type::i64()}};
    external.interpret = [](std::span<const ExecutionArgument> args) -> Result<std::vector<ScalarValue>> {
      if (args.size() != 1 || !args[0].scalar()) return invalid("invalid acceptance callback arguments");
      GET(value,args[0].scalar()->as_i64()); return std::vector<ScalarValue>{ScalarValue(host_add(*value))};
    };
    return external;
  };
  options.link.compilation_observer = [&](const codegen::CompilationSnapshot &s) -> Result<void> {
    if (s.function.target().name() != target) return invalid("machine pipeline target mismatch");
    bool body = s.function.module_name() == "application" || s.function.module_name() == "provider";
    (body ? scene.preparations : scene.adapter_preparations) += s.stage == codegen::CompilationStage::PrepareMachineIR;
    (body ? scene.legalizations : scene.adapter_legalizations) += s.stage == codegen::CompilationStage::LegalizeOperands;
    (body ? scene.frames : scene.adapter_frames) += s.stage == codegen::CompilationStage::Frame; return {};
  };
  options.link.block_versioning.observer = [&](const BlockCompilationSnapshot &) -> Result<void> { ++scene.encoded_blocks; return {}; };
  struct Handles { FunctionHandle function; LinkedData data; NativeEntry native; };
  auto load = [&]() -> Result<Handles> {
    GET(session,ExecutionSession::create(**source,std::move(options)));
    if (dump(**source) != original) return invalid("session mutated source IR");
    GET(function,session->find_function("application","entry")); GET(data,session->find_data("application","total")); GET(native,session->native_entry(*function));
    auto expected = interpreted ? ExecutionEngine::Interpreter : scene.mode == "bbv" ? ExecutionEngine::BlockVersioning
      : scene.mode == "eager" ? ExecutionEngine::EagerJIT : ExecutionEngine::LazyJIT;
    if (function->engine() != expected) return invalid("execution engine selection mismatch");
    GET(helper,session->find_function("provider","scale"));
    if (helper->engine() != (scene.mode == "mixed" ? ExecutionEngine::Interpreter : expected)) return invalid("callee engine selection mismatch");
    return Handles{std::move(*function),std::move(*data),std::move(*native)};
  };
  GET(handles,load()); source->reset(); // Handles alone now own all IR, data and code.
  auto check_data = [&](std::int64_t n) -> Result<void> {
    auto bytes = handles->data.bytes(); if (bytes.size() != sizeof(std::int64_t)) return invalid("acceptance global layout mismatch");
    std::int64_t actual; std::memcpy(&actual,bytes.data(),sizeof actual);
    if (actual != 3*n*(n-1)/2) return invalid("acceptance global write mismatch"); return {};
  };
  for (std::int64_t n : {4,0,9,2,4}) {
    std::array args{n}; GET(value,handles->function.call_i64(args));
    if (*value != 3*n*(n-1)/2+5) return invalid("acceptance checked call result mismatch");
    CHECK(check_data(n)); ++scene.checked_calls;
  }
  ExecutionInvocation invocation;
  auto raw = reinterpret_cast<std::int64_t (*)(std::int64_t)>(handles->native.address());
  if (raw(3) != 14) return invalid("acceptance native ABI result mismatch"); CHECK(invocation.status()); CHECK(check_data(3)); ++scene.raw_calls;
  if (interpreted && (scene.preparations || scene.frames || scene.encoded_blocks)) return invalid("interpreter unexpectedly compiled a function body");
  if (!interpreted && (!scene.preparations || !scene.legalizations || !scene.frames)) return invalid("native pipeline stages were not observed");
  if (scene.mode == "bbv" && !scene.encoded_blocks) return invalid("BBV did not encode reached blocks");
  if (!scene.adapter_preparations || scene.adapter_legalizations != scene.adapter_preparations || scene.adapter_frames != scene.adapter_preparations)
    return invalid("native adapters did not complete the machine pipeline");
  return scene;
#undef CHECK
#undef GET
}
}
int mirnext_native_acceptance(int argc,char **argv,std::string_view linkage) {
  Report report; report.linkage = linkage; std::string path;
  for (int n = 1; n < argc; ++n) {
    std::string_view option(argv[n]);
    if ((option != "--expected" && option != "--report") || n+1 == argc) { std::cerr << "usage: native-acceptance [--expected target] [--report path]\n"; return 2; }
    (option == "--expected" ? report.expected : path) = argv[++n];
  }
  auto failed = [&](Error error) {
    report.status = "failed"; report.diagnostic = std::move(error.message);
    auto saved = save(path,report); if (!saved) std::cerr << saved.error().message << '\n';
    json(std::cerr,report); return 2;
  };
  // Replace a previous pass receipt before any code can fail or crash.
  auto saved = save(path,report); if (!saved) return failed(saved.error());
  auto host = mirnext::test::detect_native_host(); if (!host) return failed(host.error()); report.host = std::move(*host);
  auto target = mirnext::target::TargetMachine::native(); if (!target) return failed(target.error()); report.target = target->name();
  auto admitted = mirnext::test::check_native_host(report.host,*target,report.expected); if (!admitted) return failed(admitted.error());
  auto run = [&](std::string mode,unsigned level) -> Result<void> {
    auto result = execute(std::move(mode),level,report.target);
    if (!result) return invalid("scene " + std::to_string(report.scenes.size()) + ": " + result.error().message);
    if (!report.scenes.empty() && result->fingerprint != report.scenes.front().fingerprint) return invalid("acceptance input changed between scenes");
    report.scenes.push_back(std::move(*result)); return save(path,report);
  };
  auto original = run("original-interpreter",0); if (!original) return failed(original.error());
  for (unsigned level = 0; level != 4; ++level) for (auto mode : {"optimized-interpreter","eager","lazy","bbv","mixed"}) {
    auto result = run(mode,level); if (!result) return failed(result.error());
  }
  report.status = "passed"; saved = save(path,report); if (!saved) return failed(saved.error()); json(std::cout,report); return 0;
}
