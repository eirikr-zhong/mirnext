/* Licensed under the MIT License. Bit-preserving vector canonicalization on SSA. */
#include "mirnext/Transforms/Vectorization.hpp"
#include "mirnext/Transforms/VectorUtilities.hpp"
#include <algorithm>
namespace mirnext::transforms {
namespace {
using namespace ir;
using detail::vector_input;
using detail::vector_output;
std::optional<std::uint64_t> integer (const SSAForm &form, const Instruction &i, std::size_t p) {
  const Operand *operand = &i.operands ()[p];
  if (auto value = vector_input (form, i, p)) {
    auto definition = value->instruction ().get ();
    if (!definition || definition->opcode () != Opcode::Mov) return {};
    operand = &definition->operands ()[1];
  }
  if (auto s = operand->get<std::int64_t> (); s.has_value ()) return std::uint64_t (s.value ());
  if (auto u = operand->get<std::uint64_t> (); u.has_value ()) return u.value ();
  return {};
}
const VectorConstant *mask (const SSAForm &form, const Instruction &i) {
  if (auto c = i.operands ()[3].get<VectorConstant> (); c.has_value ()) return c.get ();
  auto value = vector_input (form, i, 3);
  auto definition = value ? value->instruction ().get () : nullptr;
  return definition && definition->opcode () == Opcode::VMov
           ? definition->operands ()[1].get<VectorConstant> ().get ()
           : nullptr;
}
// Integer extraction performs an extension. Only a full-width lane can be
// replaced by the unextended scalar, even though both registers are i64.
bool exact_scalar (Type vector) {
  return vector.vector_lane_type ().is_float () || vector.vector_lanes () == 2;
}
struct State {
  SSAForm &form;
  SSAUpdater edit;
  std::size_t changes = 0;
  explicit State (SSAForm &s) : form (s), edit (s) {}
  Result<void> replace (Instruction &i, SSAValue *source) {
    auto output = vector_output (form, i);
    if (!source || !output || source->reg ().type () != output->reg ().type ()
        || !source->reg ().hardware_binding ().empty ()
        || !output->reg ().hardware_binding ().empty ())
      return {};
    auto checked = edit.replace_all_uses (*output, *source);
    if (!checked.has_value ()) return checked;
    checked = edit.erase_instruction (i);
    if (checked.has_value ()) ++changes;
    return checked;
  }
  Result<void> run (Function &function) {
    for (auto &block : function.blocks ()) {
      struct Common {
        Opcode opcode;
        Type type;
        SSAValue *input, *output;
      };
      std::unordered_map<SSAValue *, std::vector<Common>> common;
      std::vector<Instruction *> instructions;
      for (auto &i : block.instructions ()) instructions.push_back (&i);
      for (auto i : instructions) {
        if (!ir::detail::vector_opcode (i->opcode ())) continue;
        auto output = vector_output (form, *i), a = vector_input (form, *i, 1);
        if (!output || !a || !a->reg ().hardware_binding ().empty ()) continue;
        auto type = output->reg ().type ();
        auto producer = a->instruction ().get ();
        SSAValue *replacement = nullptr;
        if (i->opcode () == Opcode::VMov && type == a->reg ().type ()) replacement = a;
        if (i->opcode () == Opcode::VBitcast) {
          if (type == a->reg ().type ())
            replacement = a;
          else if (producer && producer->opcode () == Opcode::VBitcast) {
            auto original = vector_input (form, *producer, 1);
            if (!original) continue;
            if (type == original->reg ().type ())
              replacement = original;
            else {
              std::array operands{i->operands ()[0], Operand (original->reg ())};
              std::array inputs{original};
              auto checked = edit.replace (*i, Opcode::VBitcast, operands, inputs);
              if (!checked.has_value ()) return checked;
              a = original;
              ++changes;
            }
          }
        }
        if (i->opcode () == Opcode::VExtract && producer) {
          if (producer->opcode () == Opcode::VSplat && exact_scalar (a->reg ().type ()))
            replacement = vector_input (form, *producer, 1);
          if (producer->opcode () == Opcode::VInsert) {
            auto lane = integer (form, *i, 2), inserted = integer (form, *producer, 3);
            if (lane && lane == inserted && exact_scalar (a->reg ().type ()))
              replacement = vector_input (form, *producer, 2);
          }
        }
        if (i->opcode () == Opcode::VInsert) {
          auto scalar = vector_input (form, *i, 2);
          auto extraction = scalar ? scalar->instruction ().get () : nullptr;
          if (extraction && extraction->opcode () == Opcode::VExtract
              && vector_input (form, *extraction, 1) == a) {
            auto lane = integer (form, *i, 3), extracted = integer (form, *extraction, 2);
            if (lane && lane == extracted) replacement = a;
          }
        }
        if (i->opcode () == Opcode::VShuffle) {
          if (auto indices = mask (form, *i)) {
            unsigned lanes = type.vector_lanes ();
            bool left = true, right = true;
            for (unsigned n = 0; n < lanes; ++n) {
              auto index = std::to_integer<unsigned> (indices->bytes[n]);
              left &= index == n;
              right &= index == n + lanes;
            }
            if (left) replacement = a;
            if (right) replacement = vector_input (form, *i, 2);
          }
        }
        if (replacement) {
          auto checked = replace (*i, replacement);
          if (!checked.has_value ()) return checked;
          continue;
        }
        if (i->opcode () == Opcode::VSplat || i->opcode () == Opcode::VBitcast) {
          auto &bucket = common[a];
          auto previous = std::ranges::find_if (bucket, [&] (const Common &c) {
            return c.opcode == i->opcode () && c.type == type && c.input == a;
          });
          if (previous != bucket.end ()) {
            auto checked = replace (*i, previous->output);
            if (!checked.has_value ()) return checked;
          } else
            bucket.push_back ({i->opcode (), type, a, output});
        }
      }
    }
    return form.verify ();
  }
};
}  // namespace
Result<PassResult> VectorCombinePass::run (ir::MIRBuilder &edit,
                                           analysis::FunctionAnalysisManager &analyses) {
  auto checked = detail::vector_form (edit, analyses, name ());
  if (!checked.has_value ()) return checked.error ();
  State state (checked.value ());
  auto result = state.run (*edit.function ());
  if (!result.has_value ()) return result.error ();
  return PassResult{state.changes, detail::vector_preserved (state.changes)};
}
}  // namespace mirnext::transforms
