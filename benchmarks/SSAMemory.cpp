/* Licensed under the MIT License. Isolated checked SSA memory edit timings and ordinary
 * allocations. */
#include "mirnext/Transforms/SSA.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <new>
namespace {
bool measuring = false;
std::size_t allocations = 0;
}  // namespace
void *operator new (std::size_t size) {
  if (measuring) ++allocations;
  if (auto memory = std::malloc (size ? size : 1)) return memory;
  std::abort ();
}
void *operator new[] (std::size_t size) { return ::operator new (size); }
void operator delete (void *memory) noexcept { std::free (memory); }
void operator delete[] (void *memory) noexcept { ::operator delete (memory); }
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::analysis;
  using namespace mirnext::transforms;
  constexpr unsigned repetitions = 7;
  for (unsigned count : {1024, 8192, 32768}) {
    std::vector<double> times;
    std::vector<std::size_t> counts;
    std::uint64_t fingerprint = 0;
    for (unsigned rep = 0; rep < repetitions; ++rep) {
      Context context;
      auto module = context.create_module ("edits");
      if (!module) return 1;
      auto function
        = (*module)->create_function ("entry", {{Type::i64 ()}, {Type::i64 (), Type::i64 ()}});
      if (!function) return 1;
      auto block = (*function)->create_block ();
      if (!block) return 1;
      IRBuilder builder (**block);
      for (unsigned n = 0; n < count; ++n)
        if (!builder.emit (Opcode::Mov,
                           {Operand (Memory{Type::i64 (), (*function)->arguments ()[0]}),
                            Operand (*(*function)->arguments ()[1])}))
          return 1;
      if (!builder.ret (Operand (*(*function)->arguments ()[1]))) return 1;
      FunctionAnalysisManager analyses (**function);
      FunctionPassManager passes;
      passes.add<BuildSSAPass> ();
      passes.add<ConventionalSSAPass> ();
      if (!passes.run (**function, analyses)) return 1;
      auto form = (*function)->ssa_form ();
      auto rewriter = SSARewriter::get (**function);
      if (!rewriter) return 1;
      struct Site {
        Instruction *instruction;
        SSAValue *base;
        SSAUse *unrelated;
      };
      std::vector<Site> sites;
      sites.reserve (count);
      for (auto &b : (*function)->blocks ())
        for (auto &i : b.instructions ())
          if (i.opcode () == Opcode::Mov)
            sites.push_back (
              {&i, const_cast<SSAValue *> (&form->inputs (i)[0]->value ()), form->inputs (i)[1]});
      if (sites.size () != count) return 1;
      allocations = 0;
      measuring = true;
      auto start = std::chrono::steady_clock::now ();
      for (auto site : sites) {
        auto memory = *site.instruction->operands ()[0].get<Memory> ();
        memory.displacement = 3;
        if (!rewriter->replace_memory (*site.instruction, 0, std::move (memory), site.base,
                                       nullptr))
          return 1;
      }
      auto end = std::chrono::steady_clock::now ();
      measuring = false;
      if (!(*function)->verify ()) return 1;
      for (auto site : sites)
        if (!site.unrelated->alive ()
            || form->inputs (*site.instruction).front () != site.unrelated)
          return 2;
      std::uint64_t hash = 14695981039346656037ULL;
      for (unsigned char c : dump (**function)) hash = (hash ^ c) * 1099511628211ULL;
      for (unsigned char c : dump_ssa (**function)) hash = (hash ^ c) * 1099511628211ULL;
      if (rep && fingerprint != hash) return 2;
      fingerprint = hash;
      times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
      counts.push_back (allocations);
    }
    std::ranges::sort (times);
    std::ranges::sort (counts);
    std::cout << "shape=checked-ssa-memory edits=" << count << " repetitions=" << repetitions
              << " median_ms=" << times[repetitions / 2]
              << " median_allocations=" << counts[repetitions / 2] << " fingerprint=" << fingerprint
              << '\n';
  }
}
