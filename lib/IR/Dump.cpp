/* Licensed under the MIT License. Diagnostic output is distinct from MIR text serialization. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/IR.hpp"
#include "mirnext/IR/SSA.hpp"
#include <bit>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace mirnext::ir {
namespace {
std::string type_name (Type type) {
  constexpr std::string_view names[]{"bool", "i8", "u8", "i16", "u16", "i32", "u32",  "i64",
                                     "u64",  "f",  "d",  "ld",  "p",   "blk", "rblk", "undef"};
  auto n = static_cast<unsigned> (type.kind ());
  if (type.is_vector ()) {
    if (!type.valid ()) return "<invalid-vector-type>";
    constexpr std::string_view elements[]{"i8",  "u8",  "i16", "u16", "i32",
                                          "u32", "i64", "u64", "f32", "f64"};
    return "v128<" + std::string (elements[static_cast<unsigned> (type.vector_element ())]) + "x"
           + std::to_string (type.vector_lanes ()) + ">";
  }
  if (n >= std::size (names)) return "<invalid-type>";
  auto result = std::string (names[n]);
  if (type.is_block ())
    result += "(" + std::to_string (type.block_size ()) + ","
              + std::to_string (static_cast<unsigned> (type.block_class ())) + ")";
  return result;
}
struct Printer {
  std::ostringstream out;
  std::unordered_map<const Register *, std::size_t> registers;
  std::unordered_map<const BasicBlock *, std::size_t> blocks;
  std::unordered_map<const Label *, std::size_t> labels;
  std::unordered_map<const StackScope *, std::size_t> scopes;
  void signature (const Signature &signature) {
    out << '(';
    for (auto type : signature.parameters) out << type_name (type) << ',';
    if (signature.variadic) out << "...";
    out << ")->(";
    for (auto type : signature.results) out << type_name (type) << ',';
    out << ')';
  }
  void operand (const Operand &operand) {
    std::visit (
      [&] (const auto &value) {
        using T = std::decay_t<decltype (value)>;
        if constexpr (std::is_same_v<T, const Register *>)
          out << '%' << registers.at (value);
        else if constexpr (std::is_same_v<T, const Label *>)
          out << 'L' << labels.at (value);
        else if constexpr (std::is_same_v<T, SymbolReference>)
          out << '@' << std::quoted (std::string (detail::NodeAccess::symbol (value)->name ()))
              << '+' << value.displacement;
        else if constexpr (std::is_same_v<T, std::string>)
          out << std::quoted (value);
        else if constexpr (std::is_same_v<T, Memory>) {
          out << type_name (value.type) << ':' << value.displacement << '(';
          if (detail::NodeAccess::base (value))
            out << '%' << registers.at (detail::NodeAccess::base (value));
          out << ',';
          if (detail::NodeAccess::index (value))
            out << '%' << registers.at (detail::NodeAccess::index (value));
          out << ',' << value.scale << ')';
          if (!value.alias.empty () || !value.nonalias.empty ())
            out << '[' << std::quoted (value.alias) << ',' << std::quoted (value.nonalias) << ']';
          if (auto source = value.literal_source (); source.has_value ())
            out << " literal=@" << std::quoted (std::string (source.value ().symbol ().name ()));
        } else if constexpr (std::is_same_v<T, float>)
          out << "f:0x" << std::hex << std::bit_cast<std::uint32_t> (value) << std::dec;
        else if constexpr (std::is_same_v<T, double>)
          out << "d:0x" << std::hex << std::bit_cast<std::uint64_t> (value) << std::dec;
        else if constexpr (std::is_same_v<T, long double>)
          out << "ld:" << std::setprecision (std::numeric_limits<long double>::max_digits10)
              << value;
        else if constexpr (std::is_same_v<T, VectorConstant>) {
          out << type_name (value.type) << ":0x" << std::hex;
          for (auto byte : value.bytes)
            out << std::setw (2) << std::setfill ('0')
                << static_cast<unsigned> (std::to_integer<unsigned char> (byte));
          out << std::setfill (' ') << std::dec;
        } else if constexpr (std::is_same_v<T, std::uint64_t>)
          out << 'u' << value;
        else
          out << value;
      },
      detail::operand_access (operand).storage ());
  }
  void function (const Function &function) {
    registers.clear ();
    blocks.clear ();
    labels.clear ();
    scopes.clear ();
    for (auto &reg : function.registers ()) registers.emplace (&reg, registers.size ());
    for (auto &block : function.blocks ()) blocks.emplace (&block, blocks.size ());
    for (auto &label : function.labels ()) labels.emplace (&label, labels.size ());
    for (auto &scope : function.stack_scopes ()) scopes.emplace (&scope, scopes.size ());
    out << "function @" << std::quoted (std::string (function.symbol ().name ()));
    signature (function.signature ());
    out << " {\n";
    for (auto &scope : function.stack_scopes ()) {
      out << "  scope s" << scopes.at (&scope) << ' ' << std::quoted (std::string (scope.name ()));
      if (scope.parent ().get ()) out << " parent=s" << scopes.at (scope.parent ().get ());
      if (scope.materialized ()) out << " materialized";
      out << '\n';
    }
    for (auto &reg : function.registers ()) {
      out << "  %" << registers.at (&reg) << ' ' << type_name (reg.type ()) << ' '
          << std::quoted (std::string (reg.name ()));
      if (reg.is_argument ()) out << " argument";
      if (!reg.hardware_binding ().empty ())
        out << " hardware=" << std::quoted (std::string (reg.hardware_binding ()));
      if (reg.is_memory_backed ()) out << " memory-backed";
      out << '\n';
    }
    for (auto &block : function.blocks ()) {
      out << "bb" << blocks.at (&block) << ' ' << std::quoted (std::string (block.name ()));
      if (block.stack_scope ().get ()) out << " scope=s" << scopes.at (block.stack_scope ().get ());
      for (auto label : detail::ReferenceAccess::pointers (block.labels ()))
        out << " L" << labels.at (label) << '=' << std::quoted (std::string (label->name ()));
      out << ":\n";
      for (auto &instruction : block.instructions ()) {
        out << "  " << instruction_descriptor (instruction.opcode ()).get ()->name;
        for (auto &value : instruction.operands ()) {
          out << ' ';
          operand (value);
        }
        out << '\n';
      }
    }
    out << "}\n" << dump_ssa (function);
  }
};
}  // namespace
std::string dump (const Function &function) {
  Printer printer;
  printer.function (function);
  return printer.out.str ();
}
std::string dump (const Program &context) {
  Printer printer;
  for (auto &module : context.modules ()) {
    printer.out << "module " << std::quoted (std::string (module.name ())) << " {\n";
    for (auto &item : detail::NodeAccess::items (module))
      std::visit (
        [&] (auto value) {
          using T = decltype (value);
          if constexpr (std::is_same_v<T, const Function *>)
            printer.function (*value);
          else if constexpr (std::is_same_v<T, const GlobalData *>) {
            printer.out << (value->writable () ? "data @" : "constant @")
                        << std::quoted (std::string (value->symbol ().name ())) << ' ';
            std::visit (
              [&] (const auto &initializer) {
                using I = std::decay_t<decltype (initializer)>;
                if constexpr (std::is_same_v<I, InitializedData>) {
                  printer.out << type_name (initializer.type);
                  for (auto &element : initializer.values) {
                    printer.out << ' ';
                    printer.operand (element);
                  }
                } else if constexpr (std::is_same_v<I, ZeroData>)
                  printer.out << "zero " << initializer.size;
                else if constexpr (std::is_same_v<I, SymbolReference>)
                  printer.operand (Operand (initializer));
                else if constexpr (std::is_same_v<I, LabelReference>) {
                  printer.out
                    << "label @"
                    << std::quoted (std::string (
                         detail::NodeAccess::label (initializer)->function ().symbol ().name ()))
                    << ':'
                    << std::quoted (std::string (detail::NodeAccess::label (initializer)->name ()));
                  if (detail::NodeAccess::base (initializer))
                    printer.out << " - "
                                << std::quoted (
                                     std::string (detail::NodeAccess::base (initializer)->name ()));
                  printer.out << " + " << initializer.displacement;
                } else
                  printer.out << "expression @"
                              << std::quoted (std::string (
                                   detail::NodeAccess::function (initializer)->symbol ().name ()));
              },
              value->initializer ());
            if (auto variable = value->symbol ().variable (); variable.has_value ())
              printer.out << " variable=" << type_name (variable.value ().type ())
                          << (variable.value ().writable () ? " writable" : " readonly");
            if (value->is_literal_pool ()) printer.out << " literal-pool";
            printer.out << '\n';
          } else if constexpr (std::is_same_v<T, const Symbol *>) {
            printer.out << (value->kind () == Symbol::Kind::Import ? "import @" : "prototype @")
                        << std::quoted (std::string (value->name ()));
            if (value->signature ().has_value ()) printer.signature (value->signature ().value ());
            if (auto variable = value->variable (); variable.has_value ())
              printer.out << " variable=" << type_name (variable.value ().type ())
                          << (variable.value ().writable () ? " writable" : " readonly");
            printer.out << '\n';
          } else
            printer.out << (std::is_same_v<T, ForwardDeclaration> ? "forward @"
                            : std::is_same_v<T, DataDeclaration>  ? "data-declaration @"
                                                                  : "export @")
                        << std::quoted (std::string (detail::NodeAccess::symbol (value)->name ()))
                        << '\n';
        },
        item);
    printer.out << "}\n";
  }
  return printer.out.str ();
}
}  // namespace mirnext::ir
