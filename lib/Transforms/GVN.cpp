/* Licensed under the MIT License. Derived from MIR gvn; canonical IR is the only body. */
#include "mirnext/Transforms/GVN.hpp"
#include "mirnext/Transforms/IntegerFolding.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include <bit>
#include <unordered_set>
namespace mirnext::transforms {
namespace {
using namespace ir;
using namespace analysis;
Error invalid(const Function &function, std::string reason) {
  return {ErrorCode::InvalidArgument, std::string(function.symbol().name()) + ": gvn: " + std::move(reason)};
}
bool move(Opcode op) { return op == Opcode::Mov || op == Opcode::FMov || op == Opcode::DMov || op == Opcode::LDMov; }
bool address(Opcode op) { return op == Opcode::Addr || op == Opcode::Addr8 || op == Opcode::Addr16 || op == Opcode::Addr32; }
bool overflow(Opcode op) {
  return op == Opcode::Addo || op == Opcode::Addos || op == Opcode::Subo || op == Opcode::Subos
    || op == Opcode::Mulo || op == Opcode::Mulos || op == Opcode::UMulo || op == Opcode::UMulos;
}
bool call(const Instruction &i) { return instruction_descriptor(i.opcode())->count == OperandCount::Call; }
bool fixed(const Instruction &i) {
  switch (i.opcode()) {
  case Opcode::Ret: case Opcode::JRet: case Opcode::Switch: case Opcode::Alloca: case Opcode::BStart: case Opcode::BEnd:
  case Opcode::VaStart: case Opcode::VaArg: case Opcode::VaEnd: return true;
  default: return call(i);
  }
}
bool store(const Instruction &i) { return move(i.opcode()) && i.operands()[0].get<Memory>(); }
bool load(const Instruction &i) { return move(i.opcode()) && i.operands()[1].get<Memory>(); }
bool copy(const Instruction &i) { return move(i.opcode()) && i.operands()[0].get<const Register *>() && i.operands()[1].get<const Register *>(); }
bool hard(const Operand &o) { auto r = o.get<const Register *>(); return r && !(*r)->hardware_binding().empty(); }
Opcode move_code(Type type) {
  return type.kind() == Type::Kind::F32 ? Opcode::FMov : type.kind() == Type::Kind::F64 ? Opcode::DMov
    : type.kind() == Type::Kind::LongDouble ? Opcode::LDMov : Opcode::Mov;
}
Type::Kind canonical(Type type) {
  return type.kind() == Type::Kind::U64 || type.kind() == Type::Kind::Pointer ? Type::Kind::I64 : type.kind();
}
bool alias(const Memory &a, const Memory &b) {
  return (a.alias.empty() || b.alias.empty() || a.alias == b.alias)
    && (a.nonalias.empty() || b.nonalias.empty() || a.nonalias != b.nonalias);
}
struct ValueKey { bool constant; std::uint64_t value; bool operator==(const ValueKey &) const = default; };
struct MultiResult { const Instruction *identity; bool operator==(const MultiResult &) const = default; };
using Atom = std::variant<ValueKey, Operand, Type::Kind, MultiResult>;
struct ExpressionKey { Opcode opcode; std::vector<Atom> inputs; bool operator==(const ExpressionKey &) const = default; };
void mix(std::size_t &hash, std::size_t value) { hash ^= value + 0x9e3779b9 + (hash << 6) + (hash >> 2); }
struct ExpressionHash {
  std::size_t operator()(const ExpressionKey &key) const {
    std::size_t hash = static_cast<unsigned>(key.opcode);
    for (auto &atom : key.inputs) {
      mix(hash, atom.index());
      std::visit([&](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, ValueKey>) { mix(hash, value.constant); mix(hash, value.value); }
        else if constexpr (std::is_same_v<T, Type::Kind>) mix(hash, static_cast<unsigned>(value));
        else if constexpr (std::is_same_v<T, MultiResult>) mix(hash, std::hash<const Instruction *>{}(value.identity));
        else std::visit([&](const auto &operand) {
          using O = std::decay_t<decltype(operand)>;
          mix(hash, value.storage().index());
          if constexpr (std::is_same_v<O, SymbolReference>) { mix(hash, std::hash<const Symbol *>{}(operand.symbol)); mix(hash, operand.displacement); }
          else if constexpr (!std::is_same_v<O, Memory>) mix(hash, std::hash<O>{}(operand));
        }, value.storage());
      }, atom);
    }
    return hash;
  }
};
struct MemoryKey {
  ValueKey address; Type::Kind type; std::string alias, nonalias;
  bool operator==(const MemoryKey &) const = default;
};
struct MemoryHash {
  std::size_t operator()(const MemoryKey &key) const {
    std::size_t hash = key.address.value; mix(hash, key.address.constant); mix(hash, static_cast<unsigned>(key.type));
    mix(hash, std::hash<std::string>{}(key.alias)); mix(hash, std::hash<std::string>{}(key.nonalias)); return hash;
  }
};
struct State {
  struct Expression { const Instruction *instruction; SSAValue *temporary = nullptr; };
  struct MemoryExpression { const Instruction *instruction; Memory memory; bool store; SSAValue *temporary = nullptr; };
  Function &function; FunctionAnalysisManager &analyses; SSAForm &form; SSARewriter rewriter;
  FunctionGraph graph; DominatorTree dominators; GVNResult result;
  std::unordered_map<const Instruction *, GVNResult::Number> effects;
  std::unordered_map<ExpressionKey, Expression, ExpressionHash> expressions;
  std::unordered_map<MemoryKey, std::vector<std::size_t>, MemoryHash> history;
  std::vector<MemoryExpression> memory;
  std::unordered_map<const Instruction *, std::size_t> memory_indices;
  std::vector<SparseBitSet> in, out, generated, killed;
  std::vector<bool> calls, stale;
  SparseBitSet removed;
  std::uint64_t next_number = 1;
  std::size_t changes = 0;
  State(Function &f, FunctionAnalysisManager &a, SSAForm &s, FunctionGraph g, DominatorTree d)
    : function(f), analyses(a), form(s), rewriter(s), graph(std::move(g)), dominators(std::move(d)),
      in(graph.blocks.size()), out(graph.blocks.size()), generated(graph.blocks.size()), killed(graph.blocks.size()), calls(graph.blocks.size()), stale(graph.blocks.size()) {}
  SSAValue *output(const Instruction &i) { auto values = form.definitions(i); return values.empty() ? nullptr : values[0]; }
  SSAValue *input(const Instruction &i, std::size_t p) {
    for (auto use : form.inputs(i)) if (auto site = std::get_if<SSAInstructionUse>(&use->site()); site && site->operand == p)
      return const_cast<SSAValue *>(&use->value());
    return nullptr;
  }
  GVNResult::Number &number(const SSAValue &v) { return result.numbers.at(&v); }
  GVNResult::Number &number(const Instruction &i) { if (auto v = output(i)) return number(*v); return effects.at(&i); }
  ValueKey key(const SSAValue &v) { auto n = number(v); return {n.constant, n.value}; }
  std::optional<std::uint64_t> constant(const Instruction &i, std::size_t p) {
    auto v = input(i, p); if (!v || !number(*v).constant) return {}; return number(*v).value;
  }
  const Memory &mem(const Instruction &i) { return *i.operands()[store(i) ? 0 : 1].get<Memory>(); }
  ExpressionKey key(const Instruction &i) {
    ExpressionKey key{i.opcode(), {}};
    for (std::size_t p = 0; p < i.operands().size(); ++p) {
      auto &operand = i.operands()[p]; if (ssa_output_operand(function, i, p) && !operand.get<Memory>()) continue;
      if (operand.get<const Register *>() || operand.get<Memory>()) {
        if (auto value = input(i, p)) {
          key.inputs.emplace_back(this->key(*value));
          if (value->instruction() && form.definitions(*value->instruction()).size() > 1) key.inputs.emplace_back(MultiResult{&i});
        }
        if (auto memory = operand.get<Memory>()) key.inputs.emplace_back(canonical(memory->type));
      } else key.inputs.emplace_back(operand);
    }
    return key;
  }
  MemoryKey memory_key(const Instruction &i) { auto &m = mem(i); return {key(*input(i, store(i) ? 0 : 1)), canonical(m.type), m.alias, m.nonalias}; }
  Instruction *next(const Instruction &i) { return IRRewriter(function).next_instruction(i); }
  Result<Instruction *> insert(BasicBlock &block, const Instruction *before, Opcode opcode,
                               std::initializer_list<Operand> operands, std::initializer_list<SSAValue *> inputs, const SSAPhi *after_phi = nullptr) {
    auto added = rewriter.insert(block, before, opcode, {operands.begin(), operands.size()}, {inputs.begin(), inputs.size()}, after_phi);
    if (!added) return added.error();
    for (auto value : form.definitions(**added)) result.numbers.emplace(value, GVNResult::Number{false, next_number++, opcode == Opcode::Alloca ? 3u : 0u});
    if (form.definitions(**added).empty()) effects.emplace(*added, GVNResult::Number{false, next_number++});
    ++changes; return *added;
  }
  Result<Instruction *> insert_after(const Instruction &i, Opcode opcode, std::initializer_list<Operand> operands, std::initializer_list<SSAValue *> inputs) {
    return insert(const_cast<BasicBlock &>(i.block()), next(i), opcode, operands, inputs);
  }
  Result<void> bind(SSAValue &from, SSAValue &to) { return rewriter.replace_all_uses(from, to); }
  void update(SparseBitSet &available, const Instruction &i) {
    if (store(i)) available.erase_if([&](auto id) { return alias(memory[id].memory, mem(i)); });
    available.insert(memory_indices.at(&i));
  }
  bool confluence(std::size_t b) {
    auto previous = in[b]; auto &incoming = graph.incoming[b];
    if (incoming.empty()) in[b].clear();
    else {
      in[b] = out[graph.edges[incoming[0]].source];
      for (std::size_t p = 1; p < incoming.size(); ++p) in[b].intersect_with(out[graph.edges[incoming[p]].source]);
    }
    return previous != in[b];
  }
  bool transfer(std::size_t b) {
    auto previous = out[b];
    if (calls[b]) out[b] = generated[b];
    else { out[b] = in[b]; out[b].subtract(killed[b]); out[b].union_with(generated[b]); }
    return previous != out[b];
  }
  void prepare() {
    for (auto &v : form.values()) if (v.alive()) result.numbers.emplace(&v, GVNResult::Number{false, next_number++, v.instruction() && v.instruction()->opcode() == Opcode::Alloca ? 3u : 0u});
    for (auto &block : function.blocks()) {
      auto b = graph.indices.at(&block);
      for (auto &i : block.instructions()) {
        if (!output(i)) effects.emplace(&i, GVNResult::Number{false, next_number++});
        if (call(i)) { generated[b].clear(); calls[b] = true; }
        if (!store(i) && !load(i)) continue;
        memory_indices.emplace(&i, memory.size()); memory.push_back({&i, mem(i), store(i)}); update(generated[b], i);
      }
    }
    // Alias classes do not change during GVN. Compute each block's kills once;
    // fixed-point iterations then operate on words instead of memory pairs.
    for (std::size_t b = 0; b < graph.blocks.size(); ++b) if (!calls[b]) generated[b].for_each([&](auto store) {
      if (!memory[store].store) return;
      for (std::size_t id = 0; id < memory.size(); ++id) if (alias(memory[id].memory, memory[store].memory)) killed[b].insert(id);
    });
    SparseBitSet all; for (std::size_t n = 0; n < memory.size(); ++n) all.insert(n);
    for (auto &bits : out) bits = all;
    bool changed;
    do { changed = false; for (auto b : graph.order) { confluence(b); changed |= transfer(b); } } while (changed);
  }
  Result<void> erase_edge(std::size_t id) {
    auto e = graph.edges[id]; auto &incoming = graph.incoming[e.destination];
    auto found = std::find(incoming.begin(), incoming.end(), id); auto slot = static_cast<std::size_t>(found - incoming.begin());
    if (auto block = graph.blocks[e.destination].ir) for (auto phi : form.phis(*block)) {
      auto checked = rewriter.erase_phi_input(*phi, slot); if (!checked) return checked;
    }
    incoming.erase(found); std::erase(graph.outgoing[e.source], id); stale[e.destination] = true; return {};
  }
  bool cycle(std::size_t start, std::size_t exclude) {
    std::vector<bool> seen(graph.blocks.size()); std::vector<std::size_t> work{start};
    while (!work.empty()) { auto b = work.back(); work.pop_back(); if (b == exclude || seen[b]) continue; seen[b] = true;
      for (auto id : graph.outgoing[b]) { auto target = graph.edges[id].destination; if (target == start) return true; work.push_back(target); }
    }
    return false;
  }
  void location(const Instruction &i, unsigned flags) {
    auto &m = mem(i); GVNResult::MemoryLocation attr{m.type, m.alias, m.nonalias, flags};
    if (flags & 2) {
      auto value = input(i, store(i) ? 0 : 1); std::unordered_set<const SSAValue *> seen;
      while (value && seen.insert(value).second) {
        auto def = value->instruction();
        if (def && copy(*def)) { value = input(*def, 1); continue; }
        if (def && (def->opcode() == Opcode::Add || def->opcode() == Opcode::Adds || def->opcode() == Opcode::Sub || def->opcode() == Opcode::Subs) && constant(*def, 2)) {
          auto n = *constant(*def, 2); attr.displacement += def->opcode() == Opcode::Sub || def->opcode() == Opcode::Subs ? 0 - n : n;
          value = input(*def, 1); continue;
        }
        attr.base = value; break;
      }
    }
    if (result.memory.empty()) result.memory.push_back(attr);
    result.locations[&i] = result.memory.size(); result.memory.push_back(std::move(attr));
  }
  void reuse_location(const Instruction &i, const Instruction &other, unsigned flags) {
    auto slot = result.locations.at(&other); result.locations[&i] = slot; auto &attr = result.memory[slot];
    attr.alloca_flags = ((attr.alloca_flags | flags) & 1) | (attr.alloca_flags & flags & 2);
  }
  Result<void> phi(SSAPhi &phi) {
    auto &value = const_cast<SSAValue &>(phi.output()); auto &n = number(value); bool same = true; SSAValue *first = nullptr;
    for (auto use : phi.inputs()) {
      auto v = const_cast<SSAValue *>(&use->value()); auto info = number(*v);
      if (!first) { first = v; n.alloca_flags = info.alloca_flags; }
      else { same &= key(*first) == key(*v); n.alloca_flags = ((n.alloca_flags | info.alloca_flags) & 1) | (n.alloca_flags & info.alloca_flags & 2); }
    }
    if (same && first && number(*first).constant) {
      auto added = insert(const_cast<BasicBlock &>(phi.block()), phi.before(), Opcode::Mov,
        {Operand(value.reg()), Operand(std::bit_cast<std::int64_t>(number(*first).value))}, {}, form.phis(phi.block()).back());
      if (!added) return added.error(); number(**added) = {true, number(*first).value}; return bind(value, *output(**added));
    }
    if (phi.inputs().size() == 1 && value.reg().hardware_binding().empty() && first->reg().hardware_binding().empty()) {
      // Removing unreachable incoming edges can leave a closed PHI cycle.
      // Its own input is not a replacement definition.
      if (first == &value) {
        auto checked = rewriter.erase_phi_input(phi, 0); if (!checked) return checked;
        if (value.uses().empty()) checked = rewriter.erase_phi(phi);
        else {
          auto initial = rewriter.initial(value.reg(), SSAValue::Kind::Undefined); if (!initial) return initial.error();
          result.numbers.emplace(*initial, GVNResult::Number{false, next_number++});
          checked = rewriter.erase_phi(phi, **initial);
        }
        if (!checked) return checked; ++changes; return {};
      }
      auto checked = rewriter.erase_phi(phi, *first); if (!checked) return checked; ++changes; return {};
    }
    if (same && first) n.value = number(*first).value; n.constant = false; return {};
  }
  Result<void> canonicalize(Instruction *&instruction, Instruction *&following, bool &revisit) {
    auto op = instruction->opcode();
    if (op != Opcode::Add && op != Opcode::Adds && op != Opcode::Sub && op != Opcode::Subs) return {};
    if ((op == Opcode::Add || op == Opcode::Adds) && constant(*instruction, 1) && !constant(*instruction, 2)) {
      Operand operands[]{instruction->operands()[0], instruction->operands()[2], instruction->operands()[1]};
      SSAValue *inputs[]{input(*instruction, 2), input(*instruction, 1)};
      auto checked = rewriter.replace(*instruction, op, operands, inputs); if (!checked) return checked; ++changes;
    }
    auto c2 = constant(*instruction, 2); auto base = input(*instruction, 1); std::unordered_set<const SSAValue *> seen;
    while (base && base->instruction() && base->instruction()->opcode() == Opcode::Mov && copy(*base->instruction()) && !hard(base->instruction()->operands()[1]) && seen.insert(base).second)
      base = input(*base->instruction(), 1);
    auto definition = base ? base->instruction() : nullptr;
    auto c1 = definition ? constant(*definition, 2) : std::nullopt;
    if (!c1 || !c2 || !definition || hard(instruction->operands()[1]) || hard(definition->operands()[1])
        || (definition->opcode() != Opcode::Add && definition->opcode() != Opcode::Adds && definition->opcode() != Opcode::Sub && definition->opcode() != Opcode::Subs)) return {};
    auto combined = (op == Opcode::Sub || op == Opcode::Subs ? 0 - *c2 : *c2)
      + (definition->opcode() == Opcode::Sub || definition->opcode() == Opcode::Subs ? 0 - *c1 : *c1);
    bool narrow = op == Opcode::Adds || op == Opcode::Subs;
    if (narrow) combined = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(combined))));
    auto temp = rewriter.fresh_register(Type::i64()); if (!temp) return temp.error();
    auto copied = insert(const_cast<BasicBlock &>(definition->block()), definition, Opcode::Mov, {Operand(**temp), definition->operands()[1]}, {input(*definition, 1)});
    if (!copied) return copied.error(); number(**copied) = number(*input(*definition, 1));
    if (combined) {
      auto literal = rewriter.fresh_register(Type::i64()); if (!literal) return literal.error();
      auto constant_insn = insert_after(*instruction, Opcode::Mov, {Operand(**literal), Operand(std::bit_cast<std::int64_t>(combined))}, {});
      if (!constant_insn) return constant_insn.error(); number(**constant_insn) = {true, combined};
      auto added = insert_after(**constant_insn, narrow ? Opcode::Adds : Opcode::Add,
        {instruction->operands()[0], Operand(**temp), Operand(**literal)}, {output(**copied), output(**constant_insn)});
      if (!added) return added.error(); auto checked = bind(*output(*instruction), *output(**added)); if (!checked) return checked;
      following = *constant_insn; revisit = true; return {};
    }
    auto added = insert_after(*instruction, Opcode::Mov, {instruction->operands()[0], Operand(**temp)}, {output(**copied)});
    if (!added) return added.error(); auto checked = bind(*output(*instruction), *output(**added)); if (!checked) return checked;
    instruction = *added; following = next(*instruction); return {};
  }
  Result<bool> process_memory(Instruction &i, SparseBitSet &available, std::size_t b) {
    auto address = input(i, store(i) ? 0 : 1); auto &list = history[memory_key(i)];
    auto previous = [&](bool stores_only) -> std::optional<std::size_t> {
      for (auto it = list.rbegin(); it != list.rend(); ++it) if (available.contains(*it) && (!stores_only || store(*memory[*it].instruction))) return *it;
      return {};
    };
    if (store(i)) {
      auto source = input(i, 1); if (number(*source).alloca_flags) result.full_escape = true;
      auto st = previous(true); bool fresh = st && cycle(graph.indices.at(&memory[*st].instruction->block()), b); auto prev = previous(false);
      if (fresh || !prev) location(i, number(*address).alloca_flags);
      else {
        auto old = memory[*prev].instruction; reuse_location(i, *old, number(*address).alloca_flags);
        if (load(*old) && key(*source) == key(*output(*old))) {
          available.erase(memory_indices.at(&i)); removed.insert(memory_indices.at(&i));
          result.locations.erase(&i);
          auto checked = rewriter.erase_instruction(i); if (!checked) return checked.error(); ++changes; return true;
        }
      }
      list.push_back(memory_indices.at(&i)); update(available, i); number(i) = number(*source); return true;
    }
    if (output(i)->uses().empty()) return true;
    auto prev = previous(false);
    if (!prev) { location(i, number(*address).alloca_flags); list.push_back(memory_indices.at(&i)); }
    else {
      auto &old = memory[*prev]; reuse_location(i, *old.instruction, number(*address).alloca_flags);
      if (load(*old.instruction) && output(*old.instruction)->uses().empty()) list.push_back(memory_indices.at(&i));
      else {
        number(i) = number(*old.instruction);
        if (!old.temporary) {
          auto source = store(*old.instruction) ? input(*old.instruction, 1) : output(*old.instruction);
          auto temp = rewriter.fresh_register(output(i)->reg().type()); if (!temp) return temp.error();
          auto added = insert_after(*old.instruction, i.opcode(), {Operand(**temp), Operand(source->reg())}, {source});
          if (!added) return added.error(); old.temporary = output(**added); number(*old.temporary) = number(*old.instruction);
        }
        available.erase(memory_indices.at(&i)); removed.insert(memory_indices.at(&i));
        Operand operands[]{i.operands()[0], Operand(old.temporary->reg())}; SSAValue *inputs[]{old.temporary};
        auto checked = rewriter.replace(i, i.opcode(), operands, inputs); if (!checked) return checked.error(); ++changes; return true;
      }
    }
    update(available, i); return false;
  }
  Result<void> process(Instruction *i, Instruction *&following, SparseBitSet &available, std::size_t b) {
    if (i->opcode() == Opcode::Mov) {
      auto signed_value = i->operands()[1].get<std::int64_t>(); auto unsigned_value = i->operands()[1].get<std::uint64_t>();
      if (signed_value || unsigned_value) { number(*i) = {true, signed_value ? static_cast<std::uint64_t>(*signed_value) : *unsigned_value}; return {}; }
    }
    if (call(*i)) available.clear(); if (fixed(*i)) return {};
    auto value = fold_integer(i->opcode(), constant(*i, 1), constant(*i, 2));
    if (overflow(i->opcode()) && value) for (auto probe = following; probe; probe = next(*probe)) {
      auto op = probe->opcode();
      if (op == Opcode::Bo || op == Opcode::Bno || op == Opcode::UBo || op == Opcode::UBno) { value.reset(); break; }
      if (op != Opcode::Mov && op != Opcode::Ext32 && op != Opcode::UExt32) break;
    }
    bool arithmetic = i->opcode() == Opcode::Add || i->opcode() == Opcode::Adds || i->opcode() == Opcode::Addo || i->opcode() == Opcode::Addos
      || i->opcode() == Opcode::Sub || i->opcode() == Opcode::Subs || i->opcode() == Opcode::Subo || i->opcode() == Opcode::Subos;
    bool bitwise = i->opcode() == Opcode::And || i->opcode() == Opcode::Ands || i->opcode() == Opcode::Or || i->opcode() == Opcode::Ors || i->opcode() == Opcode::Xor || i->opcode() == Opcode::Xors;
    if (arithmetic || bitwise) for (auto p : {1u, 2u}) if (auto input_value = input(*i, p)) number(*i).alloca_flags |= number(*input_value).alloca_flags & (arithmetic ? 3u : 1u);
    if (value) {
      if (instruction_descriptor(i->opcode())->flow == ControlFlow::Branch) {
        if (graph.outgoing[b].size() != 2) return invalid(function, "conditional branch must have two edges");
        auto edge = graph.outgoing[b][*value ? 0 : 1]; auto checked = erase_edge(edge); if (!checked) return checked;
        if (!*value) checked = rewriter.erase_instruction(*i);
        else { Operand operands[]{i->operands()[0]}; checked = rewriter.replace(*i, Opcode::Jmp, operands, {}); }
        if (!checked) return checked; ++changes;
      } else if (auto original = output(*i)) {
        auto added = insert_after(*i, Opcode::Mov, {i->operands()[0], Operand(std::bit_cast<std::int64_t>(*value))}, {});
        if (!added) return added.error(); number(**added) = {true, *value}; return bind(*original, *output(**added));
      }
      return {};
    }
    bool revisit = false;
    auto checked = canonicalize(i, following, revisit); if (!checked) return checked;
    if (revisit) return {};
    if (store(*i) || load(*i)) { auto done = process_memory(*i, available, b); if (!done) return done.error(); if (*done) return {}; }
    else if (copy(*i) && !hard(i->operands()[0]) && !hard(i->operands()[1])) {
      auto source = input(*i, 1);
      if ((source->instruction() || source->phi()) && source->uses().size() == 1) {
        checked = bind(*output(*i), *source); if (!checked) return checked; checked = rewriter.erase_instruction(*i); if (!checked) return checked; ++changes; return {};
      }
    }
    if (instruction_descriptor(i->opcode())->flow != ControlFlow::Next) return {};
    if (copy(*i)) { number(*i) = number(*input(*i, 1)); return {}; }
    if (overflow(i->opcode()) || !output(*i)) return {};
    auto [found, fresh] = expressions.try_emplace(key(*i), Expression{i});
    if (!fresh && load(*i) && !available.contains(memory_indices.at(found->second.instruction))) found->second = {i};
    auto &expression = found->second; auto original = expression.instruction;
    number(*i) = {false, number(*original).value, number(*original).alloca_flags};
    if (original == i || address(i->opcode())) return {};
    if (move(i->opcode()) && !i->operands()[1].get<Memory>() && !i->operands()[1].get<const Register *>() && !i->operands()[1].get<SymbolReference>()) return {};
    if ((i->opcode() == Opcode::Add || i->opcode() == Opcode::Sub) && output(*i)->uses().size() == 1) {
      auto site = std::get_if<SSAInstructionUse>(&output(*i)->uses().front()->site());
      if (site && site->instruction->operands()[site->operand].get<Memory>()) {
        auto literal = [&](std::size_t p) -> std::optional<std::uint64_t> {
          auto def = input(*i, p); if (!def || !def->instruction() || !move(def->instruction()->opcode())) return {};
          auto &o = def->instruction()->operands()[1];
          if (auto s = o.get<std::int64_t>()) return static_cast<std::uint64_t>(*s);
          if (auto u = o.get<std::uint64_t>()) return *u;
          return {};
        };
        auto displacement = literal(2); if (!displacement && i->opcode() == Opcode::Add) displacement = literal(1);
        if (displacement) { auto signed_disp = std::bit_cast<std::int64_t>(i->opcode() == Opcode::Sub ? 0 - *displacement : *displacement); if (signed_disp >= -128 && signed_disp <= 127) return {}; }
      }
    }
    auto original_block = graph.indices.at(&original->block()); if (b != original_block && !dominators.dominates(original_block, b)) return {};
    auto op = move_code(output(*i)->reg().type());
    if (!expression.temporary) {
      auto temp = rewriter.fresh_register(output(*i)->reg().type()); if (!temp) return temp.error();
      // Redirect before adding the new input so it cannot redirect itself.
      auto old = output(*original); std::vector<SSAUse *> users(old->uses().begin(), old->uses().end());
      auto added = insert_after(*original, op, {Operand(**temp), Operand(old->reg())}, {old}); if (!added) return added.error();
      expression.temporary = output(**added); number(**added) = number(*original);
      checked = rewriter.replace_uses(users, *expression.temporary); if (!checked) return checked;
      expressions.try_emplace(key(**added), Expression{*added});
    }
    auto added = insert_after(*i, op, {i->operands()[0], Operand(expression.temporary->reg())}, {expression.temporary});
    if (!added) return added.error(); checked = bind(*output(*i), *output(**added)); if (!checked) return checked;
    number(**added) = number(*original); expressions.try_emplace(key(**added), Expression{*added}); return {};
  }
  Result<void> run() {
    prepare();
    for (auto b : graph.order) {
      if (b > 2 && graph.incoming[b].empty()) {
        std::vector<std::size_t> pending{b};
        while (!pending.empty()) {
          auto dead = pending.back(); pending.pop_back();
          while (!graph.outgoing[dead].empty()) { auto id = graph.outgoing[dead][0], target = graph.edges[id].destination;
            auto checked = erase_edge(id); if (!checked) return checked;
            if (target > 2 && graph.incoming[target].empty()) pending.push_back(target);
          }
        }
        continue;
      }
      if (!graph.incoming[b].empty() && stale[b] && confluence(b)) {
        in[b].subtract(removed); if (transfer(b)) for (auto id : graph.outgoing[b]) stale[graph.edges[id].destination] = true;
      }
      auto available = in[b]; available.subtract(removed);
      if (auto block = graph.blocks[b].ir) {
        // PHIs belong to the block, not a duplicated instruction sequence.
        auto phis = form.phis(*block); std::vector<SSAPhi *> pending(phis.begin(), phis.end());
        auto instruction = block->size() ? const_cast<Instruction *>(&*block->instructions().begin()) : nullptr;
        for (auto p : pending) { auto checked = phi(*p); if (!checked) return checked; }
        for (; instruction;) { auto following = next(*instruction); auto checked = process(instruction, following, available, b); if (!checked) return checked; instruction = following; }
      }
    }
    // Deleted edges are needed only while walking the original traversal.
    // Publish a dense graph, retaining each live edge's insertion order.
    std::vector<std::size_t> remap(graph.edges.size(), SIZE_MAX);
    for (auto &edges : graph.outgoing) for (auto id : edges) remap[id] = id;
    std::size_t kept = 0;
    for (std::size_t id = 0; id < graph.edges.size(); ++id) if (remap[id] != SIZE_MAX) {
      remap[id] = kept; graph.edges[kept] = graph.edges[id]; graph.back_edges[kept++] = graph.back_edges[id];
    }
    graph.edges.resize(kept); graph.back_edges.resize(kept);
    for (auto &edges : graph.incoming) for (auto &id : edges) id = remap[id];
    for (auto &edges : graph.outgoing) for (auto &id : edges) id = remap[id];
    return form.verify();
  }
};
}
Result<GVNResult> GVNAnalysis::run(const ir::Function &function) { return invalid(function, "value numbering results require a completed GVN pass"); }
Result<PassResult> GVNPass::run(ir::Function &function, analysis::FunctionAnalysisManager &analyses) {
  if (&analyses.function() != &function) return invalid(function, "analysis owner mismatch");
  auto checked = function.verify(); if (!checked) return checked.error();
  auto form = function.ssa_form(); if (!form || !form->renamed() || form->conventional()) return invalid(function, "requires renamed nonconventional SSA");
  for (auto &block : function.blocks()) for (auto &i : block.instructions()) {
    unsigned memories = 0;
    for (auto &o : i.operands()) if (auto m = o.get<Memory>()) {
      ++memories;
      bool special = fixed(i) || (i.opcode() == Opcode::VaBlockArg && m->type.kind() == Type::Kind::Undef);
      bool conversion_move = sizeof(long double) == sizeof(double) && (i.opcode() == Opcode::D2LD || i.opcode() == Opcode::LD2D);
      if (!special && (!m->base || m->index || m->displacement || (!load(i) && !store(i) && !conversion_move))) return invalid(function, "simplify must materialize memory operands before GVN");
    }
    if (!fixed(i) && memories > 1 && !(sizeof(long double) == sizeof(double) && (i.opcode() == Opcode::D2LD || i.opcode() == Opcode::LD2D))) return invalid(function, "simplify must split memory moves before GVN");
  }
  auto graph = analyses.get<analysis::FunctionGraphAnalysis>(); if (!graph) return graph.error();
  auto dom = analyses.get<analysis::DominatorAnalysis>(); if (!dom) return dom.error();
  State state(function, analyses, *const_cast<ir::SSAForm *>(form), **graph, **dom);
  checked = state.run(); if (!checked) return checked.error();
  // Persist the C GVN nloc equivalence classes past SSA teardown. Only owning,
  // typed locations cross that boundary; SSA value pointers stay in analysis.
  std::unordered_map<std::size_t,const ir::MemoryLocation *> locations;
  IRRewriter annotations(function);
  for (auto &block : function.blocks()) for (auto &i : block.instructions()) for (std::size_t p = 0; p < i.operands().size(); ++p)
    if (i.operands()[p].get<Memory>()) {
      const ir::MemoryLocation *location = nullptr;
      if (auto found = state.result.locations.find(&i); found != state.result.locations.end()) {
        auto [at,fresh] = locations.try_emplace(found->second); if (fresh) at->second = &annotations.create_memory_location(); location = at->second;
      }
      checked = state.rewriter.set_memory_location(i,p,location); if (!checked) return checked.error();
    }
  checked = analyses.set<GVNAnalysis>(std::move(state.result)); if (!checked) return checked.error();
  checked = analyses.set<analysis::FunctionGraphAnalysis>(std::move(state.graph)); if (!checked) return checked.error();
  auto preserved = analysis::PreservedAnalyses::none(); preserved.preserve<GVNAnalysis>(); preserved.preserve<analysis::FunctionGraphAnalysis>();
  return PassResult{state.changes, preserved};
}
}
