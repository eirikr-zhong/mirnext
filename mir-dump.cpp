/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <iomanip>
#include <ostream>

namespace mirnext {

static void dump_type_list(std::ostream &out, const std::vector<Type> &types) {
  if (types.empty()) {
    out << "void";
    return;
  }
  for (std::size_t i = 0; i < types.size(); ++i) {
    if (i != 0) out << ", ";
    out << type_name(types[i]);
  }
}

static void dump_parameters(std::ostream &out, const std::vector<Prototype::Parameter> &parameters) {
  for (std::size_t i = 0; i < parameters.size(); ++i) {
    if (i != 0) out << ", ";
    out << type_name(parameters[i].type) << " %" << parameters[i].name;
  }
}

static void dump_operand(std::ostream &out, const Function &function, const Operand &operand) {
  auto dump_register_id = [&out, &function](std::size_t register_id) {
    if (const Register *reg = function.find_register(register_id)) {
      out << '%' << reg->name();
    } else {
      out << "%r" << register_id;
    }
  };

  switch (operand.kind()) {
  case Operand::Kind::Poison:
    out << "<poison>";
    break;
  case Operand::Kind::Int64:
    out << operand.int64_value();
    break;
  case Operand::Kind::UInt64:
    out << operand.uint64_value();
    break;
  case Operand::Kind::Float32:
    out << std::setprecision(9) << operand.float32_value() << 'f';
    break;
  case Operand::Kind::Float64:
    out << std::setprecision(17) << operand.float64_value() << 'd';
    break;
  case Operand::Kind::LongDouble:
    out << std::setprecision(21) << operand.long_double_value() << "ld";
    break;
  case Operand::Kind::Register:
    dump_register_id(operand.register_id());
    break;
  case Operand::Kind::LabelRef:
    out << "L" << operand.label_id();
    break;
  case Operand::Kind::Memory:
    out << type_name(operand.memory_type()) << ":(";
    dump_register_id(operand.memory_base_register_id());
    out << ", ";
    dump_register_id(operand.memory_index_register_id());
    out << ", " << operand.memory_scale();
    if (operand.memory_displacement() != 0) out << ", " << operand.memory_displacement();
    out << ')';
    break;
  case Operand::Kind::ModuleSlot:
    out << "@slot" << operand.module_slot_id();
    break;
  case Operand::Kind::Reference:
    out << '@';
    switch (operand.reference_kind()) {
    case Operand::ReferenceKind::Prototype:
      if (operand.reference_pointer() != nullptr) {
        out << static_cast<const Prototype *>(operand.reference_pointer())->name();
      } else {
        out << "prototype";
      }
      break;
    case Operand::ReferenceKind::Import:
      if (operand.reference_pointer() != nullptr) {
        out << static_cast<const Import *>(operand.reference_pointer())->name();
      } else {
        out << "import";
      }
      break;
    case Operand::ReferenceKind::Function:
      if (operand.reference_pointer() != nullptr) {
        out << static_cast<const Function *>(operand.reference_pointer())->name();
      } else {
        out << "function";
      }
      break;
    case Operand::ReferenceKind::Data:
      if (operand.reference_pointer() != nullptr) {
        out << static_cast<const Data *>(operand.reference_pointer())->name();
      } else {
        out << "data";
      }
      break;
    case Operand::ReferenceKind::None:
      out << "ref";
      break;
    }
    break;
  }
}

void Context::dump(std::ostream &out) const {
  for (const auto &module : modules_) {
    out << "module " << module->name() << '\n';
    for (const auto &prototype : module->prototypes()) {
      out << "  proto " << prototype->name() << '(';
      dump_parameters(out, prototype->parameters());
      out << ") -> ";
      dump_type_list(out, prototype->return_types());
      out << '\n';
    }
    for (const auto &import : module->imports()) {
      out << "  import " << import->name() << '\n';
    }
    for (const auto &data : module->data_items()) {
      out << "  ";
      if (!data->name().empty()) out << data->name() << ": ";
      switch (data->kind()) {
      case Data::Kind::Bss:
        out << "bss " << data->size();
        break;
      case Data::Kind::Typed:
        out << "data " << type_name(data->element_type());
        for (const Operand &value : data->values()) {
          out << ' ';
          switch (value.kind()) {
          case Operand::Kind::Int64:
            out << value.int64_value();
            break;
          case Operand::Kind::UInt64:
            out << value.uint64_value();
            break;
          case Operand::Kind::Float32:
            out << std::setprecision(9) << value.float32_value() << 'f';
            break;
          case Operand::Kind::Float64:
            out << std::setprecision(17) << value.float64_value() << 'd';
            break;
          case Operand::Kind::LongDouble:
            out << std::setprecision(21) << value.long_double_value() << "ld";
            break;
          default:
            out << "<invalid>";
            break;
          }
        }
        break;
      case Data::Kind::String:
        out << "string \"" << data->string_value() << '"';
        break;
      case Data::Kind::Ref:
        out << "ref @";
        if (data->ref_target().pointer != nullptr) {
          switch (data->ref_target().kind) {
          case Operand::ReferenceKind::Prototype:
            out << static_cast<const Prototype *>(data->ref_target().pointer)->name();
            break;
          case Operand::ReferenceKind::Import:
            out << static_cast<const Import *>(data->ref_target().pointer)->name();
            break;
          case Operand::ReferenceKind::Function:
            out << static_cast<const Function *>(data->ref_target().pointer)->name();
            break;
          case Operand::ReferenceKind::Data:
            out << static_cast<const Data *>(data->ref_target().pointer)->name();
            break;
          case Operand::ReferenceKind::None:
            out << "ref";
            break;
          }
        } else {
          out << "ref";
        }
        out << ' ' << data->ref_target().displacement;
        break;
      }
      out << '\n';
    }
    for (const auto &function : module->functions()) {
      out << "  func " << function->name() << '(';
      for (std::size_t i = 0; i < function->arguments().size(); ++i) {
        if (i != 0) out << ", ";
        const Register &arg = function->arguments()[i];
        out << type_name(arg.type()) << " %" << arg.name();
      }
      out << ") -> ";
      dump_type_list(out, function->return_types());
      out << '\n';
      for (const Register &reg : function->local_registers()) {
        out << "    reg " << type_name(reg.type()) << " %" << reg.name() << '\n';
      }
      for (const auto &instruction : function->instructions()) {
        out << "    " << opcode_name(instruction->opcode());
        if (instruction->opcode() == Opcode::Label) {
          out << " L" << instruction->label_id();
        } else {
          for (const Operand &operand : instruction->operands()) {
            out << ' ';
            dump_operand(out, *function, operand);
          }
        }
        out << '\n';
      }
    }
  }
}

const char *opcode_name(Opcode opcode) noexcept {
  switch (opcode) {
  case Opcode::Label: return "label";
  case Opcode::Ret: return "ret";
  case Opcode::Nop: return "nop";
  case Opcode::Mov: return "mov";
  case Opcode::FMov: return "fmov";
  case Opcode::DMov: return "dmov";
  case Opcode::LDMov: return "ldmov";
  case Opcode::Ext8: return "ext8";
  case Opcode::Ext16: return "ext16";
  case Opcode::Ext32: return "ext32";
  case Opcode::UExt8: return "uext8";
  case Opcode::UExt16: return "uext16";
  case Opcode::UExt32: return "uext32";
  case Opcode::I2F: return "i2f";
  case Opcode::I2D: return "i2d";
  case Opcode::I2LD: return "i2ld";
  case Opcode::UI2F: return "ui2f";
  case Opcode::UI2D: return "ui2d";
  case Opcode::UI2LD: return "ui2ld";
  case Opcode::F2I: return "f2i";
  case Opcode::D2I: return "d2i";
  case Opcode::LD2I: return "ld2i";
  case Opcode::F2D: return "f2d";
  case Opcode::F2LD: return "f2ld";
  case Opcode::D2F: return "d2f";
  case Opcode::D2LD: return "d2ld";
  case Opcode::LD2F: return "ld2f";
  case Opcode::LD2D: return "ld2d";
  case Opcode::Addr: return "addr";
  case Opcode::Alloca: return "alloca";
  case Opcode::Neg: return "neg";
  case Opcode::Negs: return "negs";
  case Opcode::FNeg: return "fneg";
  case Opcode::DNeg: return "dneg";
  case Opcode::LDNeg: return "ldneg";
  case Opcode::Add: return "add";
  case Opcode::Adds: return "adds";
  case Opcode::FAdd: return "fadd";
  case Opcode::DAdd: return "dadd";
  case Opcode::LDAdd: return "ldadd";
  case Opcode::Sub: return "sub";
  case Opcode::Subs: return "subs";
  case Opcode::FSub: return "fsub";
  case Opcode::DSub: return "dsub";
  case Opcode::LDSub: return "ldsub";
  case Opcode::Mul: return "mul";
  case Opcode::Muls: return "muls";
  case Opcode::FMul: return "fmul";
  case Opcode::DMul: return "dmul";
  case Opcode::LDMul: return "ldmul";
  case Opcode::Div: return "div";
  case Opcode::Divs: return "divs";
  case Opcode::UDiv: return "udiv";
  case Opcode::UDivs: return "udivs";
  case Opcode::FDiv: return "fdiv";
  case Opcode::DDiv: return "ddiv";
  case Opcode::LDDiv: return "lddiv";
  case Opcode::Mod: return "mod";
  case Opcode::Mods: return "mods";
  case Opcode::UMod: return "umod";
  case Opcode::UMods: return "umods";
  case Opcode::And: return "and";
  case Opcode::Ands: return "ands";
  case Opcode::Or: return "or";
  case Opcode::Ors: return "ors";
  case Opcode::Xor: return "xor";
  case Opcode::Xors: return "xors";
  case Opcode::Lsh: return "lsh";
  case Opcode::Lshs: return "lshs";
  case Opcode::Rsh: return "rsh";
  case Opcode::Rshs: return "rshs";
  case Opcode::URsh: return "ursh";
  case Opcode::URshs: return "urshs";
  case Opcode::Eq: return "eq";
  case Opcode::Eqs: return "eqs";
  case Opcode::FEq: return "feq";
  case Opcode::DEq: return "deq";
  case Opcode::LDEq: return "ldeq";
  case Opcode::Ne: return "ne";
  case Opcode::Nes: return "nes";
  case Opcode::FNe: return "fne";
  case Opcode::DNe: return "dne";
  case Opcode::LDNe: return "ldne";
  case Opcode::Lt: return "lt";
  case Opcode::Lts: return "lts";
  case Opcode::ULt: return "ult";
  case Opcode::ULts: return "ults";
  case Opcode::FLt: return "flt";
  case Opcode::DLt: return "dlt";
  case Opcode::LDLt: return "ldlt";
  case Opcode::Le: return "le";
  case Opcode::Les: return "les";
  case Opcode::ULe: return "ule";
  case Opcode::ULes: return "ules";
  case Opcode::FLe: return "fle";
  case Opcode::DLe: return "dle";
  case Opcode::LDLe: return "ldle";
  case Opcode::Gt: return "gt";
  case Opcode::Gts: return "gts";
  case Opcode::UGt: return "ugt";
  case Opcode::UGts: return "ugts";
  case Opcode::FGt: return "fgt";
  case Opcode::DGt: return "dgt";
  case Opcode::LDGt: return "ldgt";
  case Opcode::Ge: return "ge";
  case Opcode::Ges: return "ges";
  case Opcode::UGe: return "uge";
  case Opcode::UGes: return "uges";
  case Opcode::FGe: return "fge";
  case Opcode::DGe: return "dge";
  case Opcode::LDGe: return "ldge";
  case Opcode::Addo: return "addo";
  case Opcode::Addos: return "addos";
  case Opcode::Subo: return "subo";
  case Opcode::Subos: return "subos";
  case Opcode::Mulo: return "mulo";
  case Opcode::Mulos: return "mulos";
  case Opcode::UMulo: return "umulo";
  case Opcode::UMulos: return "umulos";
  case Opcode::Jmp: return "jmp";
  case Opcode::Bt: return "bt";
  case Opcode::Bts: return "bts";
  case Opcode::Bf: return "bf";
  case Opcode::Bfs: return "bfs";
  case Opcode::Beq: return "beq";
  case Opcode::Beqs: return "beqs";
  case Opcode::FBeq: return "fbeq";
  case Opcode::DBeq: return "dbeq";
  case Opcode::LDBeq: return "ldbeq";
  case Opcode::Bne: return "bne";
  case Opcode::Bnes: return "bnes";
  case Opcode::FBne: return "fbne";
  case Opcode::DBne: return "dbne";
  case Opcode::LDBne: return "ldbne";
  case Opcode::Blt: return "blt";
  case Opcode::Blts: return "blts";
  case Opcode::UBlt: return "ublt";
  case Opcode::UBlts: return "ublts";
  case Opcode::FBlt: return "fblt";
  case Opcode::DBlt: return "dblt";
  case Opcode::LDBlt: return "ldblt";
  case Opcode::Ble: return "ble";
  case Opcode::Bles: return "bles";
  case Opcode::UBle: return "uble";
  case Opcode::UBles: return "ubles";
  case Opcode::FBle: return "fble";
  case Opcode::DBle: return "dble";
  case Opcode::LDBle: return "ldble";
  case Opcode::Bgt: return "bgt";
  case Opcode::Bgts: return "bgts";
  case Opcode::UBgt: return "ubgt";
  case Opcode::UBgts: return "ubgts";
  case Opcode::FBgt: return "fbgt";
  case Opcode::DBgt: return "dbgt";
  case Opcode::LDBgt: return "ldbgt";
  case Opcode::Bge: return "bge";
  case Opcode::Bges: return "bges";
  case Opcode::UBge: return "ubge";
  case Opcode::UBges: return "ubges";
  case Opcode::FBge: return "fbge";
  case Opcode::DBge: return "dbge";
  case Opcode::LDBge: return "ldbge";
  case Opcode::Bo: return "bo";
  case Opcode::UBo: return "ubo";
  case Opcode::Bno: return "bno";
  case Opcode::UBno: return "ubno";
  case Opcode::Call: return "call";
  case Opcode::Switch: return "switch";
  }
  return "unknown";
}

const char *type_name(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::B: return "b";
  case Type::Kind::I8: return "i8";
  case Type::Kind::U8: return "u8";
  case Type::Kind::I16: return "i16";
  case Type::Kind::U16: return "u16";
  case Type::Kind::I32: return "i32";
  case Type::Kind::U32: return "u32";
  case Type::Kind::I64: return "i64";
  case Type::Kind::U64: return "u64";
  case Type::Kind::F: return "f";
  case Type::Kind::D: return "d";
  case Type::Kind::LD: return "ld";
  case Type::Kind::P: return "p";
  }
  return "unknown";
}


} // namespace mirnext
