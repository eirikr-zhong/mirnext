/* Licensed under the MIT License. Grammar derived from MIR,
   Copyright 2018-2024 Vladimir Makarov. No engine objects or parser callbacks. */
#include "mirnext/Serialization/Format.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <unordered_set>

namespace mirnext::serialization {
namespace {
using namespace ir;
bool digit (char c) { return c >= '0' && c <= '9'; }
bool name_character (char c, bool first) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c == '%'
         || c == '.' || (!first && digit (c));
}
bool identifier (std::string_view name) {
  if (name.empty () || !name_character (name[0], true)) return false;
  return std::ranges::all_of (name.substr (1), [] (char c) { return name_character (c, false); });
}
std::optional<Type> named_type (std::string_view name) {
  constexpr std::string_view names[]{"bool", "i8",  "u8", "i16", "u16", "i32", "u32",
                                     "i64",  "u64", "f",  "d",   "ld",  "p"};
  for (unsigned n = 0; n < std::size (names); ++n)
    if (name == names[n]) return Type (static_cast<Type::Kind> (n));
  if (name == "undef") return Type (Type::Kind::Undef);
  if (name == "rblk") return Type::return_block (0);
  if (name.starts_with ("blk")) {
    auto suffix = name.substr (3);
    unsigned abi = 0;
    if (!suffix.empty ()) {
      auto [last, error] = std::from_chars (suffix.data (), suffix.data () + suffix.size (), abi);
      if (error != std::errc{} || last != suffix.data () + suffix.size ()) return {};
    }
    if (abi <= 4) return Type::block (0, static_cast<Type::BlockClass> (abi));
  }
  return {};
}
std::string type_name (Type type) {
  constexpr std::string_view names[]{"i64", "i8", "u8", "i16", "u16", "i32", "u32",  "i64",
                                     "u64", "f",  "d",  "ld",  "p",   "blk", "rblk", "undef"};
  if (type.kind () == Type::Kind::Block && type.block_class () != Type::BlockClass::Default)
    return "blk" + std::to_string (static_cast<unsigned> (type.block_class ()));
  return std::string (names[static_cast<unsigned> (type.kind ())]);
}
struct Token {
  enum class Kind { Name, Literal, Colon, Comma, Left, Right, Separator, End };
  Kind kind;
  std::string_view text;
  std::size_t literal = 0;
  std::size_t line, column;
};
struct TokenStream {
  std::vector<Token> tokens;
  std::vector<Operand> literals;
};
class Lexer {
 public:
  Lexer (std::string_view input, ParseOptions options) : input_ (input), options_ (options) {}
  Result<TokenStream> scan () {
    TokenStream stream;
    auto &tokens = stream.tokens;
    auto &literals = stream.literals;
    tokens.reserve (std::min ({input_.size () / 6, options_.max_tokens, std::size_t{65536}}));
    literals.reserve (std::min ({input_.size () / 32, options_.max_tokens, std::size_t{16384}}));
    while (position_ < input_.size ()) {
      auto line = line_, column = column_;
      char c = get ();
      if (c == '\0') return error ("NUL outside string", line, column);
      if (c == ' ' || c == '\t' || c == '\r') continue;
      if (c == '#') {
        while (position_ < input_.size () && input_[position_] != '\n') get ();
        continue;
      }
      Token token{Token::Kind::Name, {}, 0, line, column};
      switch (c) {
      case '\n':
      case ';': token.kind = Token::Kind::Separator; break;
      case ':': token.kind = Token::Kind::Colon; break;
      case ',': token.kind = Token::Kind::Comma; break;
      case '(': token.kind = Token::Kind::Left; break;
      case ')': token.kind = Token::Kind::Right; break;
      case '"': {
        auto value = quoted (line, column);
        if (!value) return value.error ();
        token.kind = Token::Kind::Literal;
        token.literal = literals.size ();
        literals.emplace_back (std::move (*value));
        break;
      }
      default:
        if (digit (c) || c == '+' || c == '-') {
          auto start = position_ - 1;
          while (position_ < input_.size ()) {
            auto next = input_[position_];
            if (next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == ','
                || next == ':' || next == ';' || next == '(' || next == ')' || next == '#')
              break;
            get ();
          }
          auto value = number (input_.substr (start, position_ - start), line, column);
          if (!value) return value.error ();
          token.kind = Token::Kind::Literal;
          token.literal = literals.size ();
          literals.push_back (std::move (*value));
        } else if (name_character (c, true)) {
          auto start = position_ - 1;
          while (position_ < input_.size () && name_character (input_[position_], false)) get ();
          token.text = input_.substr (start, position_ - start);
        } else
          return error ("invalid character", line, column);
      }
      if (tokens.size () >= options_.max_tokens)
        return error ("token limit exceeded", line, column);
      tokens.push_back (std::move (token));
    }
    tokens.push_back ({Token::Kind::End, {}, 0, line_, column_});
    return stream;
  }

 private:
  Error error (std::string message, std::size_t line, std::size_t column) const {
    return {ErrorCode::InvalidOperand, std::string (options_.source_name) + ":"
                                         + std::to_string (line) + ":" + std::to_string (column)
                                         + ": " + std::move (message)};
  }
  char get () {
    auto c = input_[position_++];
    if (c == '\n') {
      ++line_;
      column_ = 1;
    } else
      ++column_;
    return c;
  }
  Result<std::string> quoted (std::size_t line, std::size_t column) {
    std::string value;
    while (position_ < input_.size ()) {
      auto c = get ();
      if (c == '\n') return error ("unfinished string", line, column);
      if (c == '"') {
        if (value.empty () || value.back () != '\0') value.push_back ('\0');
        return value;
      }
      if (c == '\\') {
        if (position_ == input_.size ()) return error ("unfinished escape", line, column);
        c = get ();
        switch (c) {
        case '\n': continue;
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'v': c = '\v'; break;
        case 'a': c = '\a'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        default:
          if (c >= '0' && c <= '7') {
            unsigned byte = c - '0';
            for (unsigned n = 0; n < 2 && position_ < input_.size () && input_[position_] >= '0'
                                 && input_[position_] <= '7';
                 ++n)
              byte = byte * 8 + get () - '0';
            c = static_cast<char> (byte);
          } else if (c == 'x') {
            unsigned byte = 0;
            for (unsigned n = 0; n < 2; ++n) {
              if (position_ == input_.size ())
                return error ("unfinished hexadecimal escape", line, column);
              auto h = get ();
              unsigned v = digit (h)              ? h - '0'
                           : h >= 'a' && h <= 'f' ? h - 'a' + 10
                           : h >= 'A' && h <= 'F' ? h - 'A' + 10
                                                  : 16;
              if (v == 16) return error ("invalid hexadecimal escape", line, column);
              byte = byte * 16 + v;
            }
            c = static_cast<char> (byte);
          }
        }
      }
      value.push_back (c);
    }
    return error ("unfinished string", line, column);
  }
  Result<Operand> number (std::string_view original, std::size_t line, std::size_t column) {
    std::string spelling;
    spelling.reserve (original.size ());
    for (auto c : original)
      if (c != '_') spelling += c;
    auto body = std::string_view (spelling);
    bool negative = false;
    if (!body.empty () && (body[0] == '-' || body[0] == '+')) {
      negative = body[0] == '-';
      body.remove_prefix (1);
    }
    bool hex = body.starts_with ("0x") || body.starts_with ("0X");
    bool floating = body.find ('.') != body.npos || (!hex && body.find_first_of ("eE") != body.npos)
                    || body.starts_with ("inf") || body.starts_with ("nan");
    if (floating) {
      char suffix = spelling.back ();
      bool single = (suffix == 'f' || suffix == 'F') && body != "inf",
           extended = suffix == 'l' || suffix == 'L';
      if (single || extended) spelling.pop_back ();
      auto parse = [&]<class T> () -> Result<Operand> {
        auto text = std::string_view (spelling);
        if (!text.empty () && text.front () == '+') text.remove_prefix (1);
        bool signed_value = text.starts_with ('-');
        auto magnitude = signed_value ? text.substr (1) : text;
        if (magnitude == "inf" || magnitude == "nan") {
          auto value = magnitude == "inf" ? std::numeric_limits<T>::infinity ()
                                          : std::numeric_limits<T>::quiet_NaN ();
          return Operand (std::copysign (value, signed_value ? T{-1} : T{1}));
        }
        T value{};
        if constexpr (!std::is_same_v<T, long double> && requires {
                        std::from_chars (text.data (), text.data () + text.size (), value);
                      }) {
          auto [last, status] = std::from_chars (text.data (), text.data () + text.size (), value);
          if (status != std::errc{} || last != text.data () + text.size ())
            return error ("invalid or out-of-range floating literal", line, column);
        } else {
          // Older libc++ also lacks float/double from_chars. Keep the fallback
          // independent of both the global C++ locale and the process C locale.
          std::istringstream stream (spelling);
          stream.imbue (std::locale::classic ());
          stream >> std::noskipws >> value;
          if (stream.fail () || stream.peek () != std::char_traits<char>::eof ())
            return error ("invalid floating literal", line, column);
        }
        return Operand (value);
      };
      if (single) return parse.template operator()<float> ();
      if (extended) return parse.template operator()<long double> ();
      return parse.template operator()<double> ();
    }
    unsigned base = hex ? 16 : body.size () > 1 && body[0] == '0' ? 8 : 10;
    if (hex) body.remove_prefix (2);
    std::uint64_t bits = 0;
    auto [last, status] = std::from_chars (body.data (), body.data () + body.size (), bits, base);
    if (body.empty () || status != std::errc{} || last != body.data () + body.size ())
      return error ("invalid or out-of-range integer literal", line, column);
    if (negative) bits = std::uint64_t{0} - bits;
    return Operand (std::bit_cast<std::int64_t> (bits));
  }
  std::string_view input_;
  ParseOptions options_;
  std::size_t position_ = 0, line_ = 1, column_ = 1;
};
struct Statement {
  std::vector<std::string_view> labels;
  std::string_view name;
  std::span<const Token> arguments;
  std::size_t line, column;
};
struct Declaration {
  Type type;
  std::string_view name, hardware;
};
class Cursor {
 public:
  Cursor (std::span<const Token> tokens, std::span<const Operand> literals)
    : tokens_ (tokens), literals_ (literals) {}
  bool empty () const { return position_ == tokens_.size (); }
  const Token *peek () const { return empty () ? nullptr : &tokens_[position_]; }
  bool take (Token::Kind kind) {
    if (peek () && peek ()->kind == kind) {
      ++position_;
      return true;
    }
    return false;
  }
  Result<void> expect (Token::Kind kind, std::string message) {
    if (!take (kind)) return Error{ErrorCode::InvalidOperand, std::move (message)};
    return {};
  }
  Result<std::string_view> name () {
    if (!peek () || peek ()->kind != Token::Kind::Name)
      return Error{ErrorCode::InvalidOperand, "expected a name"};
    return tokens_[position_++].text;
  }
  Result<Operand> literal () {
    if (!peek () || peek ()->kind != Token::Kind::Literal)
      return Error{ErrorCode::InvalidOperand, "expected a literal"};
    return literals_[tokens_[position_++].literal];
  }
  Result<std::int64_t> integer () {
    auto value = literal ();
    if (!value) return value.error ();
    auto i = value->get<std::int64_t> ();
    if (!i) return Error{ErrorCode::InvalidOperand, "expected an integer"};
    return *i;
  }

 private:
  std::span<const Token> tokens_;
  std::span<const Operand> literals_;
  std::size_t position_ = 0;
};
Result<Declaration> declaration (Cursor &cursor, bool hardware = false) {
  auto spelling = cursor.name ();
  if (!spelling) return spelling.error ();
  auto type = named_type (*spelling);
  if (!type) return Error{ErrorCode::InvalidOperand, "unknown type: " + std::string (*spelling)};
  Declaration value{*type, {}, {}};
  if (!cursor.take (Token::Kind::Colon)) return value;
  if (type->is_block ()) {
    auto size = cursor.integer ();
    if (!size) return size.error ();
    if (*size <= 0) return Error{ErrorCode::InvalidOperand, "invalid block size"};
    value.type = type->kind () == Type::Kind::ReturnBlock
                   ? Type::return_block (*size)
                   : Type::block (*size, type->block_class ());
    auto open = cursor.expect (Token::Kind::Left, "expected block argument name");
    if (!open) return open.error ();
    auto name = cursor.name ();
    if (!name) return name.error ();
    value.name = *name;
    auto close = cursor.expect (Token::Kind::Right, "expected ')' after block argument");
    if (!close) return close.error ();
  } else {
    auto name = cursor.name ();
    if (!name) return name.error ();
    value.name = *name;
  }
  if (hardware) {
    auto colon = cursor.expect (Token::Kind::Colon, "expected hardware binding");
    if (!colon) return colon.error ();
    auto name = cursor.name ();
    if (!name) return name.error ();
    value.hardware = *name;
  }
  return value;
}
struct ParsedSignature {
  Signature signature;
  std::vector<std::string_view> names;
};
Result<ParsedSignature> signature (std::span<const Token> tokens,
                                   std::span<const Operand> literals) {
  Cursor cursor (tokens, literals);
  ParsedSignature result;
  bool arguments = false;
  while (!cursor.empty ()) {
    if (cursor.peek ()->kind == Token::Kind::Name && cursor.peek ()->text == "...") {
      auto dots = cursor.name ();
      (void) dots;
      result.signature.variadic = true;
      if (!cursor.empty ()) return Error{ErrorCode::InvalidOperand, "variadic marker must be last"};
      break;
    }
    auto value = declaration (cursor);
    if (!value) return value.error ();
    if (!value->name.empty ()) arguments = true;
    if (arguments) {
      if (value->name.empty ()) return Error{ErrorCode::InvalidOperand, "parameter has no name"};
      result.signature.parameters.push_back (value->type);
      result.names.push_back (value->name);
    } else
      result.signature.results.push_back (value->type);
    if (cursor.empty ()) break;
    auto comma = cursor.expect (Token::Kind::Comma, "expected ',' in signature");
    if (!comma) return comma.error ();
    if (cursor.empty ()) return Error{ErrorCode::InvalidOperand, "trailing signature comma"};
  }
  auto valid = result.signature.verify ();
  if (!valid) return valid.error ();
  return result;
}
class Parser {
 public:
  Parser (const TokenStream &stream, ParseOptions options)
    : tokens_ (stream.tokens),
      literals_ (stream.literals),
      options_ (options),
      result_ (std::make_unique<Context> ()) {}
  Result<std::unique_ptr<Context>> run () {
    auto statements = split ();
    if (!statements) return statements.error ();
    statements_ = std::move (*statements);
    for (std::size_t n = 0; n < statements_.size ();) {
      auto &start = statements_[n];
      current_ = &start;
      if (start.name != "module" || start.labels.size () != 1 || !start.arguments.empty ())
        return invalid ("expected named module");
      auto created = result_->create_module (start.labels[0]);
      if (!created) return diagnose (created.error ());
      module_ = *created;
      auto end = n + 1;
      while (end < statements_.size () && statements_[end].name != "endmodule") ++end;
      if (end == statements_.size ()) return invalid ("missing endmodule");
      if (!statements_[end].labels.empty () || !statements_[end].arguments.empty ()) {
        current_ = &statements_[end];
        return invalid ("invalid endmodule");
      }
      auto built = module (n + 1, end);
      if (!built) return built.error ();
      n = end + 1;
    }
    auto valid = result_->verify ();
    if (!valid) return diagnose (valid.error ());
    return std::move (result_);
  }

 private:
  struct Body {
    Function *function;
    std::size_t first, last;
    std::vector<BasicBlock *> blocks;
  };
  std::span<const Token> tokens_;
  std::span<const Operand> literals_;
  ParseOptions options_;
  std::unique_ptr<Context> result_;
  std::vector<Statement> statements_;
  const Statement *current_ = nullptr;
  Module *module_ = nullptr;
  StringMap<const Label *> labels_;
  std::vector<std::pair<std::size_t, Symbol *>> data_;
  static const StringMap<Opcode> &opcodes () {
    static const auto table = [] {
      StringMap<Opcode> result;
      result.reserve (instruction_descriptors ().size ());
      for (auto &descriptor : instruction_descriptors ())
        result.emplace (descriptor.name, descriptor.opcode);
      return result;
    }();
    return table;
  }
  std::size_t instruction_count_ = 0;
  Error diagnose (Error error) const {
    error.message
      = std::string (options_.source_name) + ":" + std::to_string (current_ ? current_->line : 1)
        + ":" + std::to_string (current_ ? current_->column : 1) + ": " + std::move (error.message);
    return error;
  }
  Error invalid (std::string message) const {
    return diagnose ({ErrorCode::InvalidOperand, std::move (message)});
  }
  Result<std::vector<Statement>> split () {
    std::vector<Statement> result;
    std::size_t n = 0;
    while (tokens_[n].kind != Token::Kind::End) {
      if (tokens_[n].kind == Token::Kind::Separator) {
        ++n;
        continue;
      }
      Statement statement{{}, {}, {}, tokens_[n].line, tokens_[n].column};
      while (tokens_[n].kind == Token::Kind::Name && tokens_[n + 1].kind == Token::Kind::Colon) {
        statement.labels.push_back (tokens_[n].text);
        n += 2;
        while (tokens_[n].kind == Token::Kind::Separator) ++n;
      }
      if (tokens_[n].kind != Token::Kind::Name) {
        current_ = &statement;
        return invalid ("expected statement name after labels");
      }
      statement.name = tokens_[n++].text;
      auto start = n;
      while (tokens_[n].kind != Token::Kind::Separator && tokens_[n].kind != Token::Kind::End) ++n;
      statement.arguments = tokens_.subspan (start, n - start);
      result.push_back (std::move (statement));
    }
    current_ = nullptr;
    return result;
  }
  bool datum (const Statement &statement) const {
    return named_type (statement.name).has_value () || statement.name == "bss"
           || statement.name == "ref" || statement.name == "lref" || statement.name == "expr"
           || statement.name == "string";
  }
  Result<void> emit (IRBuilder &builder, Opcode opcode, std::span<const Operand> operands) {
    if (instruction_count_ >= options_.max_instructions)
      return invalid ("instruction limit exceeded");
    auto instruction = builder.emit (opcode, operands);
    if (!instruction) return diagnose (instruction.error ());
    ++instruction_count_;
    return {};
  }
  Result<void> module (std::size_t first, std::size_t last) {
    labels_.clear ();
    data_.clear ();
    std::vector<Body> bodies;
    // Reserve symbols without publishing extra records, then preserve every input slot.
    bool in_function = false;
    for (auto n = first; n < last; ++n) {
      auto &s = statements_[n];
      current_ = &s;
      if (s.name == "func") {
        if (in_function) return invalid ("nested function");
        in_function = true;
      } else if (s.name == "endfunc") {
        if (!in_function) return invalid ("endfunc outside function");
        in_function = false;
      } else if (in_function)
        continue;
      if (s.name == "func" || s.name == "proto" || datum (s)) {
        if ((s.name == "func" || s.name == "proto") ? s.labels.size () != 1 : s.labels.size () > 1)
          return invalid ("wrong number of item names");
        if (!s.labels.empty ()) {
          auto reserved = module_->reserve_symbol (s.labels[0]);
          if (!reserved) return diagnose (reserved.error ());
        }
      } else if (s.name == "import" || s.name == "export" || s.name == "forward") {
        Cursor cursor (s.arguments, literals_);
        while (!cursor.empty ()) {
          auto name = cursor.name ();
          if (!name) return diagnose (name.error ());
          auto reserved = module_->reserve_symbol (*name);
          if (!reserved) return diagnose (reserved.error ());
          if (!cursor.empty ()) {
            auto comma = cursor.expect (Token::Kind::Comma, "expected declaration comma");
            if (!comma) return diagnose (comma.error ());
            if (cursor.empty ()) return invalid ("trailing declaration comma");
          }
        }
      }
    }
    if (in_function) return invalid ("missing endfunc");
    for (auto n = first; n < last; ++n) {
      auto &s = statements_[n];
      current_ = &s;
      if (s.name == "func" || s.name == "proto") {
        auto parsed = signature (s.arguments, literals_);
        if (!parsed) return diagnose (parsed.error ());
        if (s.name == "proto") {
          auto prototype = module_->prototype (s.labels[0], std::move (parsed->signature));
          if (!prototype) return diagnose (prototype.error ());
          continue;
        }
        auto function
          = module_->create_function (s.labels[0], std::move (parsed->signature), parsed->names);
        if (!function) return diagnose (function.error ());
        auto end = n + 1;
        while (end < last && statements_[end].name != "endfunc") ++end;
        Body body{*function, n + 1, end, std::vector<BasicBlock *> (end - n - 1)};
        auto prepared = prepare (body);
        if (!prepared) return prepared;
        bodies.push_back (std::move (body));
        n = end;
      } else if (datum (s)) {
        auto declared
          = module_->declare_data (s.labels.empty () ? std::string_view{} : s.labels[0]);
        if (!declared) return diagnose (declared.error ());
        data_.emplace_back (n, *declared);
      } else if (s.name == "import" || s.name == "export" || s.name == "forward") {
        if (!s.labels.empty () || s.arguments.empty ()) return invalid ("invalid declaration");
        Cursor cursor (s.arguments, literals_);
        while (!cursor.empty ()) {
          auto name = cursor.name ();
          if (!name) return diagnose (name.error ());
          Result<void> added;
          if (s.name == "import") {
            auto value = module_->import (*name);
            if (!value) added = value.error ();
          } else if (s.name == "forward") {
            auto value = module_->forward (*name);
            if (!value) added = value.error ();
          } else
            added = module_->export_symbol (*module_->find_symbol (*name));
          if (!added) return diagnose (added.error ());
          if (!cursor.empty ()) cursor.take (Token::Kind::Comma);
        }
      } else
        return invalid ("unexpected module record: " + std::string (s.name));
    }
    for (auto &body : bodies) {
      auto built = instructions (body);
      if (!built) return built;
    }
    for (auto [n, symbol] : data_) {
      current_ = &statements_[n];
      auto value = initializer (*current_);
      if (!value) return value.error ();
      auto defined = module_->define_data (*symbol, std::move (*value));
      if (!defined) return diagnose (defined.error ());
    }
    return {};
  }
  Result<void> prepare (Body &body) {
    auto entry = body.function->create_block ();
    if (!entry) return diagnose (entry.error ());
    auto block = *entry;
    bool has_instruction = false, terminated = false;
    for (auto n = body.first; n < body.last; ++n) {
      auto &s = statements_[n];
      current_ = &s;
      if (s.name == "local" || s.name == "global") {
        if (!s.labels.empty () || s.arguments.empty ())
          return invalid ("invalid variable declaration");
        Cursor cursor (s.arguments, literals_);
        while (!cursor.empty ()) {
          auto value = declaration (cursor, s.name == "global");
          if (!value) return diagnose (value.error ());
          if (value->name.empty () || !value->type.is_scalar ())
            return invalid ("local/global variable must be named and scalar");
          auto reg = body.function->create_register (value->type, value->name, value->hardware);
          if (!reg) return diagnose (reg.error ());
          if (cursor.empty ()) break;
          auto comma = cursor.expect (Token::Kind::Comma, "expected variable comma");
          if (!comma) return diagnose (comma.error ());
          if (cursor.empty ()) return invalid ("trailing variable comma");
        }
        continue;
      }
      auto opcode = opcodes ().find (s.name);
      if (opcode == opcodes ().end ())
        return invalid ("unknown or private opcode: " + std::string (s.name));
      if (terminated
          || (has_instruction && (!s.labels.empty () || opcode->second == Opcode::Label))) {
        auto created = body.function->create_block ();
        if (!created) return diagnose (created.error ());
        block = *created;
        has_instruction = false;
      }
      for (auto name : s.labels) {
        if (labels_.contains (name))
          return invalid ("duplicate module label: " + std::string (name));
        auto label = body.function->create_label (*block, name);
        if (!label) return diagnose (label.error ());
        labels_.emplace (name, *label);
      }
      if (opcode->second == Opcode::Label) {
        if (!s.arguments.empty ()) return invalid ("label opcode takes no operands");
        if (s.labels.empty ()) {
          auto anchor = body.function->create_label (*block);
          if (!anchor) return diagnose (anchor.error ());
        }
        terminated = false;
      } else {
        has_instruction = true;
        terminated = instruction_descriptor (opcode->second)->flow != ControlFlow::Next;
      }
      body.blocks[n - body.first] = block;
    }
    if (body.last >= statements_.size () || !statements_[body.last].labels.empty ()
        || !statements_[body.last].arguments.empty ())
      return invalid ("invalid endfunc");
    return {};
  }
  Result<Operand> memory (Cursor &cursor, Type type, Function &function, IRBuilder &builder) {
    Memory result{type};
    std::optional<std::int64_t> displacement;
    const Symbol *symbol = nullptr;
    if (cursor.peek () && cursor.peek ()->kind == Token::Kind::Literal) {
      auto value = cursor.integer ();
      if (!value) return diagnose (value.error ());
      displacement = *value;
    } else if (cursor.peek () && cursor.peek ()->kind == Token::Kind::Name) {
      auto name = cursor.name ();
      if (!name) return diagnose (name.error ());
      symbol = module_->find_symbol (*name);
      if (!symbol || symbol->kind () == Symbol::Kind::Prototype)
        return invalid ("undeclared memory symbol");
    }
    if (cursor.take (Token::Kind::Left)) {
      auto reg = [&] () -> Result<const Register *> {
        auto name = cursor.name ();
        if (!name) return diagnose (name.error ());
        auto value = function.find_register (*name);
        if (!value) return invalid ("undeclared address register: " + std::string (*name));
        return value;
      };
      if (cursor.peek () && cursor.peek ()->kind == Token::Kind::Name) {
        auto value = reg ();
        if (!value) return value.error ();
        result.base = *value;
      }
      if (cursor.take (Token::Kind::Comma)) {
        auto value = reg ();
        if (!value) return value.error ();
        result.index = *value;
        if (cursor.take (Token::Kind::Comma)) {
          auto scale = cursor.integer ();
          if (!scale) return scale.error ();
          if (*scale < 0 || *scale > 255) return invalid ("invalid memory scale");
          result.scale = *scale;
        }
      }
      auto close = cursor.expect (Token::Kind::Right, "expected ')' after address");
      if (!close) return diagnose (close.error ());
    } else if (!displacement && !symbol)
      return invalid ("empty memory address");
    if (type.is_block ()) {
      if (symbol || !displacement || *displacement <= 0)
        return invalid ("invalid block memory size");
      result.type = type.kind () == Type::Kind::ReturnBlock
                      ? Type::return_block (*displacement)
                      : Type::block (*displacement, type.block_class ());
    } else
      result.displacement = displacement.value_or (0);
    if (cursor.take (Token::Kind::Colon)) {
      if (!cursor.peek () || cursor.peek ()->kind != Token::Kind::Colon) {
        auto alias = cursor.name ();
        if (!alias) return diagnose (alias.error ());
        result.alias = *alias;
      }
      if (cursor.take (Token::Kind::Colon)) {
        auto alias = cursor.name ();
        if (!alias) return diagnose (alias.error ());
        result.nonalias = *alias;
      }
    }
    if (symbol) {
      auto reg = function.create_register (Type::pointer ());
      if (!reg) return diagnose (reg.error ());
      const Operand move[]{Operand (**reg), Operand (*symbol)};
      auto moved = emit (builder, Opcode::Mov, move);
      if (!moved) return moved.error ();
      if (result.base) {
        const Operand add[]{Operand (**reg), Operand (**reg), Operand (*result.base)};
        auto added = emit (builder, Opcode::Add, add);
        if (!added) return added.error ();
      }
      result.base = *reg;
    }
    return Operand (std::move (result));
  }
  Result<Operand> operand (Cursor &cursor, Opcode opcode, std::size_t index, Body &body,
                           IRBuilder &builder) {
    if (cursor.peek () && cursor.peek ()->kind == Token::Kind::Literal) return cursor.literal ();
    auto name = cursor.name ();
    if (!name) return diagnose (name.error ());
    if (cursor.take (Token::Kind::Colon)) {
      auto type = named_type (*name);
      if (!type) return invalid ("unknown memory type");
      return memory (cursor, *type, *body.function, builder);
    }
    auto descriptor = instruction_descriptor (opcode);
    bool label = (index < descriptor->roles.size () && descriptor->roles[index] == 'l')
                 || (opcode == Opcode::Switch && index != 0);
    if (label) {
      auto found = labels_.find (*name);
      if (found == labels_.end ()) return invalid ("undefined label: " + std::string (*name));
      return Operand (*found->second);
    }
    if (auto reg = body.function->find_register (*name)) return Operand (*reg);
    if (auto symbol = module_->find_symbol (*name)) return Operand (*symbol);
    if (*name == "inf" || *name == "nan" || *name == "inff" || *name == "nanf" || *name == "infL"
        || *name == "nanL") {
      bool nan = name->starts_with ("nan");
      if (*name == "inff" || *name == "nanf")
        return Operand (nan ? std::numeric_limits<float>::quiet_NaN ()
                            : std::numeric_limits<float>::infinity ());
      if (name->back () == 'L')
        return Operand (nan ? std::numeric_limits<long double>::quiet_NaN ()
                            : std::numeric_limits<long double>::infinity ());
      return Operand (nan ? std::numeric_limits<double>::quiet_NaN ()
                          : std::numeric_limits<double>::infinity ());
    }
    return invalid ("undeclared operand: " + std::string (*name));
  }
  Result<void> instructions (Body &body) {
    for (auto n = body.first; n < body.last; ++n)
      if (auto block = body.blocks[n - body.first]) {
        auto &s = statements_[n];
        current_ = &s;
        auto opcode = opcodes ().at (s.name);
        if (opcode == Opcode::Label) continue;
        IRBuilder builder (*block);
        Cursor cursor (s.arguments, literals_);
        std::vector<Operand> operands;
        while (!cursor.empty ()) {
          auto value = operand (cursor, opcode, operands.size (), body, builder);
          if (!value) return value.error ();
          operands.push_back (std::move (*value));
          if (cursor.empty ()) break;
          auto comma = cursor.expect (Token::Kind::Comma, "expected operand comma");
          if (!comma) return diagnose (comma.error ());
          if (cursor.empty ()) return invalid ("trailing operand comma");
        }
        auto added = emit (builder, opcode, operands);
        if (!added) return added;
      }
    auto &last = *std::prev (body.function->blocks ().end ());
    if (!last.terminator ()
        || instruction_descriptor (last.terminator ()->opcode ())->flow == ControlFlow::Branch) {
      if (!body.function->signature ().results.empty ())
        return invalid ("non-void function falls through");
      BasicBlock *exit = &last;
      if (last.terminator ()) {
        auto created = body.function->create_block ();
        if (!created) return diagnose (created.error ());
        exit = *created;
      }
      IRBuilder builder (*exit);
      auto added = emit (builder, Opcode::Ret, {});
      if (!added) return added;
    }
    return {};
  }
  Result<DataInitializer> initializer (const Statement &statement) {
    Cursor cursor (statement.arguments, literals_);
    auto kind = statement.name;
    if (kind == "bss") {
      auto size = cursor.integer ();
      if (!size) return diagnose (size.error ());
      if (*size < 0 || !cursor.empty ()) return invalid ("invalid bss size");
      return DataInitializer (ZeroData{static_cast<std::size_t> (*size)});
    }
    if (kind == "string") {
      auto literal = cursor.literal ();
      if (!literal) return diagnose (literal.error ());
      auto bytes = literal->get<std::string> ();
      if (!bytes || !cursor.empty ()) return invalid ("string data requires one string");
      InitializedData data{Type (Type::Kind::U8), {}};
      for (unsigned char byte : *bytes) data.values.emplace_back (std::uint64_t (byte));
      return DataInitializer (std::move (data));
    }
    if (kind == "ref" || kind == "expr") {
      auto name = cursor.name ();
      if (!name) return diagnose (name.error ());
      auto symbol = module_->find_symbol (*name);
      if (!symbol) return invalid ("undefined data reference");
      if (kind == "expr") {
        if (!cursor.empty () || !symbol->function ())
          return invalid ("invalid expression initializer");
        return DataInitializer (ExpressionData{symbol->function ()});
      }
      auto comma = cursor.expect (Token::Kind::Comma, "expected reference displacement");
      if (!comma) return diagnose (comma.error ());
      auto displacement = cursor.integer ();
      if (!displacement) return diagnose (displacement.error ());
      if (!cursor.empty ()) return invalid ("extra reference operand");
      return DataInitializer (SymbolReference{symbol, *displacement});
    }
    if (kind == "lref") {
      auto name = cursor.name ();
      if (!name) return diagnose (name.error ());
      auto label = labels_.find (*name);
      if (label == labels_.end ()) return invalid ("undefined label data reference");
      LabelReference value{label->second};
      if (!cursor.empty ()) {
        auto comma = cursor.expect (Token::Kind::Comma, "expected label reference comma");
        if (!comma) return diagnose (comma.error ());
        if (cursor.peek () && cursor.peek ()->kind == Token::Kind::Name) {
          auto base_name = cursor.name ();
          if (!base_name) return diagnose (base_name.error ());
          auto base = labels_.find (*base_name);
          if (base == labels_.end ()) return invalid ("undefined base label");
          value.base = base->second;
          if (!cursor.empty ()) {
            comma = cursor.expect (Token::Kind::Comma, "expected label displacement comma");
            if (!comma) return diagnose (comma.error ());
            auto displacement = cursor.integer ();
            if (!displacement) return diagnose (displacement.error ());
            value.displacement = *displacement;
          }
        } else {
          auto displacement = cursor.integer ();
          if (!displacement) return diagnose (displacement.error ());
          value.displacement = *displacement;
        }
      }
      if (!cursor.empty ()) return invalid ("extra label reference operand");
      return DataInitializer (value);
    }
    auto type = named_type (kind);
    if (!type || !type->is_scalar ()) return invalid ("invalid data type");
    InitializedData value{*type, {}};
    while (!cursor.empty ()) {
      auto element = cursor.literal ();
      if (!element) return diagnose (element.error ());
      value.values.push_back (std::move (*element));
      if (cursor.empty ()) break;
      auto comma = cursor.expect (Token::Kind::Comma, "expected data comma");
      if (!comma) return diagnose (comma.error ());
      if (cursor.empty ()) return invalid ("trailing data comma");
    }
    return DataInitializer (std::move (value));
  }
};
class Formatter {
 public:
  Result<std::string> run (const Context &context) {
    auto valid = context.verify ();
    if (!valid) return valid.error ();
    out_.imbue (std::locale::classic ());
    for (auto &module : context.modules ()) {
      auto written = write_module (module);
      if (!written) return written.error ();
    }
    return out_.str ();
  }

 private:
  std::ostringstream out_;
  const Module *module_ = nullptr;
  const Function *function_ = nullptr;
  std::unordered_set<std::string> names_;
  std::size_t next_name_ = 0, next_label_ = 0;
  std::unordered_map<const Register *, std::string> registers_;
  std::unordered_map<const Label *, std::string> labels_;
  std::unordered_map<const Instruction *, std::string> call_signatures_;
  using AddressTemporary = std::pair<std::size_t, std::string>;
  std::unordered_map<const Instruction *, std::vector<AddressTemporary>> address_temporaries_;
  std::unordered_map<const Signature *, std::string> generated_signatures_;
  std::vector<const Signature *> signature_order_;
  Error invalid (std::string message) const {
    return {ErrorCode::InvalidOperand,
            std::string (module_ ? module_->name () : "format")
              + (function_ ? "/" + std::string (function_->symbol ().name ()) : "")
              + ": text-writer: " + std::move (message)};
  }
  std::string fresh (std::string_view prefix) {
    for (;;) {
      auto candidate = std::string (prefix) + std::to_string (next_name_++);
      if (names_.insert (candidate).second) return candidate;
    }
  }
  void write_signature (const Signature &signature, const Function *function = nullptr) {
    bool comma = false;
    for (auto type : signature.results) {
      if (comma) out_ << ", ";
      out_ << type_name (type);
      comma = true;
    }
    for (std::size_t n = 0; n < signature.parameters.size (); ++n) {
      if (comma) out_ << ", ";
      auto type = signature.parameters[n];
      out_ << type_name (type) << ':';
      if (type.is_block ()) out_ << type.block_size () << '(';
      out_ << (function ? registers_.at (function->arguments ()[n]) : ".arg" + std::to_string (n));
      if (type.is_block ()) out_ << ')';
      comma = true;
    }
    if (signature.variadic) out_ << (comma ? ", ..." : "...");
    out_ << '\n';
  }
  void quoted (std::string_view value) {
    out_ << '"';
    auto byte = [&] (unsigned char c) {
      if (c == '"' || c == '\\')
        out_ << '\\' << char (c);
      else if (c == '\n')
        out_ << "\\n";
      else if (c == '\t')
        out_ << "\\t";
      else if (c >= 32 && c <= 126)
        out_ << char (c);
      else
        out_ << '\\' << char ('0' + ((c >> 6) & 7)) << char ('0' + ((c >> 3) & 7))
             << char ('0' + (c & 7));
    };
    for (unsigned char c : value) byte (c);
    if (value.empty () || value.back () != '\0') byte (0);
    out_ << '"';
  }
  template <class T>
  Result<void> floating (T value) {
    if (std::isnan (value)) {
      // Decimal MIR text cannot carry arbitrary NaN payloads. Diagnose loss rather
      // than changing bits; the binary format is the lossless representation.
      auto positive = std::copysign (value, T{1}), canonical = std::numeric_limits<T>::quiet_NaN ();
      auto bytes = std::bit_cast<std::array<std::byte, sizeof (T)>> (positive);
      auto expected = std::bit_cast<std::array<std::byte, sizeof (T)>> (canonical);
      constexpr auto significant = std::is_same_v<T, long double> && sizeof (T) == 16
#if defined(__x86_64__) && !defined(_WIN32)
                                     ? 10u
#else
                                     ? 16u
#endif
                                     : sizeof (T);
      if (!std::equal (bytes.begin (), bytes.begin () + significant, expected.begin ()))
        return invalid ("NaN payload cannot be represented in MIR text");
      out_ << (std::signbit (value) ? "-nan" : "+nan");
    } else if (std::isinf (value))
      out_ << (std::signbit (value) ? "-inf" : "+inf");
    else
      out_ << std::scientific << std::setprecision (std::numeric_limits<T>::max_digits10) << value;
    if constexpr (std::is_same_v<T, float>) out_ << 'f';
    if constexpr (std::is_same_v<T, long double>) out_ << 'L';
    return {};
  }
  Result<void> operand (const Operand &operand, std::string_view override_base = {}) {
    return std::visit (
      [&] (const auto &value) -> Result<void> {
        using T = std::decay_t<decltype (value)>;
        if constexpr (std::is_same_v<T, const Register *>)
          out_ << registers_.at (value);
        else if constexpr (std::is_same_v<T, const Label *>)
          out_ << labels_.at (value);
        else if constexpr (std::is_same_v<T, SymbolReference>) {
          if (value.displacement) return invalid ("symbol displacement was not materialized");
          out_ << value.symbol->name ();
        } else if constexpr (std::is_same_v<T, std::string>)
          quoted (value);
        else if constexpr (std::is_floating_point_v<T>)
          return floating (value);
        else if constexpr (std::is_same_v<T, Memory>) {
          if ((!value.alias.empty () && !identifier (value.alias))
              || (!value.nonalias.empty () && !identifier (value.nonalias)))
            return invalid ("alias cannot be represented as a MIR identifier");
          out_ << type_name (value.type) << ':';
          auto displacement = value.type.is_block ()
                                ? static_cast<std::int64_t> (value.type.block_size ())
                                : value.displacement;
          if (displacement || (override_base.empty () && !value.base && !value.index))
            out_ << displacement;
          if (!override_base.empty () || value.base || value.index) {
            out_ << '(';
            if (!override_base.empty ())
              out_ << override_base;
            else if (value.base)
              out_ << registers_.at (value.base);
            if (override_base.empty () && value.index) {
              out_ << ", " << registers_.at (value.index);
              if (value.scale != 1) out_ << ", " << value.scale;
            }
            out_ << ')';
          }
          if (!value.alias.empty () || !value.nonalias.empty ()) {
            out_ << ':' << value.alias;
            if (!value.nonalias.empty ()) out_ << ':' << value.nonalias;
          }
        } else if constexpr (std::is_same_v<T, std::uint64_t>)
          out_ << std::bit_cast<std::int64_t> (value);
        else
          out_ << value;
        return {};
      },
      operand.storage ());
  }
  Result<void> write_instruction (const Instruction &instruction) {
    auto found = address_temporaries_.find (&instruction);
    auto temporaries = found == address_temporaries_.end ()
                         ? std::span<const AddressTemporary>{}
                         : std::span<const AddressTemporary> (found->second);
    for (auto &[n, name] : temporaries) {
      if (auto ref = instruction.operands ()[n].get<SymbolReference> ()) {
        out_ << "\tmov " << name << ", " << ref->symbol->name () << "\n\tadd " << name << ", "
             << name << ", " << ref->displacement << '\n';
      } else {
        auto &memory = *instruction.operands ()[n].get<Memory> ();
        out_ << "\tmov " << name << ", ";
        if (memory.index)
          out_ << registers_.at (memory.index);
        else
          out_ << '0';
        out_ << '\n';
        if (memory.index && memory.scale != 1)
          out_ << "\tmul " << name << ", " << name << ", " << memory.scale << '\n';
        if (memory.base)
          out_ << "\tadd " << name << ", " << name << ", " << registers_.at (memory.base) << '\n';
        out_ << "\tadd " << name << ", " << name << ", " << memory.displacement << '\n';
      }
    }
    out_ << '\t' << instruction_descriptor (instruction.opcode ())->name;
    for (std::size_t n = 0; n < instruction.operands ().size (); ++n) {
      out_ << (n ? ", " : " ");
      if (!temporaries.empty () && temporaries.front ().first == n) {
        if (instruction.operands ()[n].get<Memory> ()) {
          auto written = operand (instruction.operands ()[n], temporaries.front ().second);
          if (!written) return written;
        } else
          out_ << temporaries.front ().second;
        temporaries = temporaries.subspan (1);
      } else if (!n && instruction_descriptor (instruction.opcode ())->count == OperandCount::Call)
        out_ << call_signatures_.at (&instruction);
      else {
        auto written = operand (instruction.operands ()[n]);
        if (!written) return written;
      }
    }
    out_ << '\n';
    return {};
  }
  Result<void> write_function (const Function &function) {
    function_ = &function;
    out_ << function.symbol ().name () << ": func ";
    write_signature (function.signature (), &function);
    for (auto &reg : function.registers ())
      if (!reg.is_argument ()) {
        auto type = reg.type ().is_integer () ? Type::i64 () : reg.type ();
        out_ << (reg.hardware_binding ().empty () ? "\tlocal " : "\tglobal ") << type_name (type)
             << ':' << registers_.at (&reg);
        if (!reg.hardware_binding ().empty ()) {
          if (!identifier (reg.hardware_binding ()))
            return invalid ("hardware binding cannot be represented as a MIR identifier");
          out_ << ':' << reg.hardware_binding ();
        }
        out_ << '\n';
      }
    for (auto &block : function.blocks ())
      for (auto &instruction : block.instructions ()) {
        auto found = address_temporaries_.find (&instruction);
        if (found != address_temporaries_.end ())
          for (auto &[n, name] : found->second) out_ << "\tlocal i64:" << name << '\n';
      }
    for (auto &block : function.blocks ()) {
      for (auto label : block.labels ()) out_ << labels_.at (label) << ":\n";
      for (auto &instruction : block.instructions ()) {
        auto written = write_instruction (instruction);
        if (!written) return written;
      }
    }
    out_ << "\tendfunc\n";
    function_ = nullptr;
    return {};
  }
  Result<void> write_data (const GlobalData &data) {
    if (!data.symbol ().name ().empty ())
      out_ << data.symbol ().name () << ": ";
    else
      out_ << '\t';
    return std::visit (
      [&] (const auto &value) -> Result<void> {
        using T = std::decay_t<decltype (value)>;
        if constexpr (std::is_same_v<T, InitializedData>) {
          // Pointer payloads are integer bits; references remain typed relocations.
          out_ << type_name (value.type.kind () == Type::Kind::Pointer ? Type (Type::Kind::U64)
                                                                       : value.type);
          for (std::size_t n = 0; n < value.values.size (); ++n) {
            out_ << (n ? ", " : " ");
            auto written = operand (value.values[n]);
            if (!written) return written;
          }
        } else if constexpr (std::is_same_v<T, ZeroData>)
          out_ << "bss " << value.size;
        else if constexpr (std::is_same_v<T, SymbolReference>)
          out_ << "ref " << value.symbol->name () << ", " << value.displacement;
        else if constexpr (std::is_same_v<T, LabelReference>) {
          out_ << "lref " << labels_.at (value.label);
          if (value.base) out_ << ", " << labels_.at (value.base);
          out_ << ", " << value.displacement;
        } else
          out_ << "expr " << value.function->symbol ().name ();
        out_ << '\n';
        return {};
      },
      data.initializer ());
  }
  Result<void> write_module (const Module &module) {
    module_ = &module;
    next_name_ = 0;
    next_label_ = 0;
    names_.clear ();
    registers_.clear ();
    labels_.clear ();
    call_signatures_.clear ();
    address_temporaries_.clear ();
    generated_signatures_.clear ();
    signature_order_.clear ();
    if (!identifier (module.name ()))
      return invalid ("module name cannot be represented as a MIR identifier");
    for (auto &symbol : module.symbols ())
      if (!symbol.name ().empty ()) {
        if (!identifier (symbol.name ()))
          return invalid ("symbol name cannot be represented as a MIR identifier");
        names_.emplace (symbol.name ());
      }
    std::unordered_set<const Function *> expression_functions;
    for (auto &data : module.data ())
      if (auto expression = std::get_if<ExpressionData> (&data.initializer ()))
        expression_functions.insert (expression->function);
    std::vector<const Function *> function_order;
    for (auto &item : module.items ())
      if (auto function = std::get_if<const Function *> (&item);
          function && expression_functions.contains (*function))
        function_order.push_back (*function);
    for (auto &item : module.items ())
      if (auto function = std::get_if<const Function *> (&item);
          function && !expression_functions.contains (*function))
        function_order.push_back (*function);
    // Register names cannot shadow symbol operands in the portable text grammar.
    for (auto &function : module.functions ()) {
      std::unordered_set<std::string> local_names;
      std::size_t next_register = 0;
      for (auto &reg : function.registers ()) {
        auto name = std::string (reg.name ());
        if (!identifier (name) || module.find_symbol (name) || local_names.contains (name)) {
          do {
            name = ".r" + std::to_string (next_register++);
          } while (module.find_symbol (name) || local_names.contains (name));
        }
        local_names.insert (name);
        names_.insert (name);
        registers_.emplace (&reg, std::move (name));
      }
    }
    for (auto function : function_order)
      for (auto &block : function->blocks ()) {
        auto label_name = [&] () {
          for (;;) {
            auto name = ".L" + std::to_string (next_label_++);
            if (!names_.contains (name)) return name;
          }
        };
        for (auto label : block.labels ()) labels_.emplace (label, label_name ());
      }
    std::unordered_set<const Symbol *> visible_prototypes;
    for (auto &item : module.items ()) {
      if (auto symbol = std::get_if<const Symbol *> (&item);
          symbol && (*symbol)->kind () == Symbol::Kind::Prototype)
        visible_prototypes.insert (*symbol);
      if (auto function = std::get_if<const Function *> (&item))
        for (auto &block : (*function)->blocks ())
          for (auto &instruction : block.instructions ()) {
            for (std::size_t n = 0; n < instruction.operands ().size (); ++n) {
              auto &value = instruction.operands ()[n];
              if ((value.get<SymbolReference> () && value.get<SymbolReference> ()->displacement)
                  || (value.get<Memory> () && value.get<Memory> ()->type.is_block ()
                      && value.get<Memory> ()->displacement))
                address_temporaries_[&instruction].emplace_back (n, fresh (".addr"));
            }
            if (instruction_descriptor (instruction.opcode ())->count != OperandCount::Call)
              continue;
            auto symbol = instruction.operands ()[0].get<SymbolReference> ()->symbol;
            if (visible_prototypes.contains (symbol))
              call_signatures_.emplace (&instruction, std::string (symbol->name ()));
            else {
              auto signature = symbol->signature ();
              auto found = generated_signatures_.find (signature);
              if (found == generated_signatures_.end ()) {
                found = generated_signatures_.emplace (signature, fresh (".sig")).first;
                signature_order_.push_back (signature);
              }
              call_signatures_.emplace (&instruction, found->second);
            }
          }
    }
    out_ << module.name () << ": module\n";
    for (auto &symbol : module.symbols ()) {
      if (symbol.kind () == Symbol::Kind::Import)
        out_ << "\timport " << symbol.name () << '\n';
      else if (symbol.kind () == Symbol::Kind::Function
               || (symbol.kind () == Symbol::Kind::Data && !symbol.name ().empty ()))
        out_ << "\tforward " << symbol.name () << '\n';
    }
    for (auto signature : signature_order_) {
      out_ << generated_signatures_.at (signature) << ": proto ";
      write_signature (*signature);
    }
    // MIR's existing reader requires expression functions to be defined already.
    for (auto function : function_order)
      if (expression_functions.contains (function)) {
        auto written = write_function (*function);
        if (!written) return written;
      }
    bool last_data = false, skipped_separator = false;
    for (auto &item : module.items ()) {
      if (std::holds_alternative<ForwardDeclaration> (item)
          || (std::holds_alternative<const Symbol *> (item)
              && std::get<const Symbol *> (item)->kind () == Symbol::Kind::Import)
          || (std::holds_alternative<const Function *> (item)
              && expression_functions.contains (std::get<const Function *> (item)))) {
        skipped_separator = true;
        continue;
      }
      auto data = std::get_if<const GlobalData *> (&item);
      if (data && (*data)->symbol ().name ().empty () && last_data && skipped_separator) {
        // Hoisted declarations must not merge formerly separate anonymous data runs.
        out_ << fresh (".boundary") << ": proto \n";
      }
      skipped_separator = false;
      last_data = data != nullptr;
      auto written = std::visit (
        [&] (auto value) -> Result<void> {
          using T = decltype (value);
          if constexpr (std::is_same_v<T, const Function *>)
            return write_function (*value);
          else if constexpr (std::is_same_v<T, const GlobalData *>)
            return write_data (*value);
          else if constexpr (std::is_same_v<T, const Symbol *>) {
            out_ << value->name () << ": proto ";
            write_signature (*value->signature ());
          } else if constexpr (std::is_same_v<T, ExportDeclaration>)
            out_ << "\texport " << value.symbol->name () << '\n';
          else
            return invalid ("unmaterialized module declaration");
          return {};
        },
        item);
      if (!written) return written;
    }
    out_ << "\tendmodule\n";
    return {};
  }
};
}  // namespace
Result<std::unique_ptr<ir::Context>> parse_text (std::string_view input, ParseOptions options) {
  auto tokens = Lexer (input, options).scan ();
  if (!tokens) return tokens.error ();
  return Parser (*tokens, options).run ();
}
Result<std::string> write_text (const ir::Context &context) {
  for (auto &module : context.modules ())
    for (auto &function : module.functions ()) {
      auto phase = function.require_register_ir ();
      if (!phase) return phase.error ();
    }
  if (ir::needs_materialization (context)) {
    auto lowered = ir::materialize (context);
    if (!lowered) return lowered.error ();
    return Formatter ().run (**lowered);
  }
  return Formatter ().run (context);
}
}  // namespace mirnext::serialization
