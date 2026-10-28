/* Licensed under the MIT License. Typed native symbol binding and owning execution. */
#include "Run.hpp"
#include "Streams.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <cstdlib>
#include <filesystem>
#include <unordered_map>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#endif
namespace mirnext::tools {
namespace {
using namespace execution;
std::vector<std::string> environment_list (const char *name) {
  std::vector<std::string> result;
  auto raw = std::getenv (name);
  if (!raw) return result;
  std::string_view value (raw);
#ifdef _WIN32
  constexpr char separator = ';';
#else
  constexpr char separator = ':';
#endif
  while (!value.empty ()) {
    auto end = value.find (separator);
    auto part = value.substr (0, end);
    if (!part.empty ()) result.emplace_back (part);
    if (end == value.npos) break;
    value.remove_prefix (end + 1);
  }
  return result;
}
class Libraries {
#ifdef _WIN32
  using Handle = HMODULE;
  static Handle open (const std::string &path) { return LoadLibraryA (path.c_str ()); }
  static void close (Handle value) { FreeLibrary (value); }
  static std::uintptr_t symbol (Handle h, const char *name) {
    return reinterpret_cast<std::uintptr_t> (GetProcAddress (h, name));
  }
#else
  using Handle = void *;
  static Handle open (const std::string &path) {
    return dlopen (path.empty () ? nullptr : path.c_str (), RTLD_LAZY | RTLD_LOCAL);
  }
  static void close (Handle value) { dlclose (value); }
  static std::uintptr_t symbol (Handle h, const char *name) {
    return reinterpret_cast<std::uintptr_t> (dlsym (h, name));
  }
#endif
  std::vector<Handle> handles_;

 public:
  ~Libraries () {
    for (auto i = handles_.rbegin (); i != handles_.rend (); ++i) close (*i);
  }
  Result<void> load () {
#ifdef _WIN32
    for (auto name : {"ucrtbase.dll", "msvcrt.dll", "kernel32.dll"})
      if (auto h = open (name)) handles_.push_back (h);
    constexpr auto prefix = "", suffix = ".dll";
#elif defined(__APPLE__)
    if (auto h = open ("")) handles_.push_back (h);
    constexpr auto prefix = "lib", suffix = ".dylib";
#else
    for (auto name : {"", "libc.so.6", "libm.so.6", "libpthread.so.0"})
      if (auto h = open (name)) handles_.push_back (h);
    constexpr auto prefix = "lib", suffix = ".so";
#endif
    auto directories = environment_list ("MIRNEXT_LIB_DIRS");
    auto loader = environment_list ("LD_LIBRARY_PATH");
    directories.insert (directories.end (), loader.begin (), loader.end ());
    for (auto &name : environment_list ("MIRNEXT_LIBS")) {
      std::filesystem::path path (name);
      auto filename
        = path.has_extension () || path.has_parent_path () ? name : prefix + name + suffix;
      Handle h = nullptr;
      if (!path.has_parent_path ())
        for (auto &dir : directories)
          if ((h = open ((std::filesystem::path (dir) / filename).string ()))) break;
      if (!h) h = open (filename);
      if (!h) return invalid ("cannot load MIR library '" + name + "'");
      handles_.push_back (h);
    }
    return {};
  }
  Result<std::uintptr_t> find (std::string_view name) const {
#ifndef _WIN32
    // stat can be a platform symbol alias selected by the system headers.
    if (name == "stat") return reinterpret_cast<std::uintptr_t> (&::stat);
    if (name == "lstat") return reinterpret_cast<std::uintptr_t> (&::lstat);
    if (name == "fstat") return reinterpret_cast<std::uintptr_t> (&::fstat);
#endif
    std::string terminated (name);
    for (auto h : handles_)
      if (auto address = symbol (h, terminated.c_str ())) return address;
    return Error{ErrorCode::UnknownName, "cannot resolve host symbol '" + terminated + "'"};
  }
};
Result<ExecutionOptions> execution_options () {
  ExecutionOptions options;
  options.link.compilation = JITCompilation::Lazy;
  std::string_view mode = std::getenv ("MIRNEXT_MODE") ? std::getenv ("MIRNEXT_MODE") : "";
  if (mode == "interp") {
    options.mode = ExecutionMode::Interpreter;
    options.native_entries = true;
    options.link.compilation = JITCompilation::Eager;
  } else if (mode == "gen")
    options.link.compilation = JITCompilation::Eager;
  else if (mode == "bbv")
    options.link.compilation = JITCompilation::BlockVersioning;
  else if (!mode.empty () && mode != "lazy")
    return invalid ("unknown MIRNEXT_MODE '" + std::string (mode)
                    + "' (expected interp, gen, lazy or bbv)");
  return options;
}
}  // namespace
Result<int> run_program (std::span<const std::byte> bytes, int argc, char **argv,
                         char **environment) {
  auto context = serialization::read_binary (bytes);
  if (!(context).has_value ()) return context.error ();
  const ir::Function *main = nullptr;
  // Derive typed bindings from direct call prototypes. Never guess an unknown
  // symbol's kind or call contract from its address or a host name convention.
  std::unordered_map<std::string, ir::Signature> signatures;
  for (auto &m : context.value ().modules ())
    for (auto &f : m.functions ()) {
      if (f.symbol ().name () == "main") {
        if (main) return invalid ("multiple main definitions");
        main = &f;
      }
      for (auto &b : f.blocks ())
        for (auto &i : b.instructions ()) {
          if (ir::instruction_descriptor (i.opcode ()).get ()->count != ir::OperandCount::Call)
            continue;
          auto ref = i.operands ()[1].get<ir::SymbolReference> ();
          if (!(ref).has_value () || ref.value ().symbol ().kind () != ir::Symbol::Kind::Import)
            continue;
          auto prototype = i.operands ()[0].get<ir::SymbolReference> ();
          auto signature = *prototype.value ().symbol ().signature ().get ();
          auto [at, inserted] = signatures.emplace (ref.value ().symbol ().name (), signature);
          if (!inserted && at->second != signature)
            return invalid ("inconsistent call prototypes for import '"
                            + std::string (ref.value ().symbol ().name ()) + "'");
        }
    }
  if (!main) return invalid ("cannot execute program without main function");
  auto &sig = main->signature ();
  auto pointer = [] (ir::Type t) {
    return t == ir::Type::pointer () || t == ir::Type::i64 () || t.kind () == ir::Type::Kind::U64;
  };
  auto count
    = [] (ir::Type t) { return t == ir::Type::i64 () || t.kind () == ir::Type::Kind::I32; };
  if (sig.variadic || sig.results.size () != 1 || !sig.results[0].is_integer ()
      || (sig.parameters.size () != 0 && sig.parameters.size () != 2 && sig.parameters.size () != 3)
      || (!sig.parameters.empty () && !count (sig.parameters[0]))
      || (sig.parameters.size () >= 2 && !pointer (sig.parameters[1]))
      || (sig.parameters.size () == 3 && !pointer (sig.parameters[2])))
    return invalid (
      "main requires one integer result and (), (argc, argv) or (argc, argv, envp) scalar "
      "parameters");
  auto options = execution_options ();
  if (!(options).has_value ()) return options.error ();
  Libraries libraries;
  auto loaded = libraries.load ();
  if (!(loaded).has_value ()) return loaded.error ();
  auto target = target::TargetMachine::native ();
  if (!(target).has_value ()) return target.error ();
  options.value ().link.resolve = [&] (const ImportRequest &request) -> Result<ExternalSymbol> {
    auto address = libraries.find (request.name);
    if (!(address).has_value ()) return address.error ();
    ExternalSymbol symbol;
    symbol.address = (address).value ();
    if ((request.signature ()).has_value ())
      symbol.signature = (request.signature ()).value ();
    else if (auto at = signatures.find (std::string (request.name)); at != signatures.end ())
      symbol.signature = at->second;
    else if ((request.variable ()).has_value ()) {
      auto layout = target.value ().layout (request.variable ().value ().type ());
      if (!(layout).has_value ()) return layout.error ();
      symbol.data = ExternalData{layout.value ().size, request.variable ().value ().writable (),
                                 request.variable ().value ().type ()};
    } else
      return invalid ("import '" + std::string (request.name)
                      + "' requires a typed declaration or direct call prototype");
    return symbol;
  };
  auto session = ExecutionSession::create ((context).value (), std::move ((options).value ()));
  if (!(session).has_value ()) return session.error ();
  auto function = session.value ().find_function (main->module ().name (), main->symbol ().name ());
  if (!(function).has_value ()) return function.error ();
  std::array<std::int64_t, 3> arguments{argc,
                                        std::int64_t (reinterpret_cast<std::uintptr_t> (argv)),
                                        std::int64_t (
                                          reinterpret_cast<std::uintptr_t> (environment))};
  auto result = function.value ().call_i64 (std::span (arguments).first (sig.parameters.size ()));
  if (!(result).has_value ()) return result.error ();
  return static_cast<int> ((result).value ());
}
}  // namespace mirnext::tools
