/* Licensed under the MIT License. MIR creates target anchors before visiting their layout. */
#include "mirnext/Analysis/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/Analysis/ConstructionOrder.hpp"
namespace mirnext::analysis {
const unsigned char *ConstructionOrderAnalysis::id () noexcept {
  static unsigned char token;
  return &token;
}
mirnext::Result<ConstructionOrder> ConstructionOrderAnalysis::run (const ir::Function &function) {
  auto checked = function.require_register_ir ();
  if (!(checked).has_value ()) return checked.error ();
  ConstructionOrder order;
  bool hardware = false;
  for (auto &reg : function.registers ())
    hardware |= !reg.hardware_binding ().empty () && !reg.is_memory_backed ();
  auto label = [&] (const ir::Label &value) {
    auto [found, fresh]
      = detail::NodeAccess::labels (order).emplace (&value, order.next_instruction);
    if (fresh) ++order.next_instruction;
    return found->second;
  };
  auto instruction = [&] (const ir::Instruction &value) {
    auto [found, fresh]
      = detail::NodeAccess::instructions (order).emplace (&value, order.next_instruction);
    if (fresh) ++order.next_instruction;
    return found->second;
  };
  auto anchor = [&] (const ir::BasicBlock &block) {
    if (!ir::detail::ReferenceAccess::pointers (block.labels ()).empty ())
      return label (**ir::detail::ReferenceAccess::pointers (block.labels ()).begin ());
    return block.size () ? instruction (*block.instructions ().begin ()) : order.next_instruction;
  };
  const auto first = &*function.blocks ().begin ();
  if (!ir::detail::ReferenceAccess::pointers (first->labels ()).empty ()) ++order.next_block;
  for (auto it = function.blocks ().begin (); it != function.blocks ().end (); ++it) {
    auto &block = *it;
    if (ir::detail::ReferenceAccess::pointers (block.labels ()).size () > 1)
      return Error{ErrorCode::InvalidArgument,
                   "construction order requires separated label anchors"};
    auto head = &block == first && ir::detail::ReferenceAccess::pointers (block.labels ()).empty ()
                  ? 0
                  : anchor (block);
    detail::NodeAccess::blocks (order).emplace (&block,
                                                ConstructionOrder::Block{order.next_block++, head});
    auto next = std::next (it);
    for (auto &insn : block.instructions ()) {
      if (hardware && insn.opcode () == ir::Opcode::Ret) ++order.next_instruction;
      instruction (insn);
      if (&insn == &*std::prev (block.instructions ().end ()) && next != function.blocks ().end ())
        anchor (*next);
      for (auto &operand : insn.operands ())
        if (auto target = ir::detail::operand_access (operand).get<const ir::Label *> ())
          label (**target);
    }
  }
  return order;
}
}  // namespace mirnext::analysis
