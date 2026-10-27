/* Licensed under the MIT License. Inspect the actual host before executing IR. */
#include "NativeHost.hpp"
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/utsname.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <cerrno>
#endif
#endif
namespace mirnext::test {
Result<NativeHost> detect_native_host () {
  NativeHost host;
#if defined(_WIN32)
  SYSTEM_INFO info{};
  GetNativeSystemInfo (&info);
  host.system = "windows";
  if (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
    host.architecture = "x86_64";
  else if (info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
    host.architecture = "aarch64";
#else
  utsname info{};
  if (uname (&info)) return Error{ErrorCode::InvalidArgument, "unable to inspect native host"};
  std::string_view machine (info.machine);
  if (machine == "aarch64" || machine == "arm64")
    host.architecture = "aarch64";
  else if (machine == "x86_64" || machine == "amd64")
    host.architecture = "x86_64";
#if defined(__APPLE__)
  host.system = "macos";
  int translated = 0;
  std::size_t size = sizeof translated;
  if (sysctlbyname ("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0)
    host.translated = translated != 0;
  else if (errno != ENOENT)
    return Error{ErrorCode::InvalidArgument, "unable to inspect process translation"};
#else
  host.system = info.sysname == std::string_view ("Linux") ? "linux" : std::string (info.sysname);
#endif
#endif
  if (host.architecture.empty ())
    return Error{ErrorCode::InvalidArgument, "unknown native host architecture"};
  return host;
}
}  // namespace mirnext::test
