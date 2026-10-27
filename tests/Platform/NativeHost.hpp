/* Licensed under the MIT License. Test-only native execution admission. */
#pragma once
#include "mirnext/Target/TargetMachine.hpp"
namespace mirnext::test {
struct NativeHost {
  std::string system, architecture;
  bool translated = false;
};
Result<NativeHost> detect_native_host ();
inline Result<void> check_native_host (const NativeHost &host, const target::TargetMachine &target,
                                       std::string_view expected = {}) {
  auto reject = [] (std::string message) -> Result<void> {
    return Error{ErrorCode::InvalidArgument, std::move (message)};
  };
  if (host.translated) return reject ("native acceptance rejects translated execution");
  auto architecture
    = target.architecture () == target::Architecture::AArch64 ? "aarch64" : "x86_64";
  if (host.architecture != architecture)
    return reject ("host architecture differs from compiled target");
  auto system = target.abi () == target::ABI::X86Windows     ? "windows"
                : target.abi () == target::ABI::AArch64MacOS ? "macos"
                : target.abi () == target::ABI::AArch64Linux ? "linux"
                                                             : host.system;
  if (host.system != system
      || (target.abi () == target::ABI::X86SysV && host.system != "linux"
          && host.system != "macos"))
    return reject ("host operating system differs from target ABI");
  if (!expected.empty () && expected != target.name ())
    return reject ("expected target " + std::string (expected) + ", actual "
                   + std::string (target.name ()));
  return {};
}
}  // namespace mirnext::test
