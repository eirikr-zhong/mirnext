/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <ostream>
#include <utility>

namespace mirnext {

Operand::Operand(Kind kind, std::int64_t int_value, std::size_t label)
    : kind_(kind), int_value_(int_value), label_id_(label) {}

Operand Operand::int64(std::int64_t value) { return Operand(Kind::Int64, value, 0); }

Operand Operand::label_ref(std::size_t label_id) { return Operand(Kind::LabelRef, 0, label_id); }

Operand::Kind Operand::kind() const noexcept { return kind_; }

std::int64_t Operand::int64_value() const noexcept { return int_value_; }

std::size_t Operand::label_id() const noexcept { return label_id_; }

Instruction::Instruction(Opcode opcode, std::vector<Operand> operands)
    : opcode_(opcode), operands_(std::move(operands)) {}

Instruction::Opcode Instruction::opcode() const noexcept { return opcode_; }

const std::vector<Operand> &Instruction::operands() const noexcept { return operands_; }

std::size_t Instruction::label_id() const noexcept { return label_id_; }

void Instruction::set_label_id(std::size_t label_id) noexcept { label_id_ = label_id; }

Function::Function(std::string name) : name_(std::move(name)) {}

std::string_view Function::name() const noexcept { return name_; }

Instruction &Function::append(Instruction instruction) {
  instructions_.push_back(std::make_unique<Instruction>(std::move(instruction)));
  return *instructions_.back();
}

Instruction &Function::append_label() {
  Instruction instruction(Instruction::Opcode::Label);
  instruction.set_label_id(next_label_id_++);
  return append(std::move(instruction));
}

Instruction &Function::append_ret() { return append(Instruction(Instruction::Opcode::Ret)); }

std::size_t Function::instruction_count() const noexcept { return instructions_.size(); }

const std::vector<std::unique_ptr<Instruction>> &Function::instructions() const noexcept {
  return instructions_;
}

Module::Module(std::string name) : name_(std::move(name)) {}

std::string_view Module::name() const noexcept { return name_; }

Function &Module::new_function(std::string_view name) {
  functions_.push_back(std::make_unique<Function>(std::string(name)));
  return *functions_.back();
}

const std::vector<std::unique_ptr<Function>> &Module::functions() const noexcept {
  return functions_;
}

Module &Context::new_module(std::string_view name) {
  modules_.push_back(std::make_unique<Module>(std::string(name)));
  return *modules_.back();
}

const std::vector<std::unique_ptr<Module>> &Context::modules() const noexcept { return modules_; }

void Context::dump(std::ostream &out) const {
  for (const auto &module : modules_) {
    out << "module " << module->name() << '\n';
    for (const auto &function : module->functions()) {
      out << "  func " << function->name() << '\n';
      for (const auto &instruction : function->instructions()) {
        out << "    " << opcode_name(instruction->opcode());
        if (instruction->opcode() == Instruction::Opcode::Label) {
          out << " L" << instruction->label_id();
        }
        out << '\n';
      }
    }
  }
}

const char *opcode_name(Instruction::Opcode opcode) noexcept {
  switch (opcode) {
  case Instruction::Opcode::Label: return "label";
  case Instruction::Opcode::Ret: return "ret";
  case Instruction::Opcode::Nop: return "nop";
  }
  return "unknown";
}

} // namespace mirnext
