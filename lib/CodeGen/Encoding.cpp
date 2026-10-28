/* Licensed under the MIT License. Checked native encoding dispatch. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Detail/Encoding.hpp"
namespace mirnext::codegen {
Result<CompiledFunction> encode_machine (const MachineFunction &f, const MachineFrameResult &frame,
                                         MachineEncodingOptions options) {
  auto error = [&] (std::string why) {
    return Error{ErrorCode::InvalidOperand,
                 std::string (f.module_name ()) + '/' + std::string (f.name ()) + " ["
                   + std::string (f.target ().name ()) + ", encode]: " + std::move (why)};
  };
  if (!frame.current (f) || !f.state ().frame_formed)
    return error ("stale/foreign frame or invalid phase");
  auto verified = f.verify ();
  if (!(verified).has_value ()) return error (verified.error ().message);
  for (auto &block : f.blocks ())
    for (auto &i : block.instructions ())
      if (i.opcode () == MachineOpcode (ir::Opcode::PrSet)
          || i.opcode () == MachineOpcode (ir::Opcode::PrBeq)
          || i.opcode () == MachineOpcode (ir::Opcode::PrBne))
        return error ("unresolved BBV property instruction requires block specialization");
  switch (f.target ().architecture ()) {
  case target::Architecture::Native: break;
  case target::Architecture::AArch64: return detail::encode_aarch64 (f, frame, std::move (options));
  case target::Architecture::X86_64: return detail::encode_x86 (f, frame, std::move (options));
  }
  return error ("unsupported target");
}
}  // namespace mirnext::codegen
