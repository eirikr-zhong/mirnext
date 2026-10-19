/* BBV preparation derived from MIR. Licensed under the MIT License. */
#include "mirnext/CodeGen/BlockVersioning.hpp"
#include <sstream>
namespace mirnext::codegen {
namespace {
using ir::Opcode;
bool is (const MachineInstruction &i, Opcode op) { return i.opcode () == MachineOpcode (op); }
bool ends (const MachineInstruction &i) {
  auto opcode = i.opcode ();
  auto code = std::get_if<Opcode> (&opcode);
  return code && ir::instruction_descriptor (*code)->flow != ir::ControlFlow::Next;
}
bool falls_through (const MachineInstruction &i) {
  return !is (i, Opcode::Jmp) && !is (i, Opcode::JmpIndirect) && !is (i, Opcode::Switch)
         && !is (i, Opcode::Ret) && !is (i, Opcode::JRet);
}
Error invalid (std::string message) {
  return {ErrorCode::InvalidArgument, "bbv-prepare: " + std::move (message)};
}
}  // namespace
namespace detail {
struct BBVLayout {
  BBVPreparation &result;
  BBVBlock &append () {
    result.blocks_.push_back (
      std::unique_ptr<BBVBlock> (new BBVBlock (result.function (), result.blocks_.size ())));
    return *result.blocks_.back ();
  }
  Result<void> run () {
    BBVBlock *current = nullptr;
    bool boundary = false;
    for (auto &block : result.function ().blocks ()) {
      // Basic-block containers without labels do not themselves introduce a
      // flat MIR boundary. Consecutive label-only containers share one anchor.
      if (!block.labels ().empty ()) {
        if (!current || !current->instructions_.empty ()) current = &append ();
        boundary = false;
        for (auto label : block.labels ()) {
          current->labels_.push_back (label);
          result.labels_.emplace (label, current);
        }
      }
      for (auto &i : block.instructions ()) {
        if (!current || boundary) current = &append ();
        current->instructions_.push_back (&i);
        result.instructions_.emplace (&i, current);
        boundary = ends (i);
      }
    }
    if (result.blocks_.empty ()) return invalid ("empty framed machine function");
    for (std::size_t n = 0; n < result.blocks_.size (); ++n) {
      auto &block = *result.blocks_[n];
      auto target = [&] (const MachineInstruction &i, std::size_t operand,
                         BBVSuccessor::Kind kind) -> Result<void> {
        auto label = i.operands ()[operand].get<const MachineLabel *> ();
        if (!label) return invalid ("expected a typed label successor");
        auto found = result.labels_.find (*label);
        if (found == result.labels_.end ()) return invalid ("unmapped label successor");
        block.successors_.push_back ({kind, found->second, &i, operand, *label});
        return {};
      };
      for (auto i : block.instructions_)
        if (is (*i, Opcode::LAddr)) {
          auto added = target (*i, 1, BBVSuccessor::Kind::LabelAddress);
          if (!added) return added;
        }
      auto last = block.instructions_.empty () ? nullptr : block.instructions_.back ();
      if (last && is (*last, Opcode::Switch)) {
        for (std::size_t p = 1; p < last->operands ().size (); ++p) {
          auto added = target (*last, p, BBVSuccessor::Kind::SwitchCase);
          if (!added) return added;
        }
      } else if (last && ends (*last) && !is (*last, Opcode::JmpIndirect)
                 && !is (*last, Opcode::Ret) && !is (*last, Opcode::JRet)) {
        auto added = target (*last, 0, BBVSuccessor::Kind::Branch);
        if (!added) return added;
      }
      if ((!last || falls_through (*last)) && n + 1 < result.blocks_.size ())
        block.successors_.push_back (
          {BBVSuccessor::Kind::Fallthrough, result.blocks_[n + 1].get (), last, SIZE_MAX, nullptr});
    }
    return {};
  }
};
}  // namespace detail
Result<BBVPreparation> BBVPreparation::prepare (const ir::Function &f,
                                                const target::TargetMachine &target,
                                                CompilationOptions options) {
  auto prepared = prepare_machine_function (f, target, std::move (options),
                                            MachinePreparationKind::BlockVersioning);
  if (!prepared) return prepared.error ();
  return create (std::move (*prepared));
}
Result<BBVPreparation> BBVPreparation::create (PreparedMachineFunction prepared) {
  auto checked = prepared.verify ();
  if (!checked) return checked.error ();
  if (prepared.kind () != MachinePreparationKind::BlockVersioning)
    return invalid ("requires property-preserving machine preparation");
  BBVPreparation result (std::move (prepared));
  auto built = detail::BBVLayout{result}.run ();
  if (!built) return built.error ();
  return result;
}
Result<const BBVBlock *> BBVPreparation::block_for (const MachineInstruction &i) const {
  if (!prepared_.valid ()) return invalid ("moved-from owner");
  auto found = instructions_.find (&i);
  if (found == instructions_.end ()) return invalid ("foreign machine instruction");
  return found->second;
}
Result<const BBVBlock *> BBVPreparation::block_for (const MachineLabel &label) const {
  if (!prepared_.valid ()) return invalid ("moved-from owner");
  auto found = labels_.find (&label);
  if (found == labels_.end ()) return invalid ("foreign machine label");
  return found->second;
}
Result<void> BBVPreparation::verify () const {
  auto checked = prepared_.verify ();
  if (!checked) return checked;
  if (prepared_.kind () != MachinePreparationKind::BlockVersioning || blocks_.empty ())
    return invalid ("invalid preparation kind or empty layout");
  std::size_t instructions = 0, labels = 0;
  for (std::size_t n = 0; n < blocks_.size (); ++n) {
    auto &block = *blocks_[n];
    if (&block.function () != &function () || block.index () != n)
      return invalid ("invalid block owner");
    for (auto i : block.instructions ()) {
      auto found = instructions_.find (i);
      if (found == instructions_.end () || found->second != &block)
        return invalid ("invalid instruction index");
      ++instructions;
    }
    for (auto label : block.labels ()) {
      auto found = labels_.find (label);
      if (found == labels_.end () || found->second != &block)
        return invalid ("invalid label index");
      ++labels;
    }
    for (auto &edge : block.successors ())
      if (edge.block->index () >= blocks_.size ()
          || blocks_[edge.block->index ()].get () != edge.block)
        return invalid ("foreign successor");
  }
  if (instructions != function ().instruction_count () || instructions != instructions_.size ()
      || labels
           != std::size_t (
             std::distance (function ().labels ().begin (), function ().labels ().end ()))
      || labels != labels_.size ())
    return invalid ("incomplete machine layout");
  return {};
}
std::string BBVPreparation::dump () const {
  if (!prepared_.valid ()) return "<moved-from BBV preparation>\n";
  std::ostringstream out;
  out << function ().module_name () << '/' << function ().name () << " bbv-layout\n";
  for (auto &block : blocks_) {
    out << "bb" << block->index ();
    for (auto label : block->labels ()) out << ' ' << label->name ();
    out << '\n';
    for (auto i : block->instructions ())
      out << "  #" << i->creation_order () << ' ' << opcode_name (i->opcode ()) << '\n';
    for (auto &edge : block->successors ())
      out << "  "
          << (edge.kind == BBVSuccessor::Kind::LabelAddress ? "address"
              : edge.kind == BBVSuccessor::Kind::Branch     ? "branch"
              : edge.kind == BBVSuccessor::Kind::SwitchCase ? "switch"
                                                            : "fallthrough")
          << " -> bb" << edge.block->index () << '\n';
  }
  return out.str ();
}
}  // namespace mirnext::codegen
