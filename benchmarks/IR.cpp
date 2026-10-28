/* Licensed under the MIT License. Timing is informational, never a test threshold. */
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
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
  std::cout << "sizeof_operand=" << sizeof (Operand)
            << " sizeof_expression=" << sizeof (mirnext::ir::detail::ExpressionState)
            << " sizeof_call_argument=" << sizeof (mirnext::ir::detail::CallArgumentState) << '\n';
  for (unsigned count : {4096, 16384, 65536}) {
    Program input;
    auto module = mirnext::ir::detail::access (input).create_module ("ssa");
    if (!(module).has_value ()) return 1;
    auto function = mirnext::ir::detail::access (((module)).value ())
                      .create_function ("entry", {{Type::i64 ()}, {Type::i64 ()}});
    if (!(function).has_value ()) return 1;
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    auto source
      = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ()),
      value = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ());
    if (!(block).has_value () || !(source).has_value () || !(value).has_value ()) return 1;
    InstructionBuilder body ((block).value ());
    if (!(body.assign ((source).value (), 1)).has_value ()
        || !(body.assign ((source).value (), 42)).has_value ())
      return 1;
    for (unsigned n = 0; n < count; ++n)
      if (!(body.emit (Opcode::Add, {Operand ((value).value ()), Operand ((source).value ()),
                                     Operand ((source).value ())}))
             .has_value ())
        return 1;
    if (!(body.ret (Operand ((value).value ()))).has_value ()) return 1;
    FunctionAnalysisManager analyses ((function).value ());
    auto graph = analyses.get<FunctionGraphAnalysis> ();
    if (!(graph).has_value ()) return 1;
    auto start = std::chrono::steady_clock::now ();
    mirnext::transforms::BuildSSAPass build;
    auto result = ::mirnext::transforms::detail::run_pass (build, (function).value (), analyses);
    if (!(result).has_value ()) return 1;
    auto end = std::chrono::steady_clock::now ();
    std::size_t uses = 0;
    for (auto &use : function.value ().ssa_form ().get ()->uses ()) uses += use.alive ();
    auto ns = std::chrono::duration<double, std::nano> (end - start).count ();
    std::cout << "ssa_adds=" << count << " uses=" << uses << " build_us=" << ns / 1000
              << " ns_per_add=" << ns / count << '\n';
    if (uses != 2 * count + 1) return 1;
    mirnext::transforms::ConventionalSSAPass conventional;
    mirnext::transforms::DestroySSAPass destroy;
    if (!(::mirnext::transforms::detail::run_pass (conventional, (function).value (), analyses))
           .has_value ()
        || !(::mirnext::transforms::detail::run_pass (destroy, (function).value (), analyses))
              .has_value ())
      return 1;
  }
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("benchmark");
  if (!(module).has_value ()) return 1;
  auto function = mirnext::ir::detail::access (((module)).value ()).create_function ("entry", {});
  if (!(function).has_value ()) return 1;
  auto reg
    = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 (), "value");
  if (!(reg).has_value ()) return 1;
  for (unsigned n = 0; n < 256; ++n) {
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    if (!(block).has_value ()) return 1;
    InstructionBuilder builder ((block).value ());
    if (!(builder.assign ((reg).value (), Operand (static_cast<std::int64_t> (n)))).has_value ())
      return 1;
    if (n == 255 && !(builder.ret ()).has_value ()) return 1;
  }
  FunctionAnalysisManager analyses ((function).value ());
  auto warm = analyses.get<ControlFlowAnalysis> ();
  if (!(warm).has_value ()) return 1;
  constexpr unsigned iterations = 2000;
  std::uint64_t cached_sum = 0, rebuilt_sum = 0;
  auto start = std::chrono::steady_clock::now ();
  for (unsigned n = 0; n < iterations; ++n) {
    auto graph = analyses.get<ControlFlowAnalysis> ();
    if (!(graph).has_value ()) return 1;
    cached_sum += graph.value ().edges.size ();
  }
  auto middle = std::chrono::steady_clock::now ();
  for (unsigned n = 0; n < iterations; ++n) {
    auto graph = ControlFlowAnalysis::run ((function).value ());
    if (!(graph).has_value ()) return 1;
    rebuilt_sum += graph.value ().edges.size ();
  }
  auto end = std::chrono::steady_clock::now ();
  if (cached_sum != rebuilt_sum) return 1;
  auto microseconds
    = [] (auto duration) { return std::chrono::duration<double, std::micro> (duration).count (); };
  std::cout << "blocks=256 iterations=" << iterations << " checksum=" << cached_sum
            << " cached_us=" << microseconds (middle - start)
            << " rebuilt_us=" << microseconds (end - middle) << '\n';
  for (unsigned count : {256, 1024, 4096}) {
    Program input;
    auto m = mirnext::ir::detail::access (input).create_module ("dominance");
    if (!(m).has_value ()) return 1;
    auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {});
    if (!(f).has_value ()) return 1;
    for (unsigned n = 0; n < count; ++n) {
      auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
      if (!(block).has_value ()
          || !(mirnext::ir::detail::access (((f)).value ()).create_label ((block).value ()))
                .has_value ())
        return 1;
      if (n + 1 == count && !(InstructionBuilder ((block).value ()).ret ()).has_value ()) return 1;
    }
    FunctionAnalysisManager analyses ((f).value ());
    auto graph = analyses.get<FunctionGraphAnalysis> ();
    if (!(graph).has_value ()) return 1;
    auto &g = (graph).value ();
    auto start = std::chrono::steady_clock::now ();
    auto dom = analyses.get<DominatorAnalysis> ();
    auto middle = std::chrono::steady_clock::now ();
    if (!(dom).has_value ()) return 1;
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
        if (dom.value ().dominates (a, b) != expected) return 1;
        checksum += expected;
      }
    std::cout << "dominance_blocks=" << blocks << " checksum=" << checksum
              << " tree_us=" << microseconds (middle - start)
              << " dense_fixed_point_us=" << microseconds (end - middle)
              << " tree_fact_bytes=" << blocks * 3 * sizeof (std::size_t)
              << " dense_fact_bytes=" << blocks * words * sizeof (std::uint64_t) << '\n';
  }
  for (unsigned count : {256, 1024, 4096}) {
    Program input;
    auto m = mirnext::ir::detail::access (input).create_module ("inline");
    if (!(m).has_value ()) return 1;
    auto leaf
      = mirnext::ir::detail::access (((m)).value ()).create_function ("leaf", {{Type::i64 ()}, {}});
    if (!(leaf).has_value ()) return 1;
    auto b = mirnext::ir::detail::access (((leaf)).value ()).create_block ();
    if (!(b).has_value ()) return 1;
    auto value = mirnext::ir::detail::access (((leaf)).value ()).create_register (Type::i64 ());
    if (!(value).has_value ()) return 1;
    InstructionBuilder body ((b).value ());
    if (!(body.assign ((value).value (), 42)).has_value ()
        || !(body.ret (Operand ((value).value ()))).has_value ())
      return 1;
    for (unsigned n = 0; n < count; ++n) {
      auto f = mirnext::ir::detail::access (((m)).value ())
                 .create_function ("caller" + std::to_string (n), {{Type::i64 ()}, {}});
      if (!(f).has_value ()) return 1;
      auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
      if (!(block).has_value ()) return 1;
      InstructionBuilder builder ((block).value ());
      auto call = builder.call ((leaf).value ());
      if (!(call).has_value ()) return 1;
      auto result = call.value ().value ();
      if (!(result).has_value () || !(builder.ret (Operand ((result).value ()))).has_value ())
        return 1;
    }
    auto separate = clone (input);
    if (!(separate).has_value ()) return 1;
    transforms::FunctionPassManager passes;
    passes.add<transforms::InlinePass> ();
    auto start = std::chrono::steady_clock::now ();
    auto result = passes.run (input);
    auto middle = std::chrono::steady_clock::now ();
    if (!(result).has_value ()) return 1;
    for (auto &module : separate.value ().modules ())
      for (auto &function : module.functions ()) {
        FunctionAnalysisManager analyses (function);
        if (!(passes.run (function, analyses)).has_value ()) return 1;
      }
    auto end = std::chrono::steady_clock::now ();
    if (dump (input) != dump ((separate).value ())) return 1;
    std::uint64_t checksum = 0;
    for (auto &f : m.value ().functions ())
      for (auto &block : f.blocks ())
        for (auto &i : block.instructions ()) {
          if (instruction_descriptor (i.opcode ()).get ()->count == OperandCount::Call) return 1;
          if (i.opcode () == Opcode::Mov
              && mirnext::ir::detail::operand_access (i.operands ()[1]).get<std::int64_t> ())
            checksum
              += *mirnext::ir::detail::operand_access (i.operands ()[1]).get<std::int64_t> ();
        }
    if (checksum != 42ULL * (count + 1)) return 1;
    std::cout << "inline_callers=" << count << " checksum=" << checksum
              << " scoped_us=" << microseconds (middle - start)
              << " per_function_scope_us=" << microseconds (end - middle) << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Program named;
    auto m = mirnext::ir::detail::access (named).create_module ("named");
    if (!(m).has_value ()) return 1;
    auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {});
    if (!(f).has_value ()) return 1;
    auto start = std::chrono::steady_clock::now ();
    for (unsigned n = 0; n < count; ++n)
      if (!(mirnext::ir::detail::access (((f)).value ())
              .create_block ("block" + std::to_string (n)))
             .has_value ())
        return 1;
    auto end = std::chrono::steady_clock::now ();
    auto &last = *std::prev (f.value ().blocks ().end ());
    if (!(InstructionBuilder (last).ret ()).has_value () || !(named.verify ()).has_value ())
      return 1;
    std::uint64_t checksum = 0;
    for (auto &b : f.value ().blocks ()) checksum += b.name ().size ();
    if (std::ranges::distance (f.value ().blocks ()) != count) return 1;
    std::cout << "named_blocks=" << count << " checksum=" << checksum
              << " total_us=" << microseconds (end - start)
              << " ns_per_block=" << microseconds (end - start) * 1000 / count << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Program rewrite_context;
    auto rewrite_module = mirnext::ir::detail::access (rewrite_context).create_module ("rewrite");
    if (!(rewrite_module).has_value ()) return 1;
    auto rewrite_function
      = mirnext::ir::detail::access (((rewrite_module)).value ()).create_function ("entry", {});
    if (!(rewrite_function).has_value ()) return 1;
    auto block = mirnext::ir::detail::access (((rewrite_function)).value ()).create_block ();
    if (!(block).has_value ()) return 1;
    InstructionBuilder builder ((block).value ());
    std::vector<Instruction *> instructions;
    instructions.reserve (count);
    for (unsigned n = 0; n < count; ++n) {
      auto value = mirnext::ir::detail::access (((rewrite_function)).value ())
                     .create_register (Type::i64 ());
      if (!(value).has_value ()) return 1;
      auto instruction = builder.assign ((value).value (), Operand (std::int64_t (0)));
      if (!(instruction).has_value ()) return 1;
      instructions.push_back (&(instruction).value ());
    }
    if (!(builder.ret ()).has_value ()) return 1;
    InstructionRewriter rewriter ((rewrite_function).value ());
    auto rewrite_start = std::chrono::steady_clock::now ();
    for (auto instruction : instructions)
      if (!(rewriter.replace_operand (*instruction, 1, Operand (std::int64_t (42)))).has_value ())
        return 1;
    auto rewrite_end = std::chrono::steady_clock::now ();
    if (!(rewrite_context.verify ()).has_value ()) return 1;
    std::uint64_t checksum = 0;
    for (auto instruction : instructions)
      checksum
        += *mirnext::ir::detail::operand_access (instruction->operands ()[1]).get<std::int64_t> ();
    if (checksum != 42ULL * count) return 1;
    std::cout << "rewrites=" << count << " checksum=" << checksum
              << " total_us=" << microseconds (rewrite_end - rewrite_start)
              << " ns_per_rewrite=" << microseconds (rewrite_end - rewrite_start) * 1000 / count
              << '\n';
    auto write_start = std::chrono::steady_clock::now ();
    auto text = mirnext::serialization::write_text (rewrite_context);
    if (!(text).has_value ()) return 1;
    auto parse_start = std::chrono::steady_clock::now ();
    auto parsed = mirnext::serialization::parse_text ((text).value ());
    if (!(parsed).has_value ()) return 1;
    auto parse_end = std::chrono::steady_clock::now ();
    auto &parsed_function = *parsed.value ().modules ().begin ()->functions ().begin ();
    if (parsed_function.instruction_count () != count + 1) return 1;
    std::uint64_t parsed_sum = 0;
    for (auto &parsed_block : parsed_function.blocks ())
      for (auto &instruction : parsed_block.instructions ())
        if (instruction.opcode () == Opcode::Mov)
          parsed_sum += *mirnext::ir::detail::operand_access (instruction.operands ()[1])
                           .get<std::int64_t> ();
    if (parsed_sum != checksum) return 1;
    std::cout << "text_instructions=" << count + 1 << " bytes=" << text.value ().size ()
              << " checksum=" << parsed_sum
              << " write_us=" << microseconds (parse_start - write_start)
              << " parse_us=" << microseconds (parse_end - parse_start) << '\n';
    auto binary_start = std::chrono::steady_clock::now ();
    auto binary = mirnext::serialization::write_binary (rewrite_context);
    if (!(binary).has_value ()) return 1;
    auto binary_read_start = std::chrono::steady_clock::now ();
    auto decoded = mirnext::serialization::read_binary ((binary).value ());
    if (!(decoded).has_value ()) return 1;
    auto binary_end = std::chrono::steady_clock::now ();
    auto &decoded_function = *decoded.value ().modules ().begin ()->functions ().begin ();
    if (decoded_function.instruction_count () != count + 1) return 1;
    std::uint64_t binary_sum = 0;
    for (auto &decoded_block : decoded_function.blocks ())
      for (auto &instruction : decoded_block.instructions ())
        if (instruction.opcode () == Opcode::Mov)
          binary_sum += *mirnext::ir::detail::operand_access (instruction.operands ()[1])
                           .get<std::int64_t> ();
    if (binary_sum != checksum) return 1;
    std::cout << "binary_instructions=" << count + 1
              << " compressed_bytes=" << binary.value ().size () << " checksum=" << binary_sum
              << " write_us=" << microseconds (binary_read_start - binary_start)
              << " read_us=" << microseconds (binary_end - binary_read_start) << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Program edits;
    auto m = mirnext::ir::detail::access (edits).create_module ("edits");
    if (!(m).has_value ()) return 1;
    auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {});
    if (!(f).has_value ()) return 1;
    auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
    if (!(block).has_value ()) return 1;
    auto value = mirnext::ir::detail::access (((f)).value ()).create_register (Type::i64 ());
    if (!(value).has_value ()) return 1;
    InstructionBuilder builder ((block).value ());
    std::vector<Instruction *> instructions;
    std::vector<const Operand *> operand_addresses;
    instructions.reserve (count);
    operand_addresses.reserve (count);
    for (unsigned n = 0; n < count; ++n) {
      auto instruction = builder.assign ((value).value (), Operand (std::int64_t{42}));
      if (!(instruction).has_value ()) return 1;
      instructions.push_back (&(instruction).value ());
      operand_addresses.push_back (instruction.value ().operands ().data ());
    }
    if (!(builder.ret ()).has_value ()) return 1;
    InstructionRewriter rewriter ((f).value ());
    auto start = std::chrono::steady_clock::now ();
    for (unsigned n = count; n > 0; --n)
      if (!(rewriter.split_before (*instructions[n - 1], "split" + std::to_string (n)))
             .has_value ())
        return 1;
    auto middle = std::chrono::steady_clock::now ();
    auto &first = *f.value ().blocks ().begin ();
    for (unsigned n = 0; n < count; ++n)
      if (!(rewriter.merge_fallthrough (first)).has_value ()) return 1;
    auto end = std::chrono::steady_clock::now ();
    if (!(edits.verify ()).has_value () || std::ranges::distance (f.value ().blocks ()) != 1
        || f.value ().instruction_count () != count + 1)
      return 1;
    std::uint64_t checksum = 0;
    for (unsigned n = 0; n < count; ++n) {
      if (instructions[n]->operands ().data () != operand_addresses[n]
          || &instructions[n]->block () != &first)
        return 1;
      checksum += *mirnext::ir::detail::operand_access (instructions[n]->operands ()[1])
                     .get<std::int64_t> ();
    }
    if (checksum != 42ULL * count) return 1;
    std::cout << "structural_edits=" << count << " checksum=" << checksum
              << " split_us=" << microseconds (middle - start)
              << " merge_us=" << microseconds (end - middle)
              << " ns_per_split=" << microseconds (middle - start) * 1000 / count
              << " ns_per_merge=" << microseconds (end - middle) * 1000 / count << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Program calls;
    auto m = mirnext::ir::detail::access (calls).create_module ("calls");
    if (!(m).has_value ()) return 1;
    auto target = mirnext::ir::detail::access (((m)).value ())
                    .import ("target", Signature{{Type::i64 ()}, {Type::i64 ()}});
    if (!(target).has_value ()) return 1;
    auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {});
    if (!(f).has_value ()) return 1;
    auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
    if (!(block).has_value ()) return 1;
    InstructionBuilder builder ((block).value ());
    auto start = std::chrono::steady_clock::now ();
    for (unsigned n = 0; n < count; ++n)
      if (!(builder.call ((target).value (), {std::int64_t{42}})).has_value ()) return 1;
    auto end = std::chrono::steady_clock::now ();
    if (!(builder.ret ()).has_value () || !(calls.verify ()).has_value ()
        || f.value ().instruction_count () != count + 1)
      return 1;
    std::uint64_t checksum = 0;
    for (auto &instruction : block.value ().instructions ())
      if (instruction.opcode () == Opcode::Call)
        checksum
          += *mirnext::ir::detail::operand_access (instruction.operands ()[3]).get<std::int64_t> ();
    if (checksum != 42ULL * count || std::ranges::distance (f.value ().registers ()) != count)
      return 1;
    std::cout << "calls=" << count << " checksum=" << checksum
              << " total_us=" << microseconds (end - start)
              << " ns_per_call=" << microseconds (end - start) * 1000 / count << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Program scoped;
    auto m = mirnext::ir::detail::access (scoped).create_module ("scoped");
    if (!(m).has_value ()) return 1;
    auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {});
    if (!(f).has_value ()) return 1;
    auto first = mirnext::ir::detail::access (((f)).value ()).create_stack_scope (),
         second = mirnext::ir::detail::access (((f)).value ()).create_stack_scope ();
    if (!(first).has_value () || !(second).has_value ()) return 1;
    auto value = mirnext::ir::detail::access (((f)).value ()).create_register (Type::i64 ());
    if (!(value).has_value ()) return 1;
    for (unsigned n = 0; n < count; ++n) {
      auto block = mirnext::ir::detail::access (((f)).value ())
                     .create_block ({}, n & 1 ? &(first).value () : &(second).value ());
      if (!(block).has_value ()) return 1;
      InstructionBuilder builder ((block).value ());
      if (!(builder.assign ((value).value (), Operand (std::int64_t{42}))).has_value ()) return 1;
      if (n + 1 == count && !(builder.ret ()).has_value ()) return 1;
    }
    auto start = std::chrono::steady_clock::now ();
    auto lowered = materialize_stack_scopes ((f).value ());
    auto end = std::chrono::steady_clock::now ();
    if (!(lowered).has_value () || lowered.value ().saves != count
        || lowered.value ().restores != count || !(scoped.verify ()).has_value ())
      return 1;
    std::cout << "scope_blocks=" << count << " saves=" << lowered.value ().saves
              << " restores=" << lowered.value ().restores
              << " total_us=" << microseconds (end - start)
              << " ns_per_block=" << microseconds (end - start) * 1000 / count << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Program variables;
    auto m = mirnext::ir::detail::access (variables).create_module ("variables");
    if (!(m).has_value ()) return 1;
    auto value
      = mirnext::ir::detail::access (((m)).value ()).create_variable ("value", Type::i64 (), 42);
    if (!(value).has_value ()) return 1;
    auto flag = mirnext::ir::detail::access (((m)).value ())
                  .create_variable ("flag", Type (Type::Kind::Bool));
    if (!(flag).has_value ()) return 1;
    auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {});
    if (!(f).has_value ()) return 1;
    auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
    if (!(block).has_value ()) return 1;
    InstructionBuilder builder ((block).value ());
    auto start = std::chrono::steady_clock::now ();
    for (unsigned n = 0; n < count; ++n)
      if (!(builder.load ((value).value ())).has_value ()) return 1;
    auto middle = std::chrono::steady_clock::now ();
    for (unsigned n = 0; n < count; ++n)
      if (!(builder.store ((flag).value (), -7)).has_value ()) return 1;
    auto end = std::chrono::steady_clock::now ();
    if (!(builder.ret ()).has_value () || !(variables.verify ()).has_value ()
        || f.value ().instruction_count () != 4 * count + 1)
      return 1;
    std::size_t loads = 0, stores = 0;
    for (auto &instruction : block.value ().instructions ()) {
      if (instruction.opcode () == Opcode::Ne) return 1;
      if (instruction.opcode () != Opcode::Mov) continue;
      if (mirnext::ir::detail::operand_access (instruction.operands ()[1]).get<Memory> ()) ++loads;
      if (mirnext::ir::detail::operand_access (instruction.operands ()[0]).get<Memory> ()) {
        auto literal
          = mirnext::ir::detail::operand_access (instruction.operands ()[1]).get<std::int64_t> ();
        if (!literal || *literal != 1) return 1;
        ++stores;
      }
    }
    if (loads != count || stores != count
        || std::ranges::distance (f.value ().registers ()) != 3 * count)
      return 1;
    std::cout << "variable_accesses=" << count << " load_us=" << microseconds (middle - start)
              << " bool_store_us=" << microseconds (end - middle)
              << " ns_per_load=" << microseconds (middle - start) * 1000 / count
              << " ns_per_bool_store=" << microseconds (end - middle) * 1000 / count << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Program expressions;
    auto m = mirnext::ir::detail::access (expressions).create_module ("expressions");
    if (!(m).has_value ()) return 1;
    auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {});
    if (!(f).has_value ()) return 1;
    auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
    if (!(block).has_value ()) return 1;
    InstructionBuilder builder ((block).value ());
    auto start = std::chrono::steady_clock::now ();
    for (unsigned n = 0; n < count; ++n)
      if (!(builder.binary (Opcode::Add, 20, 22)).has_value ()) return 1;
    auto middle = std::chrono::steady_clock::now ();
    auto literal = builder.literal (20);
    for (unsigned n = 0; n < count; ++n)
      if (!(literal + 22).is_valid ()) return 1;
    auto end = std::chrono::steady_clock::now ();
    if (!(builder.ret ()).has_value () || !(expressions.verify ()).has_value ()
        || f.value ().instruction_count () != 2 * count + 1
        || std::ranges::distance (f.value ().registers ()) != 2 * count)
      return 1;
    std::uint64_t checksum = 0;
    for (auto &instruction : block.value ().instructions ())
      if (instruction.opcode () == Opcode::Add) {
        checksum
          += *mirnext::ir::detail::operand_access (instruction.operands ()[1]).get<std::int64_t> ()
             + *mirnext::ir::detail::operand_access (instruction.operands ()[2])
                  .get<std::int64_t> ();
      }
    if (checksum != 84ULL * count) return 1;
    std::cout << "expressions=" << count << " checksum=" << checksum
              << " raw_us=" << microseconds (middle - start)
              << " typed_us=" << microseconds (end - middle)
              << " ns_per_raw=" << microseconds (middle - start) * 1000 / count
              << " ns_per_typed=" << microseconds (end - middle) * 1000 / count << '\n';
  }
  for (unsigned count : {4096, 16384, 65536}) {
    Program input;
    auto m = mirnext::ir::detail::access (input).create_module ("simplify");
    if (!(m).has_value ()) return 1;
    auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {});
    if (!(f).has_value ()) return 1;
    auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
    if (!(b).has_value ()) return 1;
    InstructionBuilder builder ((b).value ());
    for (unsigned n = 0; n < count; ++n)
      if (!(builder.binary (Opcode::Add, 20, 22)).has_value ()) return 1;
    if (!(builder.ret ()).has_value ()) return 1;
    FunctionAnalysisManager analyses ((f).value ());
    mirnext::transforms::FunctionPassManager passes;
    passes.add<mirnext::transforms::SimplifyPass> ();
    auto start = std::chrono::steady_clock::now ();
    auto result = passes.run ((f).value (), analyses);
    auto end = std::chrono::steady_clock::now ();
    if (!(result).has_value () || !(input.verify ()).has_value ()
        || f.value ().instruction_count () != 3 * count + 1
        || std::ranges::distance (f.value ().registers ()) != count + 2)
      return 1;
    std::uint64_t checksum = 0;
    for (auto &instruction : b.value ().instructions ())
      if (instruction.opcode () == Opcode::Mov)
        checksum
          += *mirnext::ir::detail::operand_access (instruction.operands ()[1]).get<std::int64_t> ();
    if (checksum != 42ULL * count) return 1;
    std::cout << "simplify_adds=" << count << " checksum=" << checksum
              << " instructions=" << f.value ().instruction_count ()
              << " registers=" << std::ranges::distance (f.value ().registers ())
              << " total_us=" << microseconds (end - start)
              << " ns_per_add=" << microseconds (end - start) * 1000 / count << '\n';
  }
}
