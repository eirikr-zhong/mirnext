/* Licensed under the MIT License. Timing is informational, never a test threshold. */
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/Inline.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include <chrono>
#include <iostream>

int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::analysis;
  std::cout << "sizeof_operand=" << sizeof (Operand) << " sizeof_expression=" << sizeof (Expression)
            << " sizeof_call_argument=" << sizeof (CallArgument) << '\n';
  for (unsigned count : {4096, 16384, 65536}) {
    Context input;
    auto module = input.create_module ("ssa");
    if (!module) return 1;
    auto function = (*module)->create_function ("entry", {{Type::i64 ()}, {Type::i64 ()}});
    if (!function) return 1;
    auto block = (*function)->create_block ();
    auto source = (*function)->create_register (Type::i64 ()),
         value = (*function)->create_register (Type::i64 ());
    if (!block || !source || !value) return 1;
    IRBuilder body (**block);
    if (!body.assign (**source, 1) || !body.assign (**source, 42)) return 1;
    for (unsigned n = 0; n < count; ++n)
      if (!body.emit (Opcode::Add, {Operand (**value), Operand (**source), Operand (**source)}))
        return 1;
    if (!body.ret (Operand (**value))) return 1;
    FunctionAnalysisManager analyses (**function);
    auto graph = analyses.get<FunctionGraphAnalysis> ();
    if (!graph) return 1;
    auto start = std::chrono::steady_clock::now ();
    mirnext::transforms::BuildSSAPass build;
    auto result = build.run (**function, analyses);
    if (!result) return 1;
    auto end = std::chrono::steady_clock::now ();
    std::size_t uses = 0;
    for (auto &use : (*function)->ssa_form ()->uses ()) uses += use.alive ();
    auto ns = std::chrono::duration<double, std::nano> (end - start).count ();
    std::cout << "ssa_adds=" << count << " uses=" << uses << " build_us=" << ns / 1000
              << " ns_per_add=" << ns / count << '\n';
    if (uses != 2 * count + 1) return 1;
    mirnext::transforms::ConventionalSSAPass conventional;
    mirnext::transforms::DestroySSAPass destroy;
    if (!conventional.run (**function, analyses) || !destroy.run (**function, analyses)) return 1;
  }
  Context context;
  auto module = context.create_module ("benchmark");
  if (!module) return 1;
  auto function = (*module)->create_function ("entry", {});
  if (!function) return 1;
  auto reg = (*function)->create_register (Type::i64 (), "value");
  if (!reg) return 1;
  for (unsigned n = 0; n < 256; ++n) {
    auto block = (*function)->create_block ();
    if (!block) return 1;
    IRBuilder builder (**block);
    if (!builder.assign (**reg, Operand (static_cast<std::int64_t> (n)))) return 1;
    if (n == 255 && !builder.ret ()) return 1;
  }
  FunctionAnalysisManager analyses (**function);
  auto warm = analyses.get<ControlFlowAnalysis> ();
  if (!warm) return 1;
  constexpr unsigned iterations = 2000;
  std::uint64_t cached_sum = 0, rebuilt_sum = 0;
  auto start = std::chrono::steady_clock::now ();
  for (unsigned n = 0; n < iterations; ++n) {
    auto graph = analyses.get<ControlFlowAnalysis> ();
    if (!graph) return 1;
    cached_sum += (*graph)->edges.size ();
  }
  auto middle = std::chrono::steady_clock::now ();
  for (unsigned n = 0; n < iterations; ++n) {
    auto graph = ControlFlowAnalysis::run (**function);
    if (!graph) return 1;
    rebuilt_sum += graph->edges.size ();
  }
  auto end = std::chrono::steady_clock::now ();
  if (cached_sum != rebuilt_sum) return 1;
  auto microseconds
    = [] (auto duration) { return std::chrono::duration<double, std::micro> (duration).count (); };
  std::cout << "blocks=256 iterations=" << iterations << " checksum=" << cached_sum
            << " cached_us=" << microseconds (middle - start)
            << " rebuilt_us=" << microseconds (end - middle) << '\n';
  for (unsigned count : {256, 1024, 4096}) {
    Context input;
    auto m = input.create_module ("dominance");
    if (!m) return 1;
    auto f = (*m)->create_function ("entry", {});
    if (!f) return 1;
    for (unsigned n = 0; n < count; ++n) {
      auto block = (*f)->create_block ();
      if (!block || !(*f)->create_label (**block)) return 1;
      if (n + 1 == count && !IRBuilder (**block).ret ()) return 1;
    }
    FunctionAnalysisManager analyses (**f);
    auto graph = analyses.get<FunctionGraphAnalysis> ();
    if (!graph) return 1;
    auto &g = **graph;
    auto start = std::chrono::steady_clock::now ();
    auto dom = analyses.get<DominatorAnalysis> ();
    auto middle = std::chrono::steady_clock::now ();
    if (!dom) return 1;
    auto blocks = g.blocks.size (), words = (blocks + 63) / 64;
    std::vector<std::vector<std::uint64_t>> dense (blocks,
                                                   std::vector<std::uint64_t> (words,
                                                                               ~std::uint64_t{}));
    std::ranges::fill (dense[0], 0);
    std::vector<std::uint64_t> next (words);
    bool changed;
    do {
      changed = false;
      for (auto b : g.order) {
        std::ranges::fill (next, 0);
        if (!g.incoming[b].empty ()) {
          next = dense[g.edges[g.incoming[b][0]].source];
          for (auto id : g.incoming[b])
            for (std::size_t w = 0; w < words; ++w) next[w] &= dense[g.edges[id].source][w];
        }
        next[b / 64] |= std::uint64_t{1} << (b % 64);
        if (next != dense[b]) {
          dense[b] = next;
          changed = true;
        }
      }
    } while (changed);
    auto end = std::chrono::steady_clock::now ();
    std::uint64_t checksum = 0;
    for (std::size_t b = 0; b < blocks; ++b)
      for (std::size_t a = 0; a < blocks; ++a) {
        bool expected = (dense[b][a / 64] >> (a % 64)) & 1;
        if ((*dom)->dominates (a, b) != expected) return 1;
        checksum += expected;
      }
    std::cout << "dominance_blocks=" << blocks << " checksum=" << checksum
              << " tree_us=" << microseconds (middle - start)
              << " dense_fixed_point_us=" << microseconds (end - middle)
              << " tree_fact_bytes=" << blocks * 3 * sizeof (std::size_t)
              << " dense_fact_bytes=" << blocks * words * sizeof (std::uint64_t) << '\n';
  }
  for (unsigned count : {256, 1024, 4096}) {
    Context input;
    auto m = input.create_module ("inline");
    if (!m) return 1;
    auto leaf = (*m)->create_function ("leaf", {{Type::i64 ()}, {}});
    if (!leaf) return 1;
    auto b = (*leaf)->create_block ();
    if (!b) return 1;
    auto value = (*leaf)->create_register (Type::i64 ());
    if (!value) return 1;
    IRBuilder body (**b);
    if (!body.assign (**value, 42) || !body.ret (Operand (**value))) return 1;
    for (unsigned n = 0; n < count; ++n) {
      auto f = (*m)->create_function ("caller" + std::to_string (n), {{Type::i64 ()}, {}});
      if (!f) return 1;
      auto block = (*f)->create_block ();
      if (!block) return 1;
      IRBuilder builder (**block);
      auto call = builder.call (**leaf);
      if (!call) return 1;
      auto result = call->value ();
      if (!result || !builder.ret (Operand (**result))) return 1;
    }
    auto separate = clone (input);
    if (!separate) return 1;
    transforms::FunctionPassManager passes;
    passes.add<transforms::InlinePass> ();
    auto start = std::chrono::steady_clock::now ();
    auto result = passes.run (input);
    auto middle = std::chrono::steady_clock::now ();
    if (!result) return 1;
    for (auto &module : (*separate)->modules ())
      for (auto &function : module.functions ()) {
        FunctionAnalysisManager analyses (function);
        if (!passes.run (function, analyses)) return 1;
      }
    auto end = std::chrono::steady_clock::now ();
    if (dump (input) != dump (**separate)) return 1;
    std::uint64_t checksum = 0;
    for (auto &f : (*m)->functions ())
      for (auto &block : f.blocks ())
        for (auto &i : block.instructions ()) {
          if (instruction_descriptor (i.opcode ())->count == OperandCount::Call) return 1;
          if (i.opcode () == Opcode::Mov && i.operands ()[1].get<std::int64_t> ())
            checksum += *i.operands ()[1].get<std::int64_t> ();
        }
    if (checksum != 42ULL * (count + 1)) return 1;
    std::cout << "inline_callers=" << count << " checksum=" << checksum
              << " scoped_us=" << microseconds (middle - start)
              << " per_function_scope_us=" << microseconds (end - middle) << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Context named;
    auto m = named.create_module ("named");
    if (!m) return 1;
    auto f = (*m)->create_function ("entry", {});
    if (!f) return 1;
    auto start = std::chrono::steady_clock::now ();
    for (unsigned n = 0; n < count; ++n)
      if (!(*f)->create_block ("block" + std::to_string (n))) return 1;
    auto end = std::chrono::steady_clock::now ();
    auto &last = *std::prev ((*f)->blocks ().end ());
    if (!IRBuilder (last).ret () || !named.verify ()) return 1;
    std::uint64_t checksum = 0;
    for (auto &b : (*f)->blocks ()) checksum += b.name ().size ();
    if (std::ranges::distance ((*f)->blocks ()) != count) return 1;
    std::cout << "named_blocks=" << count << " checksum=" << checksum
              << " total_us=" << microseconds (end - start)
              << " ns_per_block=" << microseconds (end - start) * 1000 / count << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Context rewrite_context;
    auto rewrite_module = rewrite_context.create_module ("rewrite");
    if (!rewrite_module) return 1;
    auto rewrite_function = (*rewrite_module)->create_function ("entry", {});
    if (!rewrite_function) return 1;
    auto block = (*rewrite_function)->create_block ();
    if (!block) return 1;
    IRBuilder builder (**block);
    std::vector<Instruction *> instructions;
    instructions.reserve (count);
    for (unsigned n = 0; n < count; ++n) {
      auto value = (*rewrite_function)->create_register (Type::i64 ());
      if (!value) return 1;
      auto instruction = builder.assign (**value, Operand (std::int64_t (0)));
      if (!instruction) return 1;
      instructions.push_back (*instruction);
    }
    if (!builder.ret ()) return 1;
    IRRewriter rewriter (**rewrite_function);
    auto rewrite_start = std::chrono::steady_clock::now ();
    for (auto instruction : instructions)
      if (!rewriter.replace_operand (*instruction, 1, Operand (std::int64_t (42)))) return 1;
    auto rewrite_end = std::chrono::steady_clock::now ();
    if (!rewrite_context.verify ()) return 1;
    std::uint64_t checksum = 0;
    for (auto instruction : instructions)
      checksum += *instruction->operands ()[1].get<std::int64_t> ();
    if (checksum != 42ULL * count) return 1;
    std::cout << "rewrites=" << count << " checksum=" << checksum
              << " total_us=" << microseconds (rewrite_end - rewrite_start)
              << " ns_per_rewrite=" << microseconds (rewrite_end - rewrite_start) * 1000 / count
              << '\n';
    auto write_start = std::chrono::steady_clock::now ();
    auto text = mirnext::serialization::write_text (rewrite_context);
    if (!text) return 1;
    auto parse_start = std::chrono::steady_clock::now ();
    auto parsed = mirnext::serialization::parse_text (*text);
    if (!parsed) return 1;
    auto parse_end = std::chrono::steady_clock::now ();
    auto &parsed_function = *(*parsed)->modules ().begin ()->functions ().begin ();
    if (parsed_function.instruction_count () != count + 1) return 1;
    std::uint64_t parsed_sum = 0;
    for (auto &parsed_block : parsed_function.blocks ())
      for (auto &instruction : parsed_block.instructions ())
        if (instruction.opcode () == Opcode::Mov)
          parsed_sum += *instruction.operands ()[1].get<std::int64_t> ();
    if (parsed_sum != checksum) return 1;
    std::cout << "text_instructions=" << count + 1 << " bytes=" << text->size ()
              << " checksum=" << parsed_sum
              << " write_us=" << microseconds (parse_start - write_start)
              << " parse_us=" << microseconds (parse_end - parse_start) << '\n';
    auto binary_start = std::chrono::steady_clock::now ();
    auto binary = mirnext::serialization::write_binary (rewrite_context);
    if (!binary) return 1;
    auto binary_read_start = std::chrono::steady_clock::now ();
    auto decoded = mirnext::serialization::read_binary (*binary);
    if (!decoded) return 1;
    auto binary_end = std::chrono::steady_clock::now ();
    auto &decoded_function = *(*decoded)->modules ().begin ()->functions ().begin ();
    if (decoded_function.instruction_count () != count + 1) return 1;
    std::uint64_t binary_sum = 0;
    for (auto &decoded_block : decoded_function.blocks ())
      for (auto &instruction : decoded_block.instructions ())
        if (instruction.opcode () == Opcode::Mov)
          binary_sum += *instruction.operands ()[1].get<std::int64_t> ();
    if (binary_sum != checksum) return 1;
    std::cout << "binary_instructions=" << count + 1 << " compressed_bytes=" << binary->size ()
              << " checksum=" << binary_sum
              << " write_us=" << microseconds (binary_read_start - binary_start)
              << " read_us=" << microseconds (binary_end - binary_read_start) << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Context edits;
    auto m = edits.create_module ("edits");
    if (!m) return 1;
    auto f = (*m)->create_function ("entry", {});
    if (!f) return 1;
    auto block = (*f)->create_block ();
    if (!block) return 1;
    auto value = (*f)->create_register (Type::i64 ());
    if (!value) return 1;
    IRBuilder builder (**block);
    std::vector<Instruction *> instructions;
    std::vector<const Operand *> operand_addresses;
    instructions.reserve (count);
    operand_addresses.reserve (count);
    for (unsigned n = 0; n < count; ++n) {
      auto instruction = builder.assign (**value, Operand (std::int64_t{42}));
      if (!instruction) return 1;
      instructions.push_back (*instruction);
      operand_addresses.push_back ((*instruction)->operands ().data ());
    }
    if (!builder.ret ()) return 1;
    IRRewriter rewriter (**f);
    auto start = std::chrono::steady_clock::now ();
    for (unsigned n = count; n > 0; --n)
      if (!rewriter.split_before (*instructions[n - 1], "split" + std::to_string (n))) return 1;
    auto middle = std::chrono::steady_clock::now ();
    auto &first = *(*f)->blocks ().begin ();
    for (unsigned n = 0; n < count; ++n)
      if (!rewriter.merge_fallthrough (first)) return 1;
    auto end = std::chrono::steady_clock::now ();
    if (!edits.verify () || std::ranges::distance ((*f)->blocks ()) != 1
        || (*f)->instruction_count () != count + 1)
      return 1;
    std::uint64_t checksum = 0;
    for (unsigned n = 0; n < count; ++n) {
      if (instructions[n]->operands ().data () != operand_addresses[n]
          || &instructions[n]->block () != &first)
        return 1;
      checksum += *instructions[n]->operands ()[1].get<std::int64_t> ();
    }
    if (checksum != 42ULL * count) return 1;
    std::cout << "structural_edits=" << count << " checksum=" << checksum
              << " split_us=" << microseconds (middle - start)
              << " merge_us=" << microseconds (end - middle)
              << " ns_per_split=" << microseconds (middle - start) * 1000 / count
              << " ns_per_merge=" << microseconds (end - middle) * 1000 / count << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Context calls;
    auto m = calls.create_module ("calls");
    if (!m) return 1;
    auto target = (*m)->import ("target", Signature{{Type::i64 ()}, {Type::i64 ()}});
    if (!target) return 1;
    auto f = (*m)->create_function ("entry", {});
    if (!f) return 1;
    auto block = (*f)->create_block ();
    if (!block) return 1;
    IRBuilder builder (**block);
    auto start = std::chrono::steady_clock::now ();
    for (unsigned n = 0; n < count; ++n)
      if (!builder.call (**target, {std::int64_t{42}})) return 1;
    auto end = std::chrono::steady_clock::now ();
    if (!builder.ret () || !calls.verify () || (*f)->instruction_count () != count + 1) return 1;
    std::uint64_t checksum = 0;
    for (auto &instruction : (*block)->instructions ())
      if (instruction.opcode () == Opcode::Call)
        checksum += *instruction.operands ()[3].get<std::int64_t> ();
    if (checksum != 42ULL * count || std::ranges::distance ((*f)->registers ()) != count) return 1;
    std::cout << "calls=" << count << " checksum=" << checksum
              << " total_us=" << microseconds (end - start)
              << " ns_per_call=" << microseconds (end - start) * 1000 / count << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Context scoped;
    auto m = scoped.create_module ("scoped");
    if (!m) return 1;
    auto f = (*m)->create_function ("entry", {});
    if (!f) return 1;
    auto first = (*f)->create_stack_scope (), second = (*f)->create_stack_scope ();
    if (!first || !second) return 1;
    auto value = (*f)->create_register (Type::i64 ());
    if (!value) return 1;
    for (unsigned n = 0; n < count; ++n) {
      auto block = (*f)->create_block ({}, n & 1 ? *first : *second);
      if (!block) return 1;
      IRBuilder builder (**block);
      if (!builder.assign (**value, Operand (std::int64_t{42}))) return 1;
      if (n + 1 == count && !builder.ret ()) return 1;
    }
    auto start = std::chrono::steady_clock::now ();
    auto lowered = materialize_stack_scopes (**f);
    auto end = std::chrono::steady_clock::now ();
    if (!lowered || lowered->saves != count || lowered->restores != count || !scoped.verify ())
      return 1;
    std::cout << "scope_blocks=" << count << " saves=" << lowered->saves
              << " restores=" << lowered->restores << " total_us=" << microseconds (end - start)
              << " ns_per_block=" << microseconds (end - start) * 1000 / count << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Context variables;
    auto m = variables.create_module ("variables");
    if (!m) return 1;
    auto value = (*m)->create_variable ("value", Type::i64 (), 42);
    if (!value) return 1;
    auto flag = (*m)->create_variable ("flag", Type (Type::Kind::Bool));
    if (!flag) return 1;
    auto f = (*m)->create_function ("entry", {});
    if (!f) return 1;
    auto block = (*f)->create_block ();
    if (!block) return 1;
    IRBuilder builder (**block);
    auto start = std::chrono::steady_clock::now ();
    for (unsigned n = 0; n < count; ++n)
      if (!builder.load (**value)) return 1;
    auto middle = std::chrono::steady_clock::now ();
    for (unsigned n = 0; n < count; ++n)
      if (!builder.store (**flag, -7)) return 1;
    auto end = std::chrono::steady_clock::now ();
    if (!builder.ret () || !variables.verify () || (*f)->instruction_count () != 4 * count + 1)
      return 1;
    std::size_t loads = 0, stores = 0;
    for (auto &instruction : (*block)->instructions ()) {
      if (instruction.opcode () == Opcode::Ne) return 1;
      if (instruction.opcode () != Opcode::Mov) continue;
      if (instruction.operands ()[1].get<Memory> ()) ++loads;
      if (instruction.operands ()[0].get<Memory> ()) {
        auto literal = instruction.operands ()[1].get<std::int64_t> ();
        if (!literal || *literal != 1) return 1;
        ++stores;
      }
    }
    if (loads != count || stores != count
        || std::ranges::distance ((*f)->registers ()) != 3 * count)
      return 1;
    std::cout << "variable_accesses=" << count << " load_us=" << microseconds (middle - start)
              << " bool_store_us=" << microseconds (end - middle)
              << " ns_per_load=" << microseconds (middle - start) * 1000 / count
              << " ns_per_bool_store=" << microseconds (end - middle) * 1000 / count << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Context expressions;
    auto m = expressions.create_module ("expressions");
    if (!m) return 1;
    auto f = (*m)->create_function ("entry", {});
    if (!f) return 1;
    auto block = (*f)->create_block ();
    if (!block) return 1;
    IRBuilder builder (**block);
    auto start = std::chrono::steady_clock::now ();
    for (unsigned n = 0; n < count; ++n)
      if (!builder.binary (Opcode::Add, 20, 22)) return 1;
    auto middle = std::chrono::steady_clock::now ();
    auto literal = builder.literal (20);
    for (unsigned n = 0; n < count; ++n)
      if (!(literal + 22).is_valid ()) return 1;
    auto end = std::chrono::steady_clock::now ();
    if (!builder.ret () || !expressions.verify () || (*f)->instruction_count () != 2 * count + 1
        || std::ranges::distance ((*f)->registers ()) != 2 * count)
      return 1;
    std::uint64_t checksum = 0;
    for (auto &instruction : (*block)->instructions ())
      if (instruction.opcode () == Opcode::Add) {
        checksum += *instruction.operands ()[1].get<std::int64_t> ()
                    + *instruction.operands ()[2].get<std::int64_t> ();
      }
    if (checksum != 84ULL * count) return 1;
    std::cout << "expressions=" << count << " checksum=" << checksum
              << " raw_us=" << microseconds (middle - start)
              << " typed_us=" << microseconds (end - middle)
              << " ns_per_raw=" << microseconds (middle - start) * 1000 / count
              << " ns_per_typed=" << microseconds (end - middle) * 1000 / count << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Context input;
    auto m = input.create_module ("simplify");
    if (!m) return 1;
    auto f = (*m)->create_function ("entry", {});
    if (!f) return 1;
    auto b = (*f)->create_block ();
    if (!b) return 1;
    IRBuilder builder (**b);
    for (unsigned n = 0; n < count; ++n)
      if (!builder.binary (Opcode::Add, 20, 22)) return 1;
    if (!builder.ret ()) return 1;
    FunctionAnalysisManager analyses (**f);
    mirnext::transforms::FunctionPassManager passes;
    passes.add<mirnext::transforms::SimplifyPass> ();
    auto start = std::chrono::steady_clock::now ();
    auto result = passes.run (**f, analyses);
    auto end = std::chrono::steady_clock::now ();
    if (!result || !input.verify () || (*f)->instruction_count () != 3 * count + 1
        || std::ranges::distance ((*f)->registers ()) != count + 2)
      return 1;
    std::uint64_t checksum = 0;
    for (auto &instruction : (*b)->instructions ())
      if (instruction.opcode () == Opcode::Mov)
        checksum += *instruction.operands ()[1].get<std::int64_t> ();
    if (checksum != 42ULL * count) return 1;
    std::cout << "simplify_adds=" << count << " checksum=" << checksum
              << " instructions=" << (*f)->instruction_count ()
              << " registers=" << std::ranges::distance ((*f)->registers ())
              << " total_us=" << microseconds (end - start)
              << " ns_per_add=" << microseconds (end - start) * 1000 / count << '\n';
  }
}
