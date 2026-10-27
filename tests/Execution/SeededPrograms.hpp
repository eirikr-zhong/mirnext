/* Licensed under the MIT License. Bounded programs and an independent bit-level model. */
#pragma once
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
namespace mirnext::test {
struct SeededProgram {
  std::uint64_t seed, even, odd, multiplier, callee_add;
  unsigned iterations, read_slot, write_slot;
  bool hardware;
  std::string text;
  using Memory = std::array<std::uint64_t, 8>;
  Memory initial_memory () const {
    std::mt19937_64 random (seed);
    Memory memory;
    for (auto &word : memory) word = random ();
    return memory;
  }
  std::uint64_t evaluate (std::uint64_t input, Memory &memory, unsigned alias_offset) const {
    auto x = input, local = input;
    for (unsigned n = 0; n < iterations; ++n) {
      auto y = memory[read_slot], sum = x + y;
      const bool overflow = (~(x ^ y) & (x ^ sum)) >> 63;
      x = overflow ? sum + odd : sum ^ even;
      local = (local & 0xffffffff00000000ULL) | std::uint32_t (x);
      x ^= local;
      memory[alias_offset + write_slot] = x;
      x += memory[read_slot];
      x = (x & 1) ? x * multiplier : x - even;
      auto observed = memory[alias_offset];
      x = (x ^ observed) + callee_add;
      memory[alias_offset] = x;
      if (hardware) x += observed;
    }
    memory[7] = x;
    return x;
  }
};
inline std::vector<SeededProgram> seeded_programs (std::string_view binding, bool hardware = true) {
  std::mt19937_64 random (0x4d49524e455854ULL);
  std::vector<SeededProgram> programs;
  for (unsigned n = 0; n < 12; ++n) {
    SeededProgram p{random (),
                    random () % 65536,
                    random () % 65536,
                    1 + 2 * (random () % 127),
                    random () % 65536,
                    unsigned (1 + random () % 5),
                    unsigned (random () % 4),
                    unsigned (random () % 4),
                    hardware && bool (n % 2),
                    {}};
    auto number = [] (auto value) { return std::to_string (value); };
    auto declaration = p.hardware ? "global i64:bound:" + std::string (binding) + "\n" : "";
    p.text = "helper:module\nexport mix\nmix:func i64,i64:x,p:memory\nlocal i64:y\n"+declaration
      +"mov y,i64:(memory)\nxor x,x,y\nadd x,x,"+number(p.callee_add)
      +"\nmov i64:(memory),x\n"+(p.hardware ? "mov bound,y\n" : "")
      +"ret x\nendfunc\nendmodule\nmain:module\nimport mix\nprototype:proto i64,i64:x,p:memory\n"
       "target:ref mix,0\nentry:func i64,i64:input,p:left,p:right\n"
       "local i64:x,i64:y,i64:i,i64:t,i64:local,p:slot,p:callee\n"+declaration
      +"mov x,input\nmov local,input\naddr slot,local\nmov callee,target\nmov callee,i64:(callee)\nmov i,0\n"
       "loop:mov y,i64:"+number(p.read_slot*8)+"(left)\naddo x,x,y\nbo overflow\nxor x,x,"+number(p.even)
      +"\njmp joined\noverflow:add x,x,"+number(p.odd)
      +"\njoined:mov i32:(slot),x\nmov y,i64:(slot)\nxor x,x,y\nmov i64:"+number(p.write_slot*8)
      +"(right),x\nmov t,i64:"+number(p.read_slot*8)+"(left)\nadd x,x,t\nand t,x,1\nbt alternate,t\nsub x,x,"
      +number(p.even)+"\njmp after\nalternate:mul x,x,"+number(p.multiplier)
      +"\nafter:call prototype,callee,x,x,right\n"+(p.hardware ? "add x,x,bound\n" : "")
      +"add i,i,1\nblt loop,i,"+number(p.iterations)+"\nmov i64:56(left),x\nret x\nendfunc\nendmodule\n";
    programs.push_back (std::move (p));
  }
  return programs;
}
}  // namespace mirnext::test
