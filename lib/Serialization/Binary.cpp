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
struct Code { Opcode opcode; unsigned number; };
constexpr Code codes[]{
#define MIRNEXT_OPCODE(name, number, spelling, roles, count, flow) {Opcode::name, number},
#include "mirnext/IR/Opcodes.def"
#undef MIRNEXT_OPCODE
};
std::optional<Opcode> opcode(std::uint64_t number) {
  auto found = std::lower_bound(std::begin(codes), std::end(codes), number, [](Code a, auto b) { return a.number < b; });
  return found != std::end(codes) && found->number == number ? std::optional(found->opcode) : std::nullopt;
}
unsigned type_code(Type type) {
  if (type.kind() == Type::Kind::Bool) return unsigned(wire::Type::I64);
  if (type.kind() == Type::Kind::Block) return unsigned(wire::Type::Block) + unsigned(type.block_class());
  if (type.kind() == Type::Kind::ReturnBlock) return unsigned(wire::Type::ReturnBlock);
  if (type.kind() == Type::Kind::Undef) return unsigned(wire::Type::Undef);
  return unsigned(type.kind()) - 1;
}
Type scalar_type(unsigned number) {
  if (number >= unsigned(wire::Type::Block) && number <= unsigned(wire::Type::Block4))
    return Type::block(0, static_cast<Type::BlockClass>(number - unsigned(wire::Type::Block)));
  if (number == unsigned(wire::Type::ReturnBlock)) return Type::return_block(0);
  if (number == unsigned(wire::Type::Undef)) return Type(Type::Kind::Undef);
  return Type(static_cast<Type::Kind>(number + 1));
}
// Names share wire string identities with literals containing the same trailing
// NUL. Look up that byte sequence without allocating a terminated temporary.
struct TerminatedName { std::string_view text; };
struct WireStringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view bytes) const noexcept {
    if (!bytes.empty() && bytes.back() == '\0') bytes.remove_suffix(1);
    return std::hash<std::string_view>{}(bytes);
  }
  std::size_t operator()(TerminatedName name) const noexcept {
    return std::hash<std::string_view>{}(name.text);
  }
};
struct WireStringEqual {
  using is_transparent = void;
  bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
  bool operator()(std::string_view bytes, TerminatedName name) const noexcept {
    return bytes.size() == name.text.size() + 1 && bytes.back() == '\0'
      && bytes.substr(0,bytes.size()-1) == name.text;
  }
  bool operator()(TerminatedName name, std::string_view bytes) const noexcept { return (*this)(bytes,name); }
};
class Writer {
public:
  Result<std::vector<std::byte>> run(const Context &context) {
    auto valid = context.verify(); if (!valid) return valid.error();
    for (auto &module : context.modules()) { auto result = write_module(module); if (!result) return result.error(); }
    byte(wire::EOFILE); std::vector<std::byte> body = std::move(bytes_); bytes_.clear();
    uint(wire::version); uint(strings_.size());
    for (auto string : strings_) { uint(string->size()); bytes_.insert(bytes_.end(), reinterpret_cast<const std::byte *>(string->data()), reinterpret_cast<const std::byte *>(string->data() + string->size())); }
    bytes_.insert(bytes_.end(), body.begin(), body.end()); return std::move(bytes_);
  }
private:
  const Module *module_ = nullptr; const Function *function_ = nullptr;
  std::vector<std::byte> bytes_;
  // Unordered-map keys have stable addresses across rehash; the ordered table borrows them.
  std::vector<const std::string *> strings_;
  std::unordered_map<std::string, std::uint32_t, WireStringHash, WireStringEqual> string_ids_;
  std::unordered_set<std::string> used_names_; std::size_t next_name_ = 0;
  std::unordered_map<const Register *, std::string> registers_;
  std::unordered_map<const Label *, std::uint32_t> labels_;
  std::unordered_map<const Instruction *, std::string> call_signatures_;
  struct Expansion { std::size_t index; std::string name; };
  std::unordered_map<const Instruction *, std::vector<Expansion>> expansions_;
  Error invalid(std::string message) const {
    return {ErrorCode::InvalidOperand, std::string(module_ ? module_->name() : "binary")
      + (function_ ? "/" + std::string(function_->symbol().name()) : "") + ": binary-writer: " + std::move(message)};
  }
  std::string fresh(std::string_view prefix) {
    for (;;) { auto name = std::string(prefix) + std::to_string(next_name_++); if (used_names_.insert(name).second) return name; }
  }
  void byte(unsigned value) { bytes_.push_back(static_cast<std::byte>(value)); }
  void little(std::uint64_t value, unsigned count) { for (unsigned n = 0; n < count; ++n) { byte(value); value >>= 8; } }
  static unsigned width(std::uint64_t value) { unsigned count = 1; while (value >>= 8) ++count; return count; }
  void uint(std::uint64_t value) { if (value <= 127) byte(128 | unsigned(value)); else { auto count = width(value); byte(wire::U1 + count - 1); little(value, count); } }
  void sint(std::int64_t value) { auto bits = std::bit_cast<std::uint64_t>(value); auto count = width(bits); byte(wire::I1 + count - 1); little(bits, count); }
  Result<void> string(std::string_view value, unsigned first, bool terminate = false) {
    auto found = terminate ? string_ids_.find(TerminatedName{value}) : string_ids_.find(value);
    if (found == string_ids_.end()) {
      if (strings_.size() >= UINT32_MAX) return invalid("string table exceeds binary width");
      auto number = static_cast<std::uint32_t>(strings_.size());
      std::string owned(value); if (terminate) owned.push_back('\0');
      found = string_ids_.emplace(std::move(owned), number).first; strings_.push_back(&found->first);
    }
    auto count = width(found->second); byte(first + count - 1); little(found->second, count); return {};
  }
  Result<void> name(std::string_view value, unsigned first = wire::NAME1) { return string(value, first, true); }
  void type(Type value) { byte(wire::TYPE + type_code(value)); }
  Result<void> reg(const Register *value) { return name(registers_.at(value), wire::REG1); }
  Result<void> label(const Label *value, bool signed_number = false) {
    auto found = labels_.find(value); if (found == labels_.end()) return invalid("undefined label");
    if (signed_number) sint(found->second); else { auto count = width(found->second); byte(wire::LAB1 + count - 1); little(found->second, count); }
    return {};
  }
  Result<void> memory(const Memory &value, std::string_view override_base = {}) {
    bool override = !override_base.empty(); bool base = override || value.base, index = !override && value.index;
    auto displacement = override ? std::int64_t{0} : value.displacement;
    if (value.type.is_block()) displacement = static_cast<std::int64_t>(value.type.block_size());
    unsigned form = displacement ? (base ? (index ? 6 : 3) : (index ? 4 : 0)) : (base ? (index ? 5 : 1) : (index ? 2 : 0));
    bool aliased = !value.alias.empty() || !value.nonalias.empty(); byte((aliased ? wire::ALIAS_DISP : wire::MEM_DISP) + form); type(value.type);
    if (displacement || (!base && !index)) sint(displacement);
    if (base) { auto result = override ? name(override_base, wire::REG1) : reg(value.base); if (!result) return result; }
    if (index) { auto result = reg(value.index); if (!result) return result; uint(value.scale); }
    if (aliased) { auto a = name(value.alias); if (!a) return a; return name(value.nonalias); } return {};
  }
  Result<void> operand(const Operand &operand) {
    return std::visit([&](const auto &value) -> Result<void> {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::int64_t>) sint(value);
      else if constexpr (std::is_same_v<T, std::uint64_t>) uint(value);
      else if constexpr (std::is_same_v<T, float>) { byte(wire::F); little(std::bit_cast<std::uint32_t>(value), 4); }
      else if constexpr (std::is_same_v<T, double>) { byte(wire::D); little(std::bit_cast<std::uint64_t>(value), 8); }
      else if constexpr (std::is_same_v<T, long double>) { byte(wire::LD); auto bytes = wire::floating_bytes(value); bytes_.insert(bytes_.end(), bytes.begin(), bytes.end()); }
      else if constexpr (std::is_same_v<T, std::string>) return string(value, wire::STR1);
      else if constexpr (std::is_same_v<T, const Register *>) return reg(value);
      else if constexpr (std::is_same_v<T, const Label *>) return label(value);
      else if constexpr (std::is_same_v<T, Memory>) return memory(value);
      else { if (value.displacement) return invalid("unmaterialized symbol displacement"); return name(value.symbol->name()); }
      return {};
    }, operand.storage());
  }
  Result<void> signature(const Signature &value, const Function *function = nullptr) {
    uint(value.variadic); uint(value.results.size()); for (auto result : value.results) type(result);
    for (std::size_t n = 0; n < value.parameters.size(); ++n) {
      auto parameter = value.parameters[n]; type(parameter);
      auto named = name(function ? registers_.at(function->arguments()[n]) : ".arg" + std::to_string(n)); if (!named) return named;
      if (parameter.is_block()) uint(parameter.block_size());
    }
    byte(wire::EOI); return {};
  }
  Result<void> instruction(const Instruction &instruction) {
    auto found = expansions_.find(&instruction);
    auto expansions = found == expansions_.end() ? std::span<const Expansion>{} : std::span<const Expansion>(found->second);
    for (auto &expansion : expansions) {
      auto &value = instruction.operands()[expansion.index];
      auto destination = [&]() { return name(expansion.name, wire::REG1); };
      if (auto symbol = value.get<SymbolReference>()) {
        uint(codes[std::size_t(Opcode::Mov)].number); auto d = destination(); if (!d) return d; auto s = name(symbol->symbol->name()); if (!s) return s;
        uint(codes[std::size_t(Opcode::Add)].number); d = destination(); if (!d) return d; d = destination(); if (!d) return d; sint(symbol->displacement);
      } else {
        auto &memory = *value.get<Memory>();
        uint(codes[std::size_t(Opcode::Mov)].number); auto d = destination(); if (!d) return d;
        if (memory.index) { auto source = reg(memory.index); if (!source) return source; }
        else sint(0);
        if (memory.index && memory.scale != 1) {
          uint(codes[std::size_t(Opcode::Mul)].number); d = destination(); if (!d) return d; d = destination(); if (!d) return d; sint(memory.scale);
        }
        if (memory.base) { uint(codes[std::size_t(Opcode::Add)].number); d = destination(); if (!d) return d; d = destination(); if (!d) return d; auto source = reg(memory.base); if (!source) return source; }
        uint(codes[std::size_t(Opcode::Add)].number); d = destination(); if (!d) return d; d = destination(); if (!d) return d; sint(memory.displacement);
      }
    }
    uint(codes[std::size_t(instruction.opcode())].number);
    for (std::size_t n = 0; n < instruction.operands().size(); ++n) {
      Result<void> result;
      if (!expansions.empty() && expansions.front().index == n) {
        auto expansion = expansions.front(); expansions = expansions.subspan(1);
        if (auto m = instruction.operands()[n].get<Memory>()) result = memory(*m, expansion.name);
        else result = name(expansion.name, wire::REG1);
      } else if (!n && instruction_descriptor(instruction.opcode())->count == OperandCount::Call) result = name(call_signatures_.at(&instruction));
      else result = operand(instruction.operands()[n]);
      if (!result) return result;
    }
    if (instruction_descriptor(instruction.opcode())->count != OperandCount::Fixed) byte(wire::EOI); return {};
  }
  Result<void> function(const Function &value) {
    function_ = &value; auto kind = name("func"); if (!kind) return kind; auto named = name(value.symbol().name()); if (!named) return named;
    auto sig = signature(value.signature(), &value); if (!sig) return sig;
    for (bool hardware : {false, true}) {
      bool opened = false;
      for (auto &reg : value.registers()) if (!reg.is_argument() && (!reg.hardware_binding().empty()) == hardware) {
        if (!opened) { auto k = name(hardware ? "global" : "local"); if (!k) return k; opened = true; }
        type(reg.type().is_integer() ? Type::i64() : reg.type()); auto n = name(registers_.at(&reg)); if (!n) return n;
        if (hardware) { n = name(reg.hardware_binding()); if (!n) return n; }
      }
      if (!hardware) for (auto &block : value.blocks()) for (auto &instruction : block.instructions()) {
        auto found = expansions_.find(&instruction); if (found == expansions_.end()) continue;
        for (auto &expansion : found->second) {
          if (!opened) { auto n = name("local"); if (!n) return n; opened = true; }
          type(Type::i64()); auto n = name(expansion.name); if (!n) return n;
        }
      }
      if (opened) byte(wire::EOI);
    }
    for (auto &block : value.blocks()) {
      for (auto anchor : block.labels()) { auto written = label(anchor); if (!written) return written; }
      for (auto &i : block.instructions()) { auto written = instruction(i); if (!written) return written; }
    }
    function_ = nullptr; return name("endfunc");
  }
  Result<void> data(const GlobalData &value) {
    auto named = !value.symbol().name().empty();
    return std::visit([&](const auto &initializer) -> Result<void> {
      using T = std::decay_t<decltype(initializer)>;
      auto kind = std::is_same_v<T, InitializedData> ? "data" : std::is_same_v<T, ZeroData> ? "bss"
        : std::is_same_v<T, SymbolReference> ? "ref" : std::is_same_v<T, LabelReference> ? "lref" : "expr";
      auto start = name((named ? "n" : "") + std::string(kind)); if (!start) return start;
      if (named) { auto n = name(value.symbol().name()); if (!n) return n; }
      if constexpr (std::is_same_v<T, InitializedData>) {
        auto element_type = initializer.type.kind() == Type::Kind::Pointer ? Type(Type::Kind::U64) : initializer.type;
        type(element_type);
        for (auto &element : initializer.values) {
          if (element_type.is_integer()) {
            auto bits = element.template get<std::uint64_t>() ? *element.template get<std::uint64_t>() : std::bit_cast<std::uint64_t>(*element.template get<std::int64_t>());
            switch (element_type.kind()) {
            case Type::Kind::I8: sint(static_cast<std::int8_t>(bits)); break; case Type::Kind::U8: uint(static_cast<std::uint8_t>(bits)); break;
            case Type::Kind::I16: sint(static_cast<std::int16_t>(bits)); break; case Type::Kind::U16: uint(static_cast<std::uint16_t>(bits)); break;
            case Type::Kind::I32: sint(static_cast<std::int32_t>(bits)); break; case Type::Kind::U32: uint(static_cast<std::uint32_t>(bits)); break;
            case Type::Kind::U64: uint(bits); break; default: sint(std::bit_cast<std::int64_t>(bits)); break;
            }
          } else { auto written = operand(element); if (!written) return written; }
        }
        byte(wire::EOI);
      } else if constexpr (std::is_same_v<T, ZeroData>) uint(initializer.size);
      else if constexpr (std::is_same_v<T, SymbolReference>) { auto target = name(initializer.symbol->name()); if (!target) return target; sint(initializer.displacement); }
      else if constexpr (std::is_same_v<T, LabelReference>) {
        auto first = label(initializer.label, true); if (!first) return first;
        if (initializer.base) { auto second = label(initializer.base, true); if (!second) return second; } else sint(-1);
        sint(initializer.displacement);
      } else return name(initializer.function->symbol().name());
      return {};
    }, value.initializer());
  }
  Result<void> write_module(const Module &module) {
    module_ = &module; function_ = nullptr; next_name_ = 0; used_names_.clear(); registers_.clear(); labels_.clear(); call_signatures_.clear(); expansions_.clear();
    std::unordered_set<const Function *> expressions;
    for (auto &symbol : module.symbols()) used_names_.emplace(symbol.name());
    for (auto &data : module.data()) if (auto value = std::get_if<ExpressionData>(&data.initializer())) expressions.insert(value->function);
    std::vector<const Function *> functions;
    for (bool expression : {true, false}) for (auto &item : module.items())
      if (auto f = std::get_if<const Function *>(&item); f && expressions.contains(*f) == expression) functions.push_back(*f);
    std::uint64_t label_number = 0;
    for (auto f : functions) {
      std::unordered_set<std::string> register_names; std::size_t next_register = 0;
      for (auto &reg : f->registers()) {
        auto n = std::string(reg.name());
        if (n.empty() || register_names.contains(n)) do { n = ".r" + std::to_string(next_register++); } while (register_names.contains(n));
        register_names.insert(n); used_names_.insert(n); registers_.emplace(&reg, std::move(n));
      }
      for (auto &block : f->blocks()) for (auto label : block.labels()) {
        if (label_number > UINT32_MAX) return invalid("label count exceeds binary width"); labels_.emplace(label, label_number++);
      }
    }
    std::unordered_map<const Signature *, std::string> generated_signatures;
    std::vector<const Signature *> signature_order;
    std::unordered_set<const Symbol *> visible;
    for (auto &item : module.items()) {
      if (auto symbol = std::get_if<const Symbol *>(&item); symbol && (*symbol)->kind() == Symbol::Kind::Prototype) visible.insert(*symbol);
      auto f = std::get_if<const Function *>(&item); if (!f) continue;
      for (auto &block : (*f)->blocks()) for (auto &instruction : block.instructions()) {
      for (std::size_t n = 0; n < instruction.operands().size(); ++n) {
        auto &value = instruction.operands()[n];
        if ((value.get<SymbolReference>() && value.get<SymbolReference>()->displacement)
            || (value.get<Memory>() && value.get<Memory>()->type.is_block() && value.get<Memory>()->displacement))
          expansions_[&instruction].push_back({n, fresh(".addr")});
      }
      if (instruction_descriptor(instruction.opcode())->count != OperandCount::Call) continue;
      auto symbol = instruction.operands()[0].get<SymbolReference>()->symbol;
      if (visible.contains(symbol)) { call_signatures_.emplace(&instruction, std::string(symbol->name())); continue; }
      auto signature = symbol->signature();
      auto found = generated_signatures.find(signature);
      if (found == generated_signatures.end()) { found = generated_signatures.emplace(signature, fresh(".sig")).first; signature_order.push_back(signature); }
      call_signatures_.emplace(&instruction, found->second);
      }
    }
    auto start = name("module"); if (!start) return start; auto n = name(module.name()); if (!n) return n;
    for (auto &symbol : module.symbols()) {
      if (symbol.kind() == Symbol::Kind::Import) { auto k = name("import"); if (!k) return k; n = name(symbol.name()); if (!n) return n; }
      else if (symbol.kind() == Symbol::Kind::Function || (symbol.kind() == Symbol::Kind::Data && !symbol.name().empty())) {
        auto k = name("forward"); if (!k) return k; n = name(symbol.name()); if (!n) return n;
      }
    }
    for (auto value : signature_order) { auto k = name("proto"); if (!k) return k; n = name(generated_signatures.at(value)); if (!n) return n; auto s = signature(*value); if (!s) return s; }
    for (auto f : functions) if (expressions.contains(f)) { auto written = function(*f); if (!written) return written; }
    bool last_data = false, skipped = false;
    for (auto &item : module.items()) {
      if (std::holds_alternative<ForwardDeclaration>(item)
          || (std::holds_alternative<const Symbol *>(item) && std::get<const Symbol *>(item)->kind() == Symbol::Kind::Import)
          || (std::holds_alternative<const Function *>(item) && expressions.contains(std::get<const Function *>(item)))) { skipped = true; continue; }
      auto datum = std::get_if<const GlobalData *>(&item);
      if (datum && (*datum)->symbol().name().empty() && last_data && skipped) {
        auto k = name("proto"); if (!k) return k; n = name(fresh(".boundary")); if (!n) return n; auto s = signature({}); if (!s) return s;
      }
      last_data = datum != nullptr; skipped = false;
      auto result = std::visit([&](auto value) -> Result<void> {
        using T = decltype(value);
        if constexpr (std::is_same_v<T, const Function *>) return function(*value);
        else if constexpr (std::is_same_v<T, const GlobalData *>) return data(*value);
        else if constexpr (std::is_same_v<T, const Symbol *>) {
          auto k = name("proto"); if (!k) return k; auto n = name(value->name()); if (!n) return n; return signature(*value->signature());
        } else if constexpr (std::is_same_v<T, ExportDeclaration>) { auto k = name("export"); if (!k) return k; return name(value.symbol->name()); }
        else return invalid("unmaterialized data declaration");
      }, item);
      if (!result) return result;
    }
    return name("endmodule");
  }
};
struct RegisterName { std::string_view name; };
struct SymbolName { std::string_view name; };
struct LabelNumber { std::uint32_t number; };
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
  struct Local { Type type; std::string_view name, hardware; };
  std::vector<Local> locals;
  std::vector<ParsedInstruction> instructions;
  Function *function = nullptr;
};
struct ReferenceData { std::string_view name; std::int64_t displacement; bool expression; };
struct LabelData { std::uint32_t label; std::optional<std::uint32_t> base; std::int64_t displacement; };
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
  Reader(std::span<const std::byte> bytes, BinaryOptions options) : bytes_(bytes), options_(options), context_(std::make_unique<Context>()) {}
  Result<std::unique_ptr<Context>> run() {
    if (bytes_.size() > options_.max_bytes) return invalid("input byte limit exceeded");
    auto version = uint(); if (!version) return version.error(); if (*version > wire::version) return invalid("unsupported binary version");
    auto count = uint(); if (!count) return count.error();
    if (*count > options_.max_strings || *count > UINT32_MAX || *count > bytes_.size() - position_) return invalid("string table limit or truncated input");
    strings_.reserve(*count);
    for (std::uint64_t n = 0; n < *count; ++n) {
      auto size = uint(); if (!size) return size.error(); if (*size > bytes_.size() - position_) return invalid("truncated string table");
      strings_.emplace_back(reinterpret_cast<const char *>(bytes_.data() + position_), static_cast<std::size_t>(*size)); position_ += *size;
    }
    for (;;) {
      auto first = token(); if (!first) return first.error();
      if (first->tag == wire::EOFILE) {
        if (position_ != bytes_.size()) return invalid("trailing bytes");
        auto valid = context_->verify(); if (!valid) return diagnose(valid.error()); return std::move(context_);
      }
      auto kind = name(*first); if (!kind) return kind.error(); if (*kind != "module") return invalid("expected module record");
      auto module_name = name(); if (!module_name) return module_name.error();
      auto created = context_->create_module(*module_name); if (!created) return diagnose(created.error()); module_ = *created;
      auto parsed = parse_module(); if (!parsed) return parsed.error(); auto built = build(); if (!built) return built.error(); module_ = nullptr;
    }
  }
private:
  struct Token { unsigned tag; std::uint64_t bits = 0; std::array<std::byte, 16> extended{}; };
  std::span<const std::byte> bytes_; BinaryOptions options_; std::size_t position_ = 0, instruction_count_ = 0;
  std::unique_ptr<Context> context_; Module *module_ = nullptr; std::string_view function_name_;
  std::vector<std::string_view> strings_; std::vector<Record> records_;
  std::unordered_map<std::uint32_t, const Label *> labels_;
  Error diagnose(Error error) const {
    error.message = std::string(options_.source_name) + ":" + std::to_string(position_) + ": "
      + (module_ ? std::string(module_->name()) + "/" : "") + std::string(function_name_) + ": binary-reader: " + error.message;
    return error;
  }
  Error invalid(std::string message) const { return diagnose({ErrorCode::InvalidOperand, std::move(message)}); }
  Result<unsigned> byte() {
    if (position_ == bytes_.size()) return invalid("truncated input"); return std::to_integer<unsigned>(bytes_[position_++]);
  }
  Result<std::uint64_t> little(unsigned count) {
    if (count > bytes_.size() - position_) return invalid("truncated value");
    std::uint64_t value = 0; for (unsigned n = 0; n < count; ++n) value |= std::uint64_t(std::to_integer<unsigned>(bytes_[position_++])) << (n * 8); return value;
  }
  Result<Token> token() {
    auto tag = byte(); if (!tag) return tag.error(); Token result{*tag};
    if (*tag & 128) { result.tag = wire::U0; result.bits = *tag & 127; return result; }
    unsigned count = 0;
    if (*tag >= wire::U1 && *tag <= wire::U8) count = *tag;
    else if (*tag >= wire::I1 && *tag <= wire::I8) count = *tag - wire::I1 + 1;
    else if (*tag >= wire::REG1 && *tag <= wire::LAB4) count = (*tag - wire::REG1) % 4 + 1;
    else if (*tag == wire::F) count = 4;
    else if (*tag == wire::D) count = 8;
    else if (*tag == wire::LD) {
      if (bytes_.size() - position_ < result.extended.size()) return invalid("truncated long double");
      std::memcpy(result.extended.data(), bytes_.data() + position_, result.extended.size()); position_ += result.extended.size(); return result;
    } else if ((*tag >= wire::MEM_DISP && *tag <= wire::EOFILE) || (*tag >= wire::ALIAS_DISP && *tag <= wire::ALIAS_LAST)) return result;
    else return invalid("invalid tag");
    auto value = little(count); if (!value) return value.error(); result.bits = *value; return result;
  }
  Result<std::uint64_t> uint() {
    auto value = token(); if (!value) return value.error(); if (value->tag > wire::U8) return invalid("expected unsigned integer"); return value->bits;
  }
  Result<std::int64_t> sint() {
    auto value = token(); if (!value) return value.error(); if (value->tag < wire::I1 || value->tag > wire::I8) return invalid("expected signed integer"); return std::bit_cast<std::int64_t>(value->bits);
  }
  Result<std::string_view> string(std::uint64_t id) {
    if (id >= strings_.size()) return invalid("invalid string identity"); return strings_[id];
  }
  Result<std::string_view> terminated(std::uint64_t id) {
    auto value = string(id); if (!value) return value.error();
    if (value->empty() || value->back() != '\0' || value->find('\0') != value->size() - 1) return invalid("name is not a terminated string");
    return value->substr(0, value->size() - 1);
  }
  Result<std::string_view> name(Token token) {
    if (token.tag < wire::NAME1 || token.tag > wire::NAME4) return invalid("expected name"); return terminated(token.bits);
  }
  Result<std::string_view> name() { auto value = token(); if (!value) return value.error(); return name(*value); }
  Result<Type> type(Token token, bool undef = false) {
    auto last = unsigned(undef ? wire::Type::Undef : wire::Type::ReturnBlock);
    if (token.tag < wire::TYPE || token.tag > wire::TYPE + last) return invalid("invalid type tag"); return scalar_type(token.tag - wire::TYPE);
  }
  Result<Type> type(bool undef = false) { auto value = token(); if (!value) return value.error(); return type(*value, undef); }
  Result<ParsedFunction> signature(bool body) {
    ParsedFunction result; auto variadic = uint(); if (!variadic) return variadic.error(); if (*variadic > 1) return invalid("invalid variadic flag"); result.signature.variadic = *variadic;
    auto count = uint(); if (!count) return count.error(); if (*count > bytes_.size() - position_) return invalid("invalid result count");
    for (std::uint64_t n = 0; n < *count; ++n) {
      auto result_type = type(); if (!result_type) return result_type.error(); if (!result_type->is_scalar()) return invalid("function result must be scalar"); result.signature.results.push_back(*result_type);
    }
    for (;;) {
      auto value = token(); if (!value) return value.error(); if (value->tag == wire::EOI) break;
      auto parameter = type(*value); if (!parameter) return parameter.error(); auto argument = name(); if (!argument) return argument.error();
      if (parameter->is_block()) {
        auto size = uint(); if (!size) return size.error(); if (*size > INT64_MAX || !*size) return invalid("invalid block argument size");
        *parameter = parameter->kind() == Type::Kind::ReturnBlock ? Type::return_block(*size) : Type::block(*size, parameter->block_class());
      }
      if (body && argument->empty()) return invalid("empty function argument name");
      result.signature.parameters.push_back(*parameter); result.arguments.push_back(*argument);
    }
    auto valid = result.signature.verify(); if (!valid) return diagnose(valid.error()); return result;
  }
  Result<Operand> literal(Token token) {
    if (token.tag <= wire::U8) return Operand(token.bits);
    if (token.tag <= wire::I8) return Operand(std::bit_cast<std::int64_t>(token.bits));
    if (token.tag == wire::F) return Operand(std::bit_cast<float>(static_cast<std::uint32_t>(token.bits)));
    if (token.tag == wire::D) return Operand(std::bit_cast<double>(token.bits));
    if (token.tag == wire::LD) { long double value{}; std::memcpy(&value, token.extended.data(), wire::long_double_bytes); return Operand(value); }
    if (token.tag >= wire::STR1 && token.tag <= wire::STR4) { auto value = string(token.bits); if (!value) return value.error(); return Operand(std::string(*value)); }
    return invalid("expected literal");
  }
  Result<ParsedOperand> operand(Token value) {
    if (value.tag <= wire::LD || (value.tag >= wire::STR1 && value.tag <= wire::STR4)) {
      auto l = literal(value); if (!l) return l.error(); return ParsedOperand(std::move(*l));
    }
    if (value.tag >= wire::REG1 && value.tag <= wire::REG4) { auto n = terminated(value.bits); if (!n) return n.error(); return ParsedOperand(RegisterName{*n}); }
    if (value.tag >= wire::NAME1 && value.tag <= wire::NAME4) { auto n = terminated(value.bits); if (!n) return n.error(); return ParsedOperand(SymbolName{*n}); }
    if (value.tag >= wire::LAB1 && value.tag <= wire::LAB4) return ParsedOperand(LabelNumber{static_cast<std::uint32_t>(value.bits)});
    bool aliased = value.tag >= wire::ALIAS_DISP && value.tag <= wire::ALIAS_LAST;
    if ((value.tag >= wire::MEM_DISP && value.tag <= wire::MEM_LAST) || aliased) {
      unsigned form = value.tag - (aliased ? wire::ALIAS_DISP : wire::MEM_DISP);
      auto t = type(true); if (!t) return t.error(); ParsedMemory result{Memory{*t}, {}, {}};
      if (form == 0 || form == 3 || form == 4 || form == 6) { auto displacement = sint(); if (!displacement) return displacement.error(); result.value.displacement = *displacement; }
      auto reg = [&]() -> Result<std::string_view> {
        auto token = this->token(); if (!token) return token.error(); if (token->tag < wire::REG1 || token->tag > wire::REG4) return invalid("expected address register"); return terminated(token->bits);
      };
      if (form == 1 || form == 3 || form == 5 || form == 6) { auto n = reg(); if (!n) return n.error(); result.base = *n; }
      if (form == 2 || form == 4 || form == 5 || form == 6) { auto n = reg(); if (!n) return n.error(); result.index = *n; auto scale = uint(); if (!scale) return scale.error(); if (*scale > 255) return invalid("invalid address scale"); result.value.scale = *scale; }
      if (aliased) { auto a = name(); if (!a) return a.error(); auto b = name(); if (!b) return b.error(); result.value.alias = *a; result.value.nonalias = *b; }
      if (t->is_block()) {
        if (result.value.displacement <= 0) return invalid("invalid block memory size");
        result.value.type = t->kind() == Type::Kind::ReturnBlock ? Type::return_block(result.value.displacement) : Type::block(result.value.displacement, t->block_class()); result.value.displacement = 0;
      }
      return ParsedOperand(std::move(result));
    }
    return invalid("invalid operand tag");
  }
  Result<ParsedInitializer> data(std::string_view kind) {
    if (kind == "bss") { auto size = uint(); if (!size) return size.error(); if (*size > std::numeric_limits<std::size_t>::max()) return invalid("BSS exceeds addressable storage"); return ParsedInitializer(ZeroData{static_cast<std::size_t>(*size)}); }
    if (kind == "ref" || kind == "expr") {
      auto target = name(); if (!target) return target.error(); std::int64_t displacement = 0;
      if (kind == "ref") { auto value = sint(); if (!value) return value.error(); displacement = *value; }
      return ParsedInitializer(ReferenceData{*target, displacement, kind == "expr"});
    }
    if (kind == "lref") {
      auto label = sint(); if (!label) return label.error(); auto base = sint(); if (!base) return base.error(); auto displacement = sint(); if (!displacement) return displacement.error();
      if (*label < 0 || *label > UINT32_MAX || *base < -1 || *base > UINT32_MAX) return invalid("invalid label data identity");
      return ParsedInitializer(LabelData{static_cast<std::uint32_t>(*label), *base == -1 ? std::nullopt : std::optional(static_cast<std::uint32_t>(*base)), *displacement});
    }
    auto element_type = type(); if (!element_type) return element_type.error(); if (!element_type->is_scalar()) return invalid("data type must be scalar");
    InitializedData result{*element_type, {}};
    for (;;) {
      auto value = token(); if (!value) return value.error(); if (value->tag == wire::EOI) break;
      unsigned number = type_code(*element_type);
      bool valid = number <= unsigned(wire::Type::U64) ? ((number & 1) ? value->tag <= wire::U8 : value->tag >= wire::I1 && value->tag <= wire::I8)
        : number == unsigned(wire::Type::F) ? value->tag == wire::F : number == unsigned(wire::Type::D) ? value->tag == wire::D : number == unsigned(wire::Type::LD) && value->tag == wire::LD;
      if (!valid) return invalid("data scalar and literal types differ"); auto element = literal(*value); if (!element) return element.error(); result.values.push_back(std::move(*element));
    }
    return ParsedInitializer(std::move(result));
  }
  Result<void> parse_module() {
    records_.clear(); labels_.clear(); ParsedFunction *function = nullptr;
    for (;;) {
      std::vector<std::uint32_t> labels; auto value = token(); if (!value) return value.error();
      while (value->tag >= wire::LAB1 && value->tag <= wire::LAB4) { labels.push_back(value->bits); value = token(); if (!value) return value.error(); }
      if (value->tag <= wire::U8) {
        if (!function) return invalid("instruction outside function"); auto code = opcode(value->bits);
        if (!code || *code == Opcode::Label) return invalid("private or invalid opcode");
        if (instruction_count_++ >= options_.max_instructions) return invalid("instruction limit exceeded");
        auto description = instruction_descriptor(*code); ParsedInstruction instruction{*code, std::move(labels), {}};
        for (std::size_t n = 0; description->count != OperandCount::Fixed || n < description->roles.size(); ++n) {
          auto argument = token(); if (!argument) return argument.error();
          if (argument->tag == wire::EOI) { if (description->count == OperandCount::Fixed) return invalid("too few instruction operands"); break; }
          auto parsed = operand(*argument); if (!parsed) return parsed.error(); instruction.operands.push_back(std::move(*parsed));
        }
        function->instructions.push_back(std::move(instruction)); continue;
      }
      if (!labels.empty()) return invalid("labels before a module record");
      auto kind = name(*value); if (!kind) return kind.error();
      if (*kind == "endfunc") { if (!function) return invalid("endfunc outside function"); function = nullptr; function_name_ = {}; continue; }
      if (*kind == "local" || *kind == "global") {
        if (!function) return invalid("register declaration outside function");
        for (;;) {
          auto value = token(); if (!value) return value.error(); if (value->tag == wire::EOI) break;
          auto t = type(*value); if (!t) return t.error(); if (!t->is_scalar()) return invalid("local register must be scalar");
          auto n = name(); if (!n) return n.error(); std::string_view hardware;
          if (*kind == "global") { auto h = name(); if (!h) return h.error(); if (h->empty()) return invalid("empty hardware binding"); hardware = *h; }
          function->locals.push_back({*t, *n, hardware});
        }
        continue;
      }
      if (function) return invalid("module record inside function");
      if (*kind == "endmodule") return {};
      if (*kind == "func" || *kind == "proto") {
        auto n = name(); if (!n) return n.error(); function_name_ = *kind == "func" ? *n : std::string_view{};
        auto body = signature(*kind == "func"); if (!body) return body.error();
        records_.push_back({*kind == "func" ? Record::Kind::Function : Record::Kind::Prototype, *n, std::move(*body)});
        if (*kind == "func") function = &std::get<ParsedFunction>(records_.back().body); continue;
      }
      if (*kind == "import" || *kind == "forward" || *kind == "export") {
        auto n = name(); if (!n) return n.error();
        records_.push_back({*kind == "import" ? Record::Kind::Import : *kind == "forward" ? Record::Kind::Forward : Record::Kind::Export, *n, {}}); continue;
      }
      auto spelling = *kind; bool named = spelling.starts_with('n'); if (named) spelling.remove_prefix(1);
      if (spelling != "data" && spelling != "bss" && spelling != "ref" && spelling != "lref" && spelling != "expr") return invalid("unknown module record");
      std::string_view n; if (named) { auto name = this->name(); if (!name) return name.error(); n = *name; if (n.empty()) return invalid("empty named data symbol"); }
      auto initializer = data(spelling); if (!initializer) return initializer.error(); records_.push_back({Record::Kind::Data, n, std::move(*initializer)});
    }
  }
  Result<void> build() {
    for (auto &record : records_) if (!record.name.empty()) { auto reserved = module_->reserve_symbol(record.name); if (!reserved) return diagnose(reserved.error()); }
    for (auto &record : records_) {
      switch (record.kind) {
      case Record::Kind::Function: {
        auto &body = std::get<ParsedFunction>(record.body); function_name_ = record.name;
        auto created = module_->create_function(record.name, body.signature, body.arguments); if (!created) return diagnose(created.error()); body.function = *created;
        for (auto &reg : body.locals) { auto added = body.function->create_register(reg.type, reg.name, reg.hardware); if (!added) return diagnose(added.error()); }
        auto created_block = body.function->create_block(); if (!created_block) return diagnose(created_block.error()); auto block = *created_block; bool occupied = false, terminated = false;
        for (auto &instruction : body.instructions) {
          if (terminated || (occupied && !instruction.labels.empty())) { auto next = body.function->create_block(); if (!next) return diagnose(next.error()); block = *next; }
          instruction.block = block;
          for (auto number : instruction.labels) {
            if (labels_.contains(number)) return invalid("duplicate module label identity");
            auto label = body.function->create_label(*block); if (!label) return diagnose(label.error()); labels_.emplace(number, *label);
          }
          occupied = true; terminated = instruction_descriptor(instruction.opcode)->flow != ControlFlow::Next;
        }
        function_name_ = {}; break;
      }
      case Record::Kind::Prototype: { auto p = module_->prototype(record.name, std::get<ParsedFunction>(record.body).signature); if (!p) return diagnose(p.error()); break; }
      case Record::Kind::Import: { auto imported = module_->import(record.name); if (!imported) return diagnose(imported.error()); break; }
      case Record::Kind::Forward: { auto forward = module_->forward(record.name); if (!forward) return diagnose(forward.error()); break; }
      case Record::Kind::Export: { auto exported = module_->export_symbol(*module_->find_symbol(record.name)); if (!exported) return diagnose(exported.error()); break; }
      case Record::Kind::Data: { auto declared = module_->declare_data(record.name); if (!declared) return diagnose(declared.error()); record.symbol = *declared; break; }
      }
    }
    for (auto &record : records_) if (record.kind == Record::Kind::Function) {
      auto &body = std::get<ParsedFunction>(record.body); function_name_ = record.name;
      auto reg = [&](std::string_view name) -> Result<const Register *> { auto result = body.function->find_register(name); if (!result) return invalid("undeclared register: " + std::string(name)); return result; };
      for (auto &instruction : body.instructions) {
        std::vector<Operand> operands; operands.reserve(instruction.operands.size());
        for (auto &argument : instruction.operands) {
          auto value = std::visit([&](const auto &value) -> Result<Operand> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, Operand>) return value;
            else if constexpr (std::is_same_v<T, RegisterName>) { auto r = reg(value.name); if (!r) return r.error(); return Operand(**r); }
            else if constexpr (std::is_same_v<T, SymbolName>) { auto symbol = module_->find_symbol(value.name); if (!symbol) return invalid("undeclared symbol"); return Operand(*symbol); }
            else if constexpr (std::is_same_v<T, LabelNumber>) { auto label = labels_.find(value.number); if (label == labels_.end()) return invalid("undefined label"); return Operand(*label->second); }
            else {
              auto m = value.value;
              if (value.base) { auto r = reg(*value.base); if (!r) return r.error(); m.base = *r; }
              if (value.index) { auto r = reg(*value.index); if (!r) return r.error(); m.index = *r; }
              return Operand(std::move(m));
            }
          }, argument);
          if (!value) return value.error(); operands.push_back(std::move(*value));
        }
        auto emitted = IRBuilder(*instruction.block).emit(instruction.opcode, operands); if (!emitted) return diagnose(emitted.error());
      }
      function_name_ = {};
    }
    for (auto &record : records_) if (record.kind == Record::Kind::Data) {
      auto initializer = std::visit([&](const auto &value) -> Result<DataInitializer> {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, InitializedData> || std::is_same_v<T, ZeroData>) return DataInitializer(value);
        else if constexpr (std::is_same_v<T, ReferenceData>) {
          auto symbol = module_->find_symbol(value.name); if (!symbol) return invalid("undefined data symbol");
          if (value.expression) { if (!symbol->function()) return invalid("expression requires a function"); return DataInitializer(ExpressionData{symbol->function()}); }
          return DataInitializer(SymbolReference{symbol, value.displacement});
        } else {
          auto first = labels_.find(value.label); if (first == labels_.end()) return invalid("undefined data label");
          const Label *base = nullptr; if (value.base) { auto second = labels_.find(*value.base); if (second == labels_.end()) return invalid("undefined base label"); base = second->second; }
          return DataInitializer(LabelReference{first->second, base, value.displacement});
        }
      }, std::get<ParsedInitializer>(record.body));
      if (!initializer) return initializer.error(); auto defined = module_->define_data(*record.symbol, std::move(*initializer)); if (!defined) return diagnose(defined.error());
    }
    return {};
  }
};
}
Result<std::vector<std::byte>> write_binary(const ir::Context &context, BinaryEncoding encoding) {
  for (auto &module : context.modules()) for (auto &function : module.functions()) {
    auto phase = function.require_register_ir(); if (!phase) return phase.error();
  }
  if (encoding != BinaryEncoding::Compressed && encoding != BinaryEncoding::Raw) return Error{ErrorCode::InvalidArgument, "invalid binary encoding"};
  if (ir::needs_materialization(context)) {
    auto lowered = ir::materialize(context); if (!lowered) return lowered.error(); return write_binary(**lowered, encoding);
  }
  auto raw = Writer().run(context); if (!raw || encoding == BinaryEncoding::Raw) return raw; return compress(*raw);
}
Result<std::unique_ptr<ir::Context>> read_binary(std::span<const std::byte> bytes, BinaryOptions options) {
  if (options.encoding != BinaryEncoding::Compressed && options.encoding != BinaryEncoding::Raw)
    return Error{ErrorCode::InvalidArgument, "invalid binary encoding"};
  if (options.encoding == BinaryEncoding::Raw) return Reader(bytes, options).run();
  auto raw = decompress(bytes, options.max_bytes);
  if (!raw) return Error{raw.error().code, std::string(options.source_name) + ": " + raw.error().message};
  return Reader(*raw, options).run();
}
} // namespace mirnext::serialization
