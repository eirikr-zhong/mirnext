#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
/* Licensed under the MIT License. MIR binary grammar derived from MIR,
   Copyright 2018-2024 Vladimir Makarov. Direct native IR construction. */
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Serialization/BinaryProtocol.hpp"
#include "mirnext/Serialization/Compression.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace mirnext::serialization {
namespace {
using namespace ir;
namespace wire = protocol;
struct Code {
  Opcode opcode;
  unsigned number;
};
constexpr Code codes[]{
#define MIRNEXT_OPCODE(name, number, spelling, roles, count, flow) {Opcode::name, number},
#include "mirnext/IR/Opcodes.def"
#undef MIRNEXT_OPCODE
};
std::optional<Opcode> opcode (std::uint64_t number) {
  auto found = std::lower_bound (std::begin (codes), std::end (codes), number,
                                 [] (Code a, auto b) { return a.number < b; });
  return found != std::end (codes) && found->number == number ? std::optional (found->opcode)
                                                              : std::nullopt;
}
unsigned type_code (Type type) {
  if (type.kind () == Type::Kind::Bool) return unsigned (wire::Type::I64);
  if (type.kind () == Type::Kind::Block)
    return unsigned (wire::Type::Block) + unsigned (type.block_class ());
  if (type.kind () == Type::Kind::ReturnBlock) return unsigned (wire::Type::ReturnBlock);
  if (type.kind () == Type::Kind::Undef) return unsigned (wire::Type::Undef);
  return unsigned (type.kind ()) - 1;
}
Type scalar_type (unsigned number) {
  if (number >= unsigned (wire::Type::Block) && number <= unsigned (wire::Type::Block4))
    return Type::block (0, static_cast<Type::BlockClass> (number - unsigned (wire::Type::Block)));
  if (number == unsigned (wire::Type::ReturnBlock)) return Type::return_block (0);
  if (number == unsigned (wire::Type::Undef)) return Type (Type::Kind::Undef);
  return Type (static_cast<Type::Kind> (number + 1));
}
// Names share wire string identities with literals containing the same trailing
// NUL. Look up that byte sequence without allocating a terminated temporary.
struct TerminatedName {
  std::string_view text;
};
struct WireStringHash {
  using is_transparent = void;
  std::size_t operator() (std::string_view bytes) const noexcept {
    if (!bytes.empty () && bytes.back () == '\0') bytes.remove_suffix (1);
    return std::hash<std::string_view>{}(bytes);
  }
  std::size_t operator() (TerminatedName name) const noexcept {
    return std::hash<std::string_view>{}(name.text);
  }
};
struct WireStringEqual {
  using is_transparent = void;
  bool operator() (std::string_view a, std::string_view b) const noexcept { return a == b; }
  bool operator() (std::string_view bytes, TerminatedName name) const noexcept {
    return bytes.size () == name.text.size () + 1 && bytes.back () == '\0'
           && bytes.substr (0, bytes.size () - 1) == name.text;
  }
  bool operator() (TerminatedName name, std::string_view bytes) const noexcept {
    return (*this) (bytes, name);
  }
};
class Writer {
 public:
  Result<std::vector<std::byte>> run (const Program &context) {
    auto valid = context.verify ();
    if (!(valid).has_value ()) return valid.error ();
    for (auto &module : context.modules ()) {
      auto result = write_module (module);
      if (!(result).has_value ()) return result.error ();
    }
    byte (wire::EOFILE);
    std::vector<std::byte> body = std::move (bytes_);
    bytes_.clear ();
    uint (wire::version);
    uint (strings_.size ());
    for (auto string : strings_) {
      uint (string->size ());
      bytes_.insert (bytes_.end (), reinterpret_cast<const std::byte *> (string->data ()),
                     reinterpret_cast<const std::byte *> (string->data () + string->size ()));
    }
    bytes_.insert (bytes_.end (), body.begin (), body.end ());
    return std::move (bytes_);
  }

 private:
  const Module *module_ = nullptr;
  const Function *function_ = nullptr;
  std::vector<std::byte> bytes_;
  // Unordered-map keys have stable addresses across rehash; the ordered table borrows them.
  std::vector<const std::string *> strings_;
  std::unordered_map<std::string, std::uint32_t, WireStringHash, WireStringEqual> string_ids_;
  std::unordered_set<std::string> used_names_;
  std::size_t next_name_ = 0;
  std::unordered_map<const Register *, std::string> registers_;
  std::unordered_map<const Label *, std::uint32_t> labels_;
  std::unordered_map<const Instruction *, std::string> call_signatures_;
  struct Expansion {
    std::size_t index;
    std::string name;
  };
  std::unordered_map<const Instruction *, std::vector<Expansion>> expansions_;
  Error invalid (std::string message) const {
    return {ErrorCode::InvalidOperand,
            std::string (module_ ? module_->name () : "binary")
              + (function_ ? "/" + std::string (function_->symbol ().name ()) : "")
              + ": binary-writer: " + std::move (message)};
  }
  std::string fresh (std::string_view prefix) {
    for (;;) {
      auto name = std::string (prefix) + std::to_string (next_name_++);
      if (used_names_.insert (name).second) return name;
    }
  }
  void byte (unsigned value) { bytes_.push_back (static_cast<std::byte> (value)); }
  void little (std::uint64_t value, unsigned count) {
    for (unsigned n = 0; n < count; ++n) {
      byte (value);
      value >>= 8;
    }
  }
  static unsigned width (std::uint64_t value) {
    unsigned count = 1;
    while (value >>= 8) ++count;
    return count;
  }
  void uint (std::uint64_t value) {
    if (value <= 127)
      byte (128 | unsigned (value));
    else {
      auto count = width (value);
      byte (wire::U1 + count - 1);
      little (value, count);
    }
  }
  void sint (std::int64_t value) {
    auto bits = std::bit_cast<std::uint64_t> (value);
    auto count = width (bits);
    byte (wire::I1 + count - 1);
    little (bits, count);
  }
  Result<void> string (std::string_view value, unsigned first, bool terminate = false) {
    auto found = terminate ? string_ids_.find (TerminatedName{value}) : string_ids_.find (value);
    if (found == string_ids_.end ()) {
      if (strings_.size () >= UINT32_MAX) return invalid ("string table exceeds binary width");
      auto number = static_cast<std::uint32_t> (strings_.size ());
      std::string owned (value);
      if (terminate) owned.push_back ('\0');
      found = string_ids_.emplace (std::move (owned), number).first;
      strings_.push_back (&found->first);
    }
    auto count = width (found->second);
    byte (first + count - 1);
    little (found->second, count);
    return {};
  }
  Result<void> name (std::string_view value, unsigned first = wire::NAME1) {
    return string (value, first, true);
  }
  void type (Type value) {
    if (value.is_vector ()) {
      byte (wire::VECTOR_TYPE);
      byte (static_cast<unsigned> (value.vector_element ()));
      byte (value.vector_lanes ());
    } else
      byte (wire::TYPE + type_code (value));
  }
  Result<void> reg (const Register *value) { return name (registers_.at (value), wire::REG1); }
  Result<void> label (const Label *value, bool signed_number = false) {
    auto found = labels_.find (value);
    if (found == labels_.end ()) return invalid ("undefined label");
    if (signed_number)
      sint (found->second);
    else {
      auto count = width (found->second);
      byte (wire::LAB1 + count - 1);
      little (found->second, count);
    }
    return {};
  }
  Result<void> memory (const Memory &value, std::string_view override_base = {}) {
    bool override = !override_base.empty ();
    bool base = override || ir::detail::NodeAccess::base (value),
         index = !override && ir::detail::NodeAccess::index (value);
    auto displacement = override ? std::int64_t{0} : value.displacement;
    if (value.type.is_block ()) displacement = static_cast<std::int64_t> (value.type.block_size ());
    unsigned form = displacement ? (base ? (index ? 6 : 3) : (index ? 4 : 0))
                                 : (base ? (index ? 5 : 1) : (index ? 2 : 0));
    bool aliased = !value.alias.empty () || !value.nonalias.empty ();
    byte ((aliased ? wire::ALIAS_DISP : wire::MEM_DISP) + form);
    type (value.type);
    if (displacement || (!base && !index)) sint (displacement);
    if (base) {
      auto result
        = override ? name (override_base, wire::REG1) : reg (ir::detail::NodeAccess::base (value));
      if (!(result).has_value ()) return result;
    }
    if (index) {
      auto result = reg (ir::detail::NodeAccess::index (value));
      if (!(result).has_value ()) return result;
      uint (value.scale);
    }
    if (aliased) {
      auto a = name (value.alias);
      if (!(a).has_value ()) return a;
      return name (value.nonalias);
    }
    return {};
  }
  Result<void> operand (const Operand &operand) {
    return std::visit (
      [&] (const auto &value) -> Result<void> {
        using T = std::decay_t<decltype (value)>;
        if constexpr (std::is_same_v<T, std::int64_t>)
          sint (value);
        else if constexpr (std::is_same_v<T, std::uint64_t>)
          uint (value);
        else if constexpr (std::is_same_v<T, float>) {
          byte (wire::F);
          little (std::bit_cast<std::uint32_t> (value), 4);
        } else if constexpr (std::is_same_v<T, double>) {
          byte (wire::D);
          little (std::bit_cast<std::uint64_t> (value), 8);
        } else if constexpr (std::is_same_v<T, long double>) {
          byte (wire::LD);
          auto bytes = wire::floating_bytes (value);
          bytes_.insert (bytes_.end (), bytes.begin (), bytes.end ());
        } else if constexpr (std::is_same_v<T, std::string>)
          return string (value, wire::STR1);
        else if constexpr (std::is_same_v<T, const Register *>)
          return reg (value);
        else if constexpr (std::is_same_v<T, const Label *>)
          return label (value);
        else if constexpr (std::is_same_v<T, VectorConstant>) {
          byte (wire::VECTOR_LITERAL);
          byte (static_cast<unsigned> (value.type.vector_element ()));
          byte (value.type.vector_lanes ());
          bytes_.insert (bytes_.end (), value.bytes.begin (), value.bytes.end ());
        } else if constexpr (std::is_same_v<T, Memory>)
          return memory (value);
        else {
          if (value.displacement) return invalid ("unmaterialized symbol displacement");
          return name (ir::detail::NodeAccess::symbol (value)->name ());
        }
        return {};
      },
      ir::detail::operand_access (operand).storage ());
  }
  Result<void> signature (const Signature &value, const Function *function = nullptr) {
    uint (value.variadic);
    uint (value.results.size ());
    for (auto result : value.results) type (result);
    for (std::size_t n = 0; n < value.parameters.size (); ++n) {
      auto parameter = value.parameters[n];
      type (parameter);
      auto named = name (
        function ? registers_.at (ir::detail::ReferenceAccess::pointers (function->arguments ())[n])
                 : ".arg" + std::to_string (n));
      if (!(named).has_value ()) return named;
      if (parameter.is_block ()) uint (parameter.block_size ());
    }
    byte (wire::EOI);
    return {};
  }
  Result<void> instruction (const Instruction &instruction) {
    auto found = expansions_.find (&instruction);
    auto expansions = found == expansions_.end () ? std::span<const Expansion>{}
                                                  : std::span<const Expansion> (found->second);
    for (auto &expansion : expansions) {
      auto &value = instruction.operands ()[expansion.index];
      auto destination = [&] () { return name (expansion.name, wire::REG1); };
      if (auto symbol = ir::detail::operand_access (value).get<SymbolReference> ()) {
        uint (codes[std::size_t (Opcode::Mov)].number);
        auto d = destination ();
        if (!(d).has_value ()) return d;
        auto s = name (ir::detail::NodeAccess::symbol (*(symbol))->name ());
        if (!(s).has_value ()) return s;
        uint (codes[std::size_t (Opcode::Add)].number);
        d = destination ();
        if (!(d).has_value ()) return d;
        d = destination ();
        if (!(d).has_value ()) return d;
        sint (symbol->displacement);
      } else {
        auto &memory = *ir::detail::operand_access (value).get<Memory> ();
        uint (codes[std::size_t (Opcode::Mov)].number);
        auto d = destination ();
        if (!(d).has_value ()) return d;
        if (ir::detail::NodeAccess::index (memory)) {
          auto source = reg (ir::detail::NodeAccess::index (memory));
          if (!(source).has_value ()) return source;
        } else
          sint (0);
        if (ir::detail::NodeAccess::index (memory) && memory.scale != 1) {
          uint (codes[std::size_t (Opcode::Mul)].number);
          d = destination ();
          if (!(d).has_value ()) return d;
          d = destination ();
          if (!(d).has_value ()) return d;
          sint (memory.scale);
        }
        if (ir::detail::NodeAccess::base (memory)) {
          uint (codes[std::size_t (Opcode::Add)].number);
          d = destination ();
          if (!(d).has_value ()) return d;
          d = destination ();
          if (!(d).has_value ()) return d;
          auto source = reg (ir::detail::NodeAccess::base (memory));
          if (!(source).has_value ()) return source;
        }
        uint (codes[std::size_t (Opcode::Add)].number);
        d = destination ();
        if (!(d).has_value ()) return d;
        d = destination ();
        if (!(d).has_value ()) return d;
        sint (memory.displacement);
      }
    }
    uint (codes[std::size_t (instruction.opcode ())].number);
    for (std::size_t n = 0; n < instruction.operands ().size (); ++n) {
      Result<void> result;
      if (!expansions.empty () && expansions.front ().index == n) {
        auto expansion = expansions.front ();
        expansions = expansions.subspan (1);
        if (auto m = ir::detail::operand_access (instruction.operands ()[n]).get<Memory> ())
          result = memory (*m, expansion.name);
        else
          result = name (expansion.name, wire::REG1);
      } else if (!n
                 && instruction_descriptor (instruction.opcode ()).get ()->count
                      == OperandCount::Call)
        result = name (call_signatures_.at (&instruction));
      else
        result = operand (instruction.operands ()[n]);
      if (!(result).has_value ()) return result;
    }
    if (instruction_descriptor (instruction.opcode ()).get ()->count != OperandCount::Fixed)
      byte (wire::EOI);
    return {};
  }
  Result<void> function (const Function &value) {
    function_ = &value;
    auto kind = name ("func");
    if (!(kind).has_value ()) return kind;
    auto named = name (value.symbol ().name ());
    if (!(named).has_value ()) return named;
    auto sig = signature (value.signature (), &value);
    if (!(sig).has_value ()) return sig;
    for (bool hardware : {false, true}) {
      bool opened = false;
      for (auto &reg : value.registers ())
        if (!reg.is_argument () && (!reg.hardware_binding ().empty ()) == hardware) {
          if (!opened) {
            auto k = name (hardware ? "global" : "local");
            if (!(k).has_value ()) return k;
            opened = true;
          }
          type (reg.type ().is_integer () ? Type::i64 () : reg.type ());
          auto n = name (registers_.at (&reg));
          if (!(n).has_value ()) return n;
          if (hardware) {
            n = name (reg.hardware_binding ());
            if (!(n).has_value ()) return n;
          }
        }
      if (!hardware)
        for (auto &block : value.blocks ())
          for (auto &instruction : block.instructions ()) {
            auto found = expansions_.find (&instruction);
            if (found == expansions_.end ()) continue;
            for (auto &expansion : found->second) {
              if (!opened) {
                auto n = name ("local");
                if (!(n).has_value ()) return n;
                opened = true;
              }
              type (Type::i64 ());
              auto n = name (expansion.name);
              if (!(n).has_value ()) return n;
            }
          }
      if (opened) byte (wire::EOI);
    }
    for (auto &block : value.blocks ()) {
      for (auto anchor : ir::detail::ReferenceAccess::pointers (block.labels ())) {
        auto written = label (anchor);
        if (!(written).has_value ()) return written;
      }
      for (auto &i : block.instructions ()) {
        auto written = instruction (i);
        if (!(written).has_value ()) return written;
      }
    }
    function_ = nullptr;
    return name ("endfunc");
  }
  Result<void> data (const GlobalData &value) {
    auto named = !value.symbol ().name ().empty ();
    return std::visit (
      [&] (const auto &initializer) -> Result<void> {
        using T = std::decay_t<decltype (initializer)>;
        auto kind = std::is_same_v<T, InitializedData>   ? "data"
                    : std::is_same_v<T, ZeroData>        ? "bss"
                    : std::is_same_v<T, SymbolReference> ? "ref"
                    : std::is_same_v<T, LabelReference>  ? "lref"
                                                         : "expr";
        auto start = name ((named ? "n" : "") + std::string (kind));
        if (!(start).has_value ()) return start;
        if (named) {
          auto n = name (value.symbol ().name ());
          if (!(n).has_value ()) return n;
        }
        if constexpr (std::is_same_v<T, InitializedData>) {
          auto element_type = initializer.type.kind () == Type::Kind::Pointer
                                ? Type (Type::Kind::U64)
                                : initializer.type;
          type (element_type);
          for (auto &element : initializer.values) {
            if (element_type.is_integer ()) {
              auto bits
                = ir::detail::operand_access (element).template get<std::uint64_t> ()
                    ? *ir::detail::operand_access (element).template get<std::uint64_t> ()
                    : std::bit_cast<std::uint64_t> (
                        *ir::detail::operand_access (element).template get<std::int64_t> ());
              switch (element_type.kind ()) {
              case Type::Kind::I8: sint (static_cast<std::int8_t> (bits)); break;
              case Type::Kind::U8: uint (static_cast<std::uint8_t> (bits)); break;
              case Type::Kind::I16: sint (static_cast<std::int16_t> (bits)); break;
              case Type::Kind::U16: uint (static_cast<std::uint16_t> (bits)); break;
              case Type::Kind::I32: sint (static_cast<std::int32_t> (bits)); break;
              case Type::Kind::U32: uint (static_cast<std::uint32_t> (bits)); break;
              case Type::Kind::U64: uint (bits); break;
              default: sint (std::bit_cast<std::int64_t> (bits)); break;
              }
            } else {
              auto written = operand (element);
              if (!written.has_value ()) return written;
            }
          }
          byte (wire::EOI);
        } else if constexpr (std::is_same_v<T, ZeroData>)
          uint (initializer.size);
        else if constexpr (std::is_same_v<T, SymbolReference>) {
          auto target = name (ir::detail::NodeAccess::symbol (initializer)->name ());
          if (!target.has_value ()) return target;
          sint (initializer.displacement);
        } else if constexpr (std::is_same_v<T, LabelReference>) {
          auto first = label (ir::detail::NodeAccess::label (initializer), true);
          if (!first.has_value ()) return first;
          if (ir::detail::NodeAccess::base (initializer)) {
            auto second = label (ir::detail::NodeAccess::base (initializer), true);
            if (!second.has_value ()) return second;
          } else
            sint (-1);
          sint (initializer.displacement);
        } else
          return name (ir::detail::NodeAccess::function (initializer)->symbol ().name ());
        return {};
      },
      value.initializer ());
  }
  Result<void> write_module (const Module &module) {
    module_ = &module;
    function_ = nullptr;
    next_name_ = 0;
    used_names_.clear ();
    registers_.clear ();
    labels_.clear ();
    call_signatures_.clear ();
    expansions_.clear ();
    std::unordered_set<const Function *> expressions;
    for (auto &symbol : module.symbols ()) used_names_.emplace (symbol.name ());
    for (auto &data : module.data ())
      if (auto value = std::get_if<ExpressionData> (&data.initializer ()))
        expressions.insert (ir::detail::NodeAccess::function (*(value)));
    std::vector<const Function *> functions;
    for (bool expression : {true, false})
      for (auto &item : ir::detail::NodeAccess::items (module))
        if (auto f = std::get_if<const Function *> (&item);
            f && expressions.contains (*f) == expression)
          functions.push_back (*f);
    std::uint64_t label_number = 0;
    for (auto f : functions) {
      std::unordered_set<std::string> register_names;
      std::size_t next_register = 0;
      for (auto &reg : f->registers ()) {
        auto n = std::string (reg.name ());
        if (n.empty () || register_names.contains (n)) do {
            n = ".r" + std::to_string (next_register++);
          } while (register_names.contains (n));
        register_names.insert (n);
        used_names_.insert (n);
        registers_.emplace (&reg, std::move (n));
      }
      for (auto &block : f->blocks ())
        for (auto label : ir::detail::ReferenceAccess::pointers (block.labels ())) {
          if (label_number > UINT32_MAX) return invalid ("label count exceeds binary width");
          labels_.emplace (label, label_number++);
        }
    }
    std::unordered_map<const Signature *, std::string> generated_signatures;
    std::vector<const Signature *> signature_order;
    std::unordered_set<const Symbol *> visible;
    for (auto &item : ir::detail::NodeAccess::items (module)) {
      if (auto symbol = std::get_if<const Symbol *> (&item);
          symbol && (*symbol)->kind () == Symbol::Kind::Prototype)
        visible.insert (*symbol);
      auto f = std::get_if<const Function *> (&item);
      if (!f) continue;
      for (auto &block : (*f)->blocks ())
        for (auto &instruction : block.instructions ()) {
          for (std::size_t n = 0; n < instruction.operands ().size (); ++n) {
            auto &value = instruction.operands ()[n];
            if ((ir::detail::operand_access (value).get<SymbolReference> ()
                 && ir::detail::operand_access (value).get<SymbolReference> ()->displacement)
                || (ir::detail::operand_access (value).get<Memory> ()
                    && ir::detail::operand_access (value).get<Memory> ()->type.is_block ()
                    && ir::detail::operand_access (value).get<Memory> ()->displacement))
              expansions_[&instruction].push_back ({n, fresh (".addr")});
          }
          if (instruction_descriptor (instruction.opcode ()).get ()->count != OperandCount::Call)
            continue;
          auto symbol = ir::detail::NodeAccess::symbol (
            *(ir::detail::operand_access (instruction.operands ()[0]).get<SymbolReference> ()));
          if (visible.contains (symbol)) {
            call_signatures_.emplace (&instruction, std::string (symbol->name ()));
            continue;
          }
          auto signature = symbol->signature ().get ();
          auto found = generated_signatures.find (signature);
          if (found == generated_signatures.end ()) {
            found = generated_signatures.emplace (signature, fresh (".sig")).first;
            signature_order.push_back (signature);
          }
          call_signatures_.emplace (&instruction, found->second);
        }
    }
    auto start = name ("module");
    if (!(start).has_value ()) return start;
    auto n = name (module.name ());
    if (!(n).has_value ()) return n;
    for (auto &symbol : module.symbols ()) {
      if (symbol.kind () == Symbol::Kind::Import) {
        auto k = name ("import");
        if (!(k).has_value ()) return k;
        n = name (symbol.name ());
        if (!(n).has_value ()) return n;
      } else if (symbol.kind () == Symbol::Kind::Function
                 || (symbol.kind () == Symbol::Kind::Data && !symbol.name ().empty ())) {
        auto k = name ("forward");
        if (!(k).has_value ()) return k;
        n = name (symbol.name ());
        if (!(n).has_value ()) return n;
      }
    }
    for (auto value : signature_order) {
      auto k = name ("proto");
      if (!(k).has_value ()) return k;
      n = name (generated_signatures.at (value));
      if (!(n).has_value ()) return n;
      auto s = signature (*value);
      if (!(s).has_value ()) return s;
    }
    for (auto f : functions)
      if (expressions.contains (f)) {
        auto written = function (*f);
        if (!(written).has_value ()) return written;
      }
    bool last_data = false, skipped = false;
    for (auto &item : ir::detail::NodeAccess::items (module)) {
      if (std::holds_alternative<ForwardDeclaration> (item)
          || (std::holds_alternative<const Symbol *> (item)
              && std::get<const Symbol *> (item)->kind () == Symbol::Kind::Import)
          || (std::holds_alternative<const Function *> (item)
              && expressions.contains (std::get<const Function *> (item)))) {
        skipped = true;
        continue;
      }
      auto datum = std::get_if<const GlobalData *> (&item);
      if (datum && (*datum)->symbol ().name ().empty () && last_data && skipped) {
        auto k = name ("proto");
        if (!(k).has_value ()) return k;
        n = name (fresh (".boundary"));
        if (!(n).has_value ()) return n;
        auto s = signature ({});
        if (!(s).has_value ()) return s;
      }
      last_data = datum != nullptr;
      skipped = false;
      auto result = std::visit (
        [&] (auto value) -> Result<void> {
          using T = decltype (value);
          if constexpr (std::is_same_v<T, const Function *>)
            return function (*value);
          else if constexpr (std::is_same_v<T, const GlobalData *>)
            return data (*value);
          else if constexpr (std::is_same_v<T, const Symbol *>) {
            auto k = name ("proto");
            if (!(k).has_value ()) return k;
            auto n = name (value->name ());
            if (!n.has_value ()) return n;
            return signature (value->signature ().value ());
          } else if constexpr (std::is_same_v<T, ExportDeclaration>) {
            auto k = name ("export");
            if (!(k).has_value ()) return k;
            return name (ir::detail::NodeAccess::symbol (value)->name ());
          } else
            return invalid ("unmaterialized data declaration");
        },
        item);
      if (!(result).has_value ()) return result;
    }
    return name ("endmodule");
  }
};
struct RegisterName {
  std::string_view name;
};
struct SymbolName {
  std::string_view name;
};
struct LabelNumber {
  std::uint32_t number;
};
struct ParsedMemory {
  Memory value;
  std::optional<std::string_view> base, index;
};
using ParsedOperand = std::variant<Operand, RegisterName, SymbolName, LabelNumber, ParsedMemory>;
struct ParsedInstruction {
  Opcode opcode;
  std::vector<std::uint32_t> labels;
  std::vector<ParsedOperand> operands;
  BasicBlock *block = nullptr;
};
struct ParsedFunction {
  Signature signature;
  std::vector<std::string_view> arguments;
  struct Local {
    Type type;
    std::string_view name, hardware;
  };
  std::vector<Local> locals;
  std::vector<ParsedInstruction> instructions;
  Function *function = nullptr;
};
struct ReferenceData {
  std::string_view name;
  std::int64_t displacement;
  bool expression;
};
struct LabelData {
  std::uint32_t label;
  std::optional<std::uint32_t> base;
  std::int64_t displacement;
};
using ParsedInitializer = std::variant<InitializedData, ZeroData, ReferenceData, LabelData>;
struct Record {
  enum class Kind { Function, Prototype, Import, Forward, Export, Data };
  Kind kind;
  std::string_view name;
  std::variant<std::monostate, ParsedFunction, ParsedInitializer> body;
  Symbol *symbol = nullptr;
};
class Reader {
 public:
  Reader (std::span<const std::byte> bytes, BinaryOptions options)
    : bytes_ (bytes), options_ (options), context_ () {}
  Result<Program> run () {
    if (bytes_.size () > options_.max_bytes) return invalid ("input byte limit exceeded");
    auto version = uint ();
    if (!(version).has_value ()) return version.error ();
    if ((version).value () > wire::version) return invalid ("unsupported binary version");
    auto count = uint ();
    if (!(count).has_value ()) return count.error ();
    if ((count).value () > options_.max_strings || (count).value () > UINT32_MAX
        || (count).value () > bytes_.size () - position_)
      return invalid ("string table limit or truncated input");
    strings_.reserve ((count).value ());
    for (std::uint64_t n = 0; n < (count).value (); ++n) {
      auto size = uint ();
      if (!(size).has_value ()) return size.error ();
      if ((size).value () > bytes_.size () - position_) return invalid ("truncated string table");
      strings_.emplace_back (reinterpret_cast<const char *> (bytes_.data () + position_),
                             static_cast<std::size_t> ((size).value ()));
      position_ += (size).value ();
    }
    for (;;) {
      auto first = token ();
      if (!(first).has_value ()) return first.error ();
      if (first.value ().tag == wire::EOFILE) {
        if (position_ != bytes_.size ()) return invalid ("trailing bytes");
        auto valid = context_.verify ();
        if (!(valid).has_value ()) return diagnose (valid.error ());
        return std::move (context_);
      }
      auto kind = name ((first).value ());
      if (!(kind).has_value ()) return kind.error ();
      if ((kind).value () != "module") return invalid ("expected module record");
      auto module_name = name ();
      if (!(module_name).has_value ()) return module_name.error ();
      auto created = ir::detail::access (context_).create_module ((module_name).value ());
      if (!(created).has_value ()) return diagnose (created.error ());
      module_ = &(created).value ();
      auto parsed = parse_module ();
      if (!(parsed).has_value ()) return parsed.error ();
      auto built = build ();
      if (!(built).has_value ()) return built.error ();
      module_ = nullptr;
    }
  }

 private:
  struct Token {
    unsigned tag;
    std::uint64_t bits = 0;
    std::array<std::byte, 16> extended{};
  };
  std::span<const std::byte> bytes_;
  BinaryOptions options_;
  std::size_t position_ = 0, instruction_count_ = 0;
  Program context_;
  Module *module_ = nullptr;
  std::string_view function_name_;
  std::vector<std::string_view> strings_;
  std::vector<Record> records_;
  std::unordered_map<std::uint32_t, const Label *> labels_;
  Error diagnose (Error error) const {
    error.message = std::string (options_.source_name) + ":" + std::to_string (position_) + ": "
                    + (module_ ? std::string (module_->name ()) + "/" : "")
                    + std::string (function_name_) + ": binary-reader: " + error.message;
    return error;
  }
  Error invalid (std::string message) const {
    return diagnose ({ErrorCode::InvalidOperand, std::move (message)});
  }
  Result<unsigned> byte () {
    if (position_ == bytes_.size ()) return invalid ("truncated input");
    return std::to_integer<unsigned> (bytes_[position_++]);
  }
  Result<std::uint64_t> little (unsigned count) {
    if (count > bytes_.size () - position_) return invalid ("truncated value");
    std::uint64_t value = 0;
    for (unsigned n = 0; n < count; ++n)
      value |= std::uint64_t (std::to_integer<unsigned> (bytes_[position_++])) << (n * 8);
    return value;
  }
  Result<Token> token () {
    auto tag = byte ();
    if (!(tag).has_value ()) return tag.error ();
    Token result{(tag).value ()};
    if ((tag).value () & 128) {
      result.tag = wire::U0;
      result.bits = (tag).value () & 127;
      return result;
    }
    unsigned count = 0;
    if ((tag).value () >= wire::U1 && (tag).value () <= wire::U8)
      count = (tag).value ();
    else if ((tag).value () >= wire::I1 && (tag).value () <= wire::I8)
      count = (tag).value () - wire::I1 + 1;
    else if ((tag).value () >= wire::REG1 && (tag).value () <= wire::LAB4)
      count = ((tag).value () - wire::REG1) % 4 + 1;
    else if ((tag).value () == wire::F)
      count = 4;
    else if ((tag).value () == wire::D)
      count = 8;
    else if ((tag).value () == wire::LD) {
      if (bytes_.size () - position_ < result.extended.size ())
        return invalid ("truncated long double");
      std::memcpy (result.extended.data (), bytes_.data () + position_, result.extended.size ());
      position_ += result.extended.size ();
      return result;
    } else if ((tag).value () == wire::VECTOR_TYPE) {
      auto element = byte ();
      if (!(element).has_value ()) return element.error ();
      auto lanes = byte ();
      if (!(lanes).has_value ()) return lanes.error ();
      result.bits = (std::uint64_t ((element).value ()) << 8) | lanes.value ();
      return result;
    } else if ((tag).value () == wire::VECTOR_LITERAL) {
      auto element = byte ();
      if (!(element).has_value ()) return element.error ();
      auto lanes = byte ();
      if (!(lanes).has_value ()) return lanes.error ();
      if (bytes_.size () - position_ < result.extended.size ())
        return invalid ("truncated v128 literal");
      result.bits = (std::uint64_t ((element).value ()) << 8) | lanes.value ();
      std::memcpy (result.extended.data (), bytes_.data () + position_, result.extended.size ());
      position_ += result.extended.size ();
      return result;
    } else if (((tag).value () >= wire::MEM_DISP && (tag).value () <= wire::EOFILE)
               || ((tag).value () >= wire::ALIAS_DISP && (tag).value () <= wire::ALIAS_LAST))
      return result;
    else
      return invalid ("invalid tag");
    auto value = little (count);
    if (!(value).has_value ()) return value.error ();
    result.bits = (value).value ();
    return result;
  }
  Result<std::uint64_t> uint () {
    auto value = token ();
    if (!(value).has_value ()) return value.error ();
    if (value.value ().tag > wire::U8) return invalid ("expected unsigned integer");
    return value.value ().bits;
  }
  Result<std::int64_t> sint () {
    auto value = token ();
    if (!(value).has_value ()) return value.error ();
    if (value.value ().tag < wire::I1 || value.value ().tag > wire::I8)
      return invalid ("expected signed integer");
    return std::bit_cast<std::int64_t> (value.value ().bits);
  }
  Result<std::string_view> string (std::uint64_t id) {
    if (id >= strings_.size ()) return invalid ("invalid string identity");
    return strings_[id];
  }
  Result<std::string_view> terminated (std::uint64_t id) {
    auto value = string (id);
    if (!(value).has_value ()) return value.error ();
    if (value.value ().empty () || value.value ().back () != '\0'
        || value.value ().find ('\0') != value.value ().size () - 1)
      return invalid ("name is not a terminated string");
    return value.value ().substr (0, value.value ().size () - 1);
  }
  Result<std::string_view> name (Token token) {
    if (token.tag < wire::NAME1 || token.tag > wire::NAME4) return invalid ("expected name");
    return terminated (token.bits);
  }
  Result<std::string_view> name () {
    auto value = token ();
    if (!(value).has_value ()) return value.error ();
    return name ((value).value ());
  }
  Result<Type> type (Token token, bool undef = false) {
    if (token.tag == wire::VECTOR_TYPE) {
      auto element = static_cast<unsigned> (token.bits >> 8);
      auto lanes = static_cast<unsigned> (token.bits & 0xff);
      if (element > static_cast<unsigned> (Type::VectorElement::F64) || lanes == 0)
        return invalid ("invalid v128 type");
      auto result = Type::vector128 (static_cast<Type::VectorElement> (element), lanes);
      if (!result.valid ()) return invalid ("invalid v128 type");
      return result;
    }
    auto last = unsigned (undef ? wire::Type::Undef : wire::Type::ReturnBlock);
    if (token.tag < wire::TYPE || token.tag > wire::TYPE + last)
      return invalid ("invalid type tag");
    return scalar_type (token.tag - wire::TYPE);
  }
  Result<Type> type (bool undef = false) {
    auto value = token ();
    if (!(value).has_value ()) return value.error ();
    return type ((value).value (), undef);
  }
  Result<ParsedFunction> signature (bool body) {
    ParsedFunction result;
    auto variadic = uint ();
    if (!(variadic).has_value ()) return variadic.error ();
    if ((variadic).value () > 1) return invalid ("invalid variadic flag");
    result.signature.variadic = (variadic).value ();
    auto count = uint ();
    if (!(count).has_value ()) return count.error ();
    if ((count).value () > bytes_.size () - position_) return invalid ("invalid result count");
    for (std::uint64_t n = 0; n < (count).value (); ++n) {
      auto result_type = type ();
      if (!(result_type).has_value ()) return result_type.error ();
      if (!result_type.value ().is_scalar () && !result_type.value ().is_vector ())
        return invalid ("function result must be scalar or vector");
      result.signature.results.push_back ((result_type).value ());
    }
    for (;;) {
      auto value = token ();
      if (!(value).has_value ()) return value.error ();
      if (value.value ().tag == wire::EOI) break;
      auto parameter = type ((value).value ());
      if (!(parameter).has_value ()) return parameter.error ();
      auto argument = name ();
      if (!(argument).has_value ()) return argument.error ();
      if (parameter.value ().is_block ()) {
        auto size = uint ();
        if (!(size).has_value ()) return size.error ();
        if ((size).value () > INT64_MAX || !(size).value ())
          return invalid ("invalid block argument size");
        (parameter).value () = parameter.value ().kind () == Type::Kind::ReturnBlock
                                 ? Type::return_block ((size).value ())
                                 : Type::block ((size).value (), parameter.value ().block_class ());
      }
      if (body && argument.value ().empty ()) return invalid ("empty function argument name");
      result.signature.parameters.push_back ((parameter).value ());
      result.arguments.push_back ((argument).value ());
    }
    auto valid = result.signature.verify ();
    if (!(valid).has_value ()) return diagnose (valid.error ());
    return result;
  }
  Result<Operand> literal (Token token) {
    if (token.tag == wire::VECTOR_LITERAL) {
      auto element = static_cast<unsigned> (token.bits >> 8);
      auto lanes = static_cast<unsigned> (token.bits & 0xff);
      if (element > static_cast<unsigned> (Type::VectorElement::F64) || lanes == 0)
        return invalid ("invalid v128 literal type");
      auto type = Type::vector128 (static_cast<Type::VectorElement> (element), lanes);
      if (!type.valid ()) return invalid ("invalid v128 literal type");
      return Operand (VectorConstant{type, token.extended});
    }
    if (token.tag <= wire::U8) return Operand (token.bits);
    if (token.tag <= wire::I8) return Operand (std::bit_cast<std::int64_t> (token.bits));
    if (token.tag == wire::F)
      return Operand (std::bit_cast<float> (static_cast<std::uint32_t> (token.bits)));
    if (token.tag == wire::D) return Operand (std::bit_cast<double> (token.bits));
    if (token.tag == wire::LD) {
      long double value{};
      std::memcpy (&value, token.extended.data (), wire::long_double_bytes);
      return Operand (value);
    }
    if (token.tag >= wire::STR1 && token.tag <= wire::STR4) {
      auto value = string (token.bits);
      if (!(value).has_value ()) return value.error ();
      return Operand (std::string ((value).value ()));
    }
    return invalid ("expected literal");
  }
  Result<ParsedOperand> operand (Token value) {
    if (value.tag == wire::VECTOR_LITERAL || value.tag <= wire::LD
        || (value.tag >= wire::STR1 && value.tag <= wire::STR4)) {
      auto l = literal (value);
      if (!(l).has_value ()) return l.error ();
      return ParsedOperand (std::move ((l).value ()));
    }
    if (value.tag >= wire::REG1 && value.tag <= wire::REG4) {
      auto n = terminated (value.bits);
      if (!(n).has_value ()) return n.error ();
      return ParsedOperand (RegisterName{(n).value ()});
    }
    if (value.tag >= wire::NAME1 && value.tag <= wire::NAME4) {
      auto n = terminated (value.bits);
      if (!(n).has_value ()) return n.error ();
      return ParsedOperand (SymbolName{(n).value ()});
    }
    if (value.tag >= wire::LAB1 && value.tag <= wire::LAB4)
      return ParsedOperand (LabelNumber{static_cast<std::uint32_t> (value.bits)});
    bool aliased = value.tag >= wire::ALIAS_DISP && value.tag <= wire::ALIAS_LAST;
    if ((value.tag >= wire::MEM_DISP && value.tag <= wire::MEM_LAST) || aliased) {
      unsigned form = value.tag - (aliased ? wire::ALIAS_DISP : wire::MEM_DISP);
      auto t = type (true);
      if (!(t).has_value ()) return t.error ();
      ParsedMemory result{ir::detail::NodeAccess::make_Memory ((t).value ()), {}, {}};
      if (form == 0 || form == 3 || form == 4 || form == 6) {
        auto displacement = sint ();
        if (!(displacement).has_value ()) return displacement.error ();
        result.value.displacement = (displacement).value ();
      }
      auto reg = [&] () -> Result<std::string_view> {
        auto token = this->token ();
        if (!(token).has_value ()) return token.error ();
        if (token.value ().tag < wire::REG1 || token.value ().tag > wire::REG4)
          return invalid ("expected address register");
        return terminated (token.value ().bits);
      };
      if (form == 1 || form == 3 || form == 5 || form == 6) {
        auto n = reg ();
        if (!(n).has_value ()) return n.error ();
        result.base = (n).value ();
      }
      if (form == 2 || form == 4 || form == 5 || form == 6) {
        auto n = reg ();
        if (!(n).has_value ()) return n.error ();
        result.index = (n).value ();
        auto scale = uint ();
        if (!(scale).has_value ()) return scale.error ();
        if ((scale).value () > 255) return invalid ("invalid address scale");
        result.value.scale = (scale).value ();
      }
      if (aliased) {
        auto a = name ();
        if (!(a).has_value ()) return a.error ();
        auto b = name ();
        if (!(b).has_value ()) return b.error ();
        result.value.alias = (a).value ();
        result.value.nonalias = (b).value ();
      }
      if (t.value ().is_block ()) {
        if (result.value.displacement <= 0) return invalid ("invalid block memory size");
        result.value.type = t.value ().kind () == Type::Kind::ReturnBlock
                              ? Type::return_block (result.value.displacement)
                              : Type::block (result.value.displacement, t.value ().block_class ());
        result.value.displacement = 0;
      }
      return ParsedOperand (std::move (result));
    }
    return invalid ("invalid operand tag");
  }
  Result<ParsedInitializer> data (std::string_view kind) {
    if (kind == "bss") {
      auto size = uint ();
      if (!(size).has_value ()) return size.error ();
      if ((size).value () > std::numeric_limits<std::size_t>::max ())
        return invalid ("BSS exceeds addressable storage");
      return ParsedInitializer (ZeroData{static_cast<std::size_t> ((size).value ())});
    }
    if (kind == "ref" || kind == "expr") {
      auto target = name ();
      if (!(target).has_value ()) return target.error ();
      std::int64_t displacement = 0;
      if (kind == "ref") {
        auto value = sint ();
        if (!(value).has_value ()) return value.error ();
        displacement = (value).value ();
      }
      return ParsedInitializer (ReferenceData{(target).value (), displacement, kind == "expr"});
    }
    if (kind == "lref") {
      auto label = sint ();
      if (!(label).has_value ()) return label.error ();
      auto base = sint ();
      if (!(base).has_value ()) return base.error ();
      auto displacement = sint ();
      if (!(displacement).has_value ()) return displacement.error ();
      if ((label).value () < 0 || (label).value () > UINT32_MAX || (base).value () < -1
          || (base).value () > UINT32_MAX)
        return invalid ("invalid label data identity");
      return ParsedInitializer (
        LabelData{static_cast<std::uint32_t> ((label).value ()),
                  (base).value () == -1
                    ? std::nullopt
                    : std::optional (static_cast<std::uint32_t> ((base).value ())),
                  (displacement).value ()});
    }
    auto element_type = type ();
    if (!(element_type).has_value ()) return element_type.error ();
    if (!element_type.value ().is_scalar () && !element_type.value ().is_vector ())
      return invalid ("data type must be scalar or vector");
    InitializedData result{(element_type).value (), {}};
    for (;;) {
      auto value = token ();
      if (!(value).has_value ()) return value.error ();
      if (value.value ().tag == wire::EOI) break;
      unsigned number = element_type.value ().is_vector () ? std::numeric_limits<unsigned>::max ()
                                                           : type_code ((element_type).value ());
      bool valid
        = element_type.value ().is_vector () ? value.value ().tag == wire::VECTOR_LITERAL
          : number <= unsigned (wire::Type::U64)
            ? ((number & 1) ? value.value ().tag <= wire::U8
                            : value.value ().tag >= wire::I1 && value.value ().tag <= wire::I8)
          : number == unsigned (wire::Type::F) ? value.value ().tag == wire::F
          : number == unsigned (wire::Type::D)
            ? value.value ().tag == wire::D
            : number == unsigned (wire::Type::LD) && value.value ().tag == wire::LD;
      if (!valid) return invalid ("data scalar and literal types differ");
      auto element = literal ((value).value ());
      if (!(element).has_value ()) return element.error ();
      if (element_type.value ().is_vector ()) {
        auto constant = ir::detail::operand_access (element.value ()).get<VectorConstant> ();
        if (!constant || constant->type != element_type.value ())
          return invalid ("data vector and literal types differ");
      }
      result.values.push_back (std::move ((element).value ()));
    }
    return ParsedInitializer (std::move (result));
  }
  Result<void> parse_module () {
    records_.clear ();
    labels_.clear ();
    ParsedFunction *function = nullptr;
    for (;;) {
      std::vector<std::uint32_t> labels;
      auto value = token ();
      if (!(value).has_value ()) return value.error ();
      while (value.value ().tag >= wire::LAB1 && value.value ().tag <= wire::LAB4) {
        labels.push_back (value.value ().bits);
        value = token ();
        if (!(value).has_value ()) return value.error ();
      }
      if (value.value ().tag <= wire::U8) {
        if (!function) return invalid ("instruction outside function");
        auto code = opcode (value.value ().bits);
        if (!code || *code == Opcode::Label) return invalid ("private or invalid opcode");
        if (instruction_count_++ >= options_.max_instructions)
          return invalid ("instruction limit exceeded");
        auto description = instruction_descriptor (*code).get ();
        ParsedInstruction instruction{*code, std::move (labels), {}};
        for (std::size_t n = 0;
             description->count != OperandCount::Fixed || n < description->roles.size (); ++n) {
          auto argument = token ();
          if (!(argument).has_value ()) return argument.error ();
          if (argument.value ().tag == wire::EOI) {
            if (description->count == OperandCount::Fixed)
              return invalid ("too few instruction operands");
            break;
          }
          auto parsed = operand ((argument).value ());
          if (!(parsed).has_value ()) return parsed.error ();
          instruction.operands.push_back (std::move ((parsed).value ()));
        }
        function->instructions.push_back (std::move (instruction));
        continue;
      }
      if (!labels.empty ()) return invalid ("labels before a module record");
      auto kind = name ((value).value ());
      if (!(kind).has_value ()) return kind.error ();
      if ((kind).value () == "endfunc") {
        if (!function) return invalid ("endfunc outside function");
        function = nullptr;
        function_name_ = {};
        continue;
      }
      if ((kind).value () == "local" || (kind).value () == "global") {
        if (!function) return invalid ("register declaration outside function");
        for (;;) {
          auto value = token ();
          if (!(value).has_value ()) return value.error ();
          if (value.value ().tag == wire::EOI) break;
          auto t = type ((value).value ());
          if (!(t).has_value ()) return t.error ();
          if (!t.value ().is_scalar () && !t.value ().is_vector ())
            return invalid ("local register must be scalar or vector");
          auto n = name ();
          if (!(n).has_value ()) return n.error ();
          std::string_view hardware;
          if ((kind).value () == "global") {
            auto h = name ();
            if (!(h).has_value ()) return h.error ();
            if (h.value ().empty ()) return invalid ("empty hardware binding");
            hardware = (h).value ();
          }
          function->locals.push_back ({(t).value (), (n).value (), hardware});
        }
        continue;
      }
      if (function) return invalid ("module record inside function");
      if ((kind).value () == "endmodule") return {};
      if ((kind).value () == "func" || (kind).value () == "proto") {
        auto n = name ();
        if (!(n).has_value ()) return n.error ();
        function_name_ = (kind).value () == "func" ? (n).value () : std::string_view{};
        auto body = signature ((kind).value () == "func");
        if (!(body).has_value ()) return body.error ();
        records_.push_back (
          {(kind).value () == "func" ? Record::Kind::Function : Record::Kind::Prototype,
           (n).value (), std::move ((body).value ())});
        if ((kind).value () == "func") function = &std::get<ParsedFunction> (records_.back ().body);
        continue;
      }
      if ((kind).value () == "import" || (kind).value () == "forward"
          || (kind).value () == "export") {
        auto n = name ();
        if (!(n).has_value ()) return n.error ();
        records_.push_back ({(kind).value () == "import"    ? Record::Kind::Import
                             : (kind).value () == "forward" ? Record::Kind::Forward
                                                            : Record::Kind::Export,
                             (n).value (),
                             {}});
        continue;
      }
      auto spelling = (kind).value ();
      bool named = spelling.starts_with ('n');
      if (named) spelling.remove_prefix (1);
      if (spelling != "data" && spelling != "bss" && spelling != "ref" && spelling != "lref"
          && spelling != "expr")
        return invalid ("unknown module record");
      std::string_view n;
      if (named) {
        auto name = this->name ();
        if (!(name).has_value ()) return name.error ();
        n = (name).value ();
        if (n.empty ()) return invalid ("empty named data symbol");
      }
      auto initializer = data (spelling);
      if (!(initializer).has_value ()) return initializer.error ();
      records_.push_back ({Record::Kind::Data, n, std::move ((initializer).value ())});
    }
  }
  Result<void> build () {
    for (auto &record : records_)
      if (!record.name.empty ()) {
        auto reserved = ir::detail::access (*(module_)).reserve_symbol (record.name);
        if (!(reserved).has_value ()) return diagnose (reserved.error ());
      }
    for (auto &record : records_) {
      switch (record.kind) {
      case Record::Kind::Function: {
        auto &body = std::get<ParsedFunction> (record.body);
        function_name_ = record.name;
        auto created = ir::detail::access (*(module_))
                         .create_function (record.name, body.signature, body.arguments);
        if (!(created).has_value ()) return diagnose (created.error ());
        body.function = &(created).value ();
        for (auto &reg : body.locals) {
          auto added = ir::detail::access (*(body.function))
                         .create_register (reg.type, reg.name, reg.hardware);
          if (!(added).has_value ()) return diagnose (added.error ());
        }
        auto created_block = ir::detail::access (*(body.function)).create_block ();
        if (!(created_block).has_value ()) return diagnose (created_block.error ());
        auto block = &(created_block).value ();
        bool occupied = false, terminated = false;
        for (auto &instruction : body.instructions) {
          if (terminated || (occupied && !instruction.labels.empty ())) {
            auto next = ir::detail::access (*(body.function)).create_block ();
            if (!(next).has_value ()) return diagnose (next.error ());
            block = &(next).value ();
          }
          instruction.block = block;
          for (auto number : instruction.labels) {
            if (labels_.contains (number)) return invalid ("duplicate module label identity");
            auto label = ir::detail::access (*(body.function)).create_label (*block);
            if (!(label).has_value ()) return diagnose (label.error ());
            labels_.emplace (number, &(label).value ());
          }
          occupied = true;
          terminated
            = instruction_descriptor (instruction.opcode).get ()->flow != ControlFlow::Next;
        }
        function_name_ = {};
        break;
      }
      case Record::Kind::Prototype: {
        auto p = ir::detail::access (*(module_))
                   .prototype (record.name, std::get<ParsedFunction> (record.body).signature);
        if (!(p).has_value ()) return diagnose (p.error ());
        break;
      }
      case Record::Kind::Import: {
        auto imported = ir::detail::access (*(module_)).import (record.name);
        if (!(imported).has_value ()) return diagnose (imported.error ());
        break;
      }
      case Record::Kind::Forward: {
        auto forward = ir::detail::access (*(module_)).forward (record.name);
        if (!(forward).has_value ()) return diagnose (forward.error ());
        break;
      }
      case Record::Kind::Export: {
        auto exported = ir::detail::access (*(module_))
                          .export_symbol (*module_->find_symbol (record.name).get ());
        if (!(exported).has_value ()) return diagnose (exported.error ());
        break;
      }
      case Record::Kind::Data: {
        auto declared = ir::detail::access (*(module_)).declare_data (record.name);
        if (!(declared).has_value ()) return diagnose (declared.error ());
        record.symbol = &(declared).value ();
        break;
      }
      }
    }
    for (auto &record : records_)
      if (record.kind == Record::Kind::Function) {
        auto &body = std::get<ParsedFunction> (record.body);
        function_name_ = record.name;
        auto reg = [&] (std::string_view name) -> Result<const Register &> {
          auto result = body.function->find_register (name).get ();
          if (!result) return invalid ("undeclared register: " + std::string (name));
          return *(result);
        };
        for (auto &instruction : body.instructions) {
          std::vector<Operand> operands;
          operands.reserve (instruction.operands.size ());
          for (auto &argument : instruction.operands) {
            auto value = std::visit (
              [&] (const auto &value) -> Result<Operand> {
                using T = std::decay_t<decltype (value)>;
                if constexpr (std::is_same_v<T, Operand>)
                  return value;
                else if constexpr (std::is_same_v<T, RegisterName>) {
                  auto r = reg (value.name);
                  if (!r.has_value ()) return r.error ();
                  return Operand (r.value ());
                } else if constexpr (std::is_same_v<T, SymbolName>) {
                  auto symbol = module_->find_symbol (value.name).get ();
                  if (!symbol) return invalid ("undeclared symbol");
                  return Operand (*symbol);
                } else if constexpr (std::is_same_v<T, LabelNumber>) {
                  auto label = labels_.find (value.number);
                  if (label == labels_.end ()) return invalid ("undefined label");
                  return Operand (*label->second);
                } else {
                  auto m = value.value;
                  if (value.base) {
                    auto r = reg (*value.base);
                    if (!r.has_value ()) return r.error ();
                    ir::detail::NodeAccess::base (m) = &r.value ();
                  }
                  if (value.index) {
                    auto r = reg (*value.index);
                    if (!r.has_value ()) return r.error ();
                    ir::detail::NodeAccess::index (m) = &r.value ();
                  }
                  return Operand (std::move (m));
                }
              },
              argument);
            if (!(value).has_value ()) return value.error ();
            operands.push_back (std::move ((value).value ()));
          }
          auto emitted
            = InstructionBuilder (*instruction.block).emit (instruction.opcode, operands);
          if (!(emitted).has_value ()) return diagnose (emitted.error ());
        }
        function_name_ = {};
      }
    for (auto &record : records_)
      if (record.kind == Record::Kind::Data) {
        auto initializer = std::visit (
          [&] (const auto &value) -> Result<DataInitializer> {
            using T = std::decay_t<decltype (value)>;
            if constexpr (std::is_same_v<T, InitializedData> || std::is_same_v<T, ZeroData>)
              return DataInitializer (value);
            else if constexpr (std::is_same_v<T, ReferenceData>) {
              auto symbol = module_->find_symbol (value.name).get ();
              if (!symbol) return invalid ("undefined data symbol");
              if (value.expression) {
                if (!symbol->function ().has_value ())
                  return invalid ("expression requires a function");
                return DataInitializer (
                  ir::detail::NodeAccess::make_ExpressionData (symbol->function ().get ()));
              }
              return DataInitializer (
                ir::detail::NodeAccess::make_SymbolReference (symbol, value.displacement));
            } else {
              auto first = labels_.find (value.label);
              if (first == labels_.end ()) return invalid ("undefined data label");
              const Label *base = nullptr;
              if (value.base) {
                auto second = labels_.find (*value.base);
                if (second == labels_.end ()) return invalid ("undefined base label");
                base = second->second;
              }
              return DataInitializer (
                ir::detail::NodeAccess::make_LabelReference (first->second, base,
                                                             value.displacement));
            }
          },
          std::get<ParsedInitializer> (record.body));
        if (!(initializer).has_value ()) return initializer.error ();
        auto defined = ir::detail::access (*(module_))
                         .define_data (*record.symbol, std::move ((initializer).value ()));
        if (!(defined).has_value ()) return diagnose (defined.error ());
      }
    return {};
  }
};
}  // namespace
Result<std::vector<std::byte>> write_binary (const ir::Program &context, BinaryEncoding encoding) {
  for (auto &module : context.modules ())
    for (auto &function : module.functions ()) {
      auto phase = function.require_register_ir ();
      if (!(phase).has_value ()) return phase.error ();
    }
  if (encoding != BinaryEncoding::Compressed && encoding != BinaryEncoding::Raw)
    return Error{ErrorCode::InvalidArgument, "invalid binary encoding"};
  if (ir::needs_materialization (context)) {
    auto lowered = ir::materialize (context);
    if (!(lowered).has_value ()) return lowered.error ();
    return write_binary ((lowered).value (), encoding);
  }
  auto raw = Writer ().run (context);
  if (!(raw).has_value () || encoding == BinaryEncoding::Raw) return raw;
  return compress ((raw).value ());
}
Result<ir::Program> read_binary (std::span<const std::byte> bytes, BinaryOptions options) {
  if (options.encoding != BinaryEncoding::Compressed && options.encoding != BinaryEncoding::Raw)
    return Error{ErrorCode::InvalidArgument, "invalid binary encoding"};
  if (options.encoding == BinaryEncoding::Raw) return Reader (bytes, options).run ();
  auto raw = decompress (bytes, options.max_bytes);
  if (!(raw).has_value ())
    return Error{raw.error ().code,
                 std::string (options.source_name) + ": " + raw.error ().message};
  return Reader ((raw).value (), options).run ();
}
}  // namespace mirnext::serialization
