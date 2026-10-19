/* MIR frame rules, Copyright (C) 2020-2024 Vladimir Makarov. MIT License. */
#include "mirnext/CodeGen/Frame.hpp"
#include "mirnext/CodeGen/Detail/SpillEmitter.hpp"
#include "mirnext/CodeGen/Detail/X86Frame.hpp"
#include "mirnext/CodeGen/Detail/InstructionPatterns.hpp"
#include <algorithm>
#include <climits>
namespace mirnext::codegen {
namespace {
using ir::Opcode;
using ir::Type;
Error invalid (const MachineFunction &f, std::string why) {
  return {ErrorCode::InvalidOperand, std::string (f.module_name ()) + "/" + std::string (f.name ())
                                       + " [" + std::string (f.target ().name ())
                                       + ", frame]: " + std::move (why)};
}
std::size_t align16 (std::size_t n) { return (n + 15) / 16 * 16; }
struct Emission {
  MachineOpcode code;
  std::vector<MachineOperand> operands;
};
}  // namespace
namespace detail {
struct FrameEngine {
  MachineFunction &f;
  MachineRewriter rw;
  MachinePostResult post;
  MachineFrameResult result;
  std::vector<Emission> prologue, epilogue;
  MachineInstruction *last_return = nullptr;
  std::vector<MachineInstruction *> earlier_returns;
  bool jump_return = false;
  FrameEngine (MachineFunction &f, MachinePostResult post)
    : f (f), rw (f), post (std::move (post)) {}
  MachineOperand reg (unsigned n) const { return MachineOperand (f.target ().registers ()[n].reg); }
  MachineOperand imm (std::size_t n) const {
    return MachineOperand (static_cast<std::int64_t> (n));
  }
  MachineOperand memory (Type type, std::int64_t offset, unsigned base) const {
    return MachineOperand (MachineMemory{type,
                                         MachineRegister (f.target ().registers ()[base].reg),
                                         {},
                                         offset,
                                         1,
                                         {},
                                         {}});
  }
  void emit (std::vector<Emission> &code, Opcode op, std::vector<MachineOperand> operands) {
    code.push_back ({op, std::move (operands)});
  }
  MachineOperand address (std::vector<Emission> &code, Type type, std::size_t offset,
                          unsigned base) {
    if (MachineSpillEmitter::valid_offset (f.target ().architecture (), type,
                                           static_cast<std::int64_t> (offset)))
      return memory (type, offset, base);
    emit (code, Opcode::Mov, {reg (10), imm (offset)});
    emit (code, Opcode::Add, {reg (10), reg (10), reg (base)});
    return memory (type, 0, 10);
  }
  void save (std::vector<Emission> &code, unsigned n, std::size_t offset, unsigned base,
             bool restore) {
    auto type = n < 32 ? Type::i64 () : Type (Type::Kind::LongDouble);
    auto mem = address (code, type, offset, base);
    emit (code, n < 32 ? Opcode::Mov : Opcode::LDMov,
          restore ? std::vector{reg (n), mem} : std::vector{mem, reg (n)});
  }
  Result<void> prepare_x86 (MachineFrameOptions options) {
    std::vector<target::X86Register> saved;
    for (auto &r : f.target ().registers ()) {
      const bool used = std::find (post.allocation ().used_registers ().begin (),
                                   post.allocation ().used_registers ().end (), r.reg)
                        != post.allocation ().used_registers ().end ();
      if (!f.target ().call_clobbers (r.reg, Type (Type::Kind::Undef))
          && (used || (options.preserve_host_registers && !r.reserved)))
        saved.push_back (static_cast<target::X86Register> (r.reg.encoding ()));
    }
    X86FrameInput input{f.target ().abi ()};
    input.spill_bytes = post.allocation ().stack_bytes ();
    input.saved_registers = saved;
    input.leaf = f.state ().is_leaf;
    input.uses_alloca = f.state ().uses_alloca;
    input.block_arguments = f.state ().has_block_arguments;
    input.variadic = f.signature ().variadic;
    input.frame_pointer = f.state ().requires_frame_pointer;
    input.jump_return = jump_return;
    input.has_return = last_return != nullptr;
    input.no_red_zone = options.no_red_zone;
    auto plan = plan_x86_frame (input);
    if (!plan) return invalid (f, plan.error ().message);
    auto &layout = result.layout_;
    layout.frame_bytes = plan->frame_bytes;
    layout.spill_bytes = input.spill_bytes;
    layout.variadic_save_bytes = plan->variadic_save_bytes;
    layout.saves_frame_pointer = plan->saves_frame_pointer;
    for (auto &s : plan->saves)
      layout.callee_saves.push_back (
        {f.target ().registers ()[static_cast<unsigned> (s.reg)].reg, s.offset, s.size});
    auto convert = [&] (const auto &source, auto &destination) {
      for (auto &i : source) {
        Emission emission{i.opcode, {}};
        emission.operands.reserve (i.operands.size ());
        for (auto &o : i.operands) {
          if (auto r = std::get_if<target::X86Register> (&o))
            emission.operands.push_back (reg (static_cast<unsigned> (*r)));
          else if (auto n = std::get_if<std::int64_t> (&o))
            emission.operands.emplace_back (*n);
          else {
            auto &m = std::get<X86FrameMemory> (o);
            emission.operands.push_back (
              memory (m.type, m.displacement, static_cast<unsigned> (m.base)));
          }
        }
        destination.push_back (std::move (emission));
      }
    };
    convert (plan->prologue, prologue);
    convert (plan->epilogue, epilogue);
    return {};
  }
  Result<void> prepare_aarch64 (MachineFrameOptions options) {
    constexpr auto reserve = 4096u;
    const auto &allocation = post.allocation ();
    const auto stack_bytes = allocation.stack_bytes ();
    result.layout_.spill_bytes = stack_bytes;
    auto aggregate
      = std::get<MachineFunctionState::AArch64> (f.state ().target).aggregate_save_bytes;
    if (stack_bytes > INT32_MAX - reserve || aggregate > INT32_MAX - reserve
        || stack_bytes > INT32_MAX - reserve - aggregate)
      return invalid (f, "frame size exceeds target offset range");
    std::vector<target::PhysicalRegister> saved;
    for (auto &r : f.target ().registers ()) {
      auto used = std::find (allocation.used_registers ().begin (),
                             allocation.used_registers ().end (), r.reg)
                  != allocation.used_registers ().end ();
      if (!f.target ().call_clobbers (r.reg, Type (Type::Kind::Undef))
          && (used || (options.preserve_host_registers && !r.reserved)))
        saved.push_back (r.reg);
    }
    if (f.state ().is_leaf && !f.state ().uses_alloca && saved.empty () && !f.signature ().variadic
        && stack_bytes == 0 && !f.state ().has_block_arguments && aggregate == 0)
      return {};
    auto &layout = result.layout_;
    layout.variadic_save_bytes
      = f.target ().abi () == target::ABI::AArch64MacOS || !f.signature ().variadic ? 0u : 192u;
    std::size_t frame = layout.variadic_save_bytes;
    for (auto r : saved) {
      if (r.encoding () >= 32) frame = align16 (frame);
      frame += r.encoding () < 32 ? 8 : 16;
    }
    frame = align16 (frame);
    const auto saved_end = frame;
    frame = align16 (frame + stack_bytes);
    layout.saves_previous_stack = f.signature ().variadic || f.state ().has_block_arguments;
    if (layout.saves_previous_stack) {
      emit (prologue, Opcode::Mov, {reg (9), reg (31)});
      frame += 16;
    }
    frame += 16;
    layout.frame_bytes = frame;
    layout.aggregate_bytes = align16 (aggregate);
    layout.saves_link = !jump_return;
    layout.saves_frame_pointer = true;
    if (frame >= 4096) emit (prologue, Opcode::Mov, {reg (10), imm (frame)});
    emit (prologue, Opcode::Sub, {reg (31), reg (31), frame < 4096 ? imm (frame) : reg (10)});
    if (layout.saves_previous_stack)
      emit (prologue, Opcode::Mov, {memory (Type::i64 (), 16, 31), reg (9)});
    if (layout.saves_link) emit (prologue, Opcode::Mov, {memory (Type::i64 (), 8, 31), reg (30)});
    emit (prologue, Opcode::Mov, {memory (Type::i64 (), 0, 31), reg (29)});
    emit (prologue, Opcode::Mov, {reg (29), reg (31)});
    if (f.target ().abi () == target::ABI::AArch64Linux && f.signature ().variadic) {
      std::size_t start = frame - 192;
      unsigned base = 31;
      if (start + 184 >= 4096) {
        emit (prologue, Opcode::Mov, {reg (9), imm (start)});
        start = 0;
        base = 9;
      }
      for (unsigned n = 0; n < 8; ++n) save (prologue, 32 + n, start + 16 * n, base, false);
      for (unsigned n = 0; n < 8; ++n) save (prologue, n, start + 128 + 8 * n, base, false);
    }
    auto offset = frame - saved_end;
    for (auto r : saved) {
      if (r.encoding () >= 32) offset = align16 (offset);
      const auto bytes = r.encoding () < 32 ? 8u : 16u;
      layout.callee_saves.push_back ({r, static_cast<std::int64_t> (offset), bytes});
      save (prologue, r.encoding (), offset, 29, false);
      offset += bytes;
    }
    if (aggregate) emit (prologue, Opcode::Sub, {reg (31), reg (31), imm (layout.aggregate_bytes)});
    if (last_return) {
      for (auto &s : layout.callee_saves) save (epilogue, s.reg.encoding (), s.offset, 29, true);
      if (layout.saves_link) emit (epilogue, Opcode::Mov, {reg (30), memory (Type::i64 (), 8, 29)});
      emit (epilogue, Opcode::Mov, {reg (10), reg (29)});
      emit (epilogue, Opcode::Mov, {reg (29), memory (Type::i64 (), 0, 29)});
      if (frame >= 4096) emit (epilogue, Opcode::Mov, {reg (9), imm (frame)});
      emit (epilogue, Opcode::Add, {reg (31), reg (10), frame < 4096 ? imm (frame) : reg (9)});
    }
    return {};
  }
  Result<MachineFrameResult> run (MachineFrameOptions options) {
    for (auto b : post.allocation ().loop_alignment_blocks ())
      for (auto label : b->labels ()) result.aligned_labels_.push_back (label);
    for (auto &b : f.blocks ())
      for (auto &i : b.instructions ()) {
        if (i.opcode () == MachineOpcode (Opcode::Ret)
            || i.opcode () == MachineOpcode (Opcode::JRet)) {
          if (last_return) earlier_returns.push_back (last_return);
          last_return = &i;
        }
        jump_return |= i.opcode () == MachineOpcode (Opcode::JRet);
      }
    auto prepared = f.target ().architecture () == target::Architecture::AArch64
                      ? prepare_aarch64 (options)
                      : prepare_x86 (options);
    if (!prepared) return prepared.error ();
    // Validate the entire sequence before modifying stable machine nodes.
    // Full vector saves retain LDMOV on ARM and private MOV128 on x86.
    for (auto sequence : {&prologue, &epilogue})
      for (auto &i : *sequence)
        if (!instruction_pattern_matches (f.target (), i.code, i.operands))
          return invalid (f, "generated instruction has no target pattern");
    if (!prologue.empty ()) {
      auto &first = *f.blocks ().begin ();
      auto block = &first;
      if (!first.labels ().empty ()) {
        auto r = rw.create_block_before (first);
        if (!r) return invalid (f, r.error ().message);
        block = *r;
      }
      auto body = block->instructions ();
      auto anchor = body.begin () == body.end () ? nullptr : &*body.begin ();
      for (auto &i : prologue) {
        auto r = anchor ? rw.insert_before (*anchor, i.code, std::move (i.operands))
                        : rw.append (*block, i.code, std::move (i.operands));
        if (!r) return invalid (f, r.error ().message);
        result.inserted_.push_back (*r);
      }
      // Simplify normally leaves a single RET, but direct machine preparation
      // and multiple JRET exits need a complete frame restoration at each exit.
      // Keep the usual single-exit case move-only, without extra allocations.
      auto restore = [&] (MachineInstruction &anchor, bool consume) -> Result<void> {
        for (auto &i : epilogue) {
          auto r = rw.insert_before (anchor, i.code, consume ? std::move (i.operands) : i.operands);
          if (!r) return invalid (f, r.error ().message);
          result.inserted_.push_back (*r);
        }
        return {};
      };
      for (auto anchor : earlier_returns) {
        auto r = restore (*anchor, false);
        if (!r) return r.error ();
      }
      if (last_return) {
        auto r = restore (*last_return, true);
        if (!r) return r.error ();
      }
    }
    // MIR's synthetic entry anchor follows incoming ABI copies. Earlier
    // analyses keep those copies in the first CFG block. After allocation,
    // expose the real flat anchor as a block entry;
    // BBV must not start its next thunk before those incoming copies.
    for (auto &label : f.labels ())
      if (label.is_function_entry ()) {
        auto &body = const_cast<MachineBasicBlock &> (label.block ());
        std::vector<MachineInstruction *> setup;
        for (auto &i : body.instructions ()) {
          if (!i.is_entry_setup ()) break;
          setup.push_back (&i);
        }
        if (!setup.empty ()) {
          auto before = rw.create_block_before (body);
          if (!before) return before.error ();
          for (auto i : setup) {
            auto moved = rw.move_to (*i, **before);
            if (!moved) return moved.error ();
          }
        }
      }
    auto state = f.state ();
    state.frame_formed = true;
    if (auto arm = std::get_if<MachineFunctionState::AArch64> (&state.target)) {
      arm->variadic_save_bytes = result.layout_.variadic_save_bytes;
      if (result.layout_.aggregate_bytes)
        arm->aggregate_save_bytes = result.layout_.aggregate_bytes;
    } else
      std::get<MachineFunctionState::X86> (state.target).variadic_save_bytes
        = result.layout_.variadic_save_bytes;
    auto committed = rw.set_state (state);
    if (!committed) return invalid (f, committed.error ().message);
    auto cfg = MachineControlFlow::build (f);
    if (!cfg) return invalid (f, cfg.error ().message);
    result.cfg_ = std::move (*cfg);
    result.owner_ = &f;
    result.revision_ = f.revision ();
    return std::move (result);
  }
};
}  // namespace detail
Result<MachineFrameResult> form_frame (MachineFunction &f, MachinePostResult post,
                                       MachineFrameOptions options) {
  if (f.target ().architecture () != target::Architecture::AArch64
      && f.target ().architecture () != target::Architecture::X86_64)
    return invalid (f, "unsupported target");
  if (!post.current (f) || !f.state ().post_allocation_optimized || f.state ().frame_formed)
    return invalid (f, "stale/foreign input or invalid phase");
  auto verified = f.verify ();
  if (!verified) return invalid (f, verified.error ().message);
  detail::FrameEngine engine (f, std::move (post));
  return engine.run (options);
}
}  // namespace mirnext::codegen
