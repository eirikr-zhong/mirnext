/* Licensed under the MIT License. Owning replacements for old MIR utility fixtures. */
#pragma once
#include "mirnext/Serialization/Format.hpp"
#include <array>
namespace mirnext::test {
inline constexpr std::string_view utility_memory_text = R"mir(memory:module
memop:func i64,i64:arg1,i64:arg2
add i64:(arg1,arg2,8),i64:64(arg1),i64:(,arg1,8)
ret i64:(arg1)
ret i64:(,arg2,1)
ret i64:1024
mov i64:(arg1,arg2,8),i64:(arg1)
ret 0
endfunc
endmodule
)mir";
inline std::string utility_unterminated_memory_text() {
  std::string text(utility_memory_text); text.erase(text.find("ret 0\nendfunc"),6); return text;
}
// Signature and inline intent of issue 219; sessions load immutable snapshots
// rather than extending an old mutable runtime after linking.
inline constexpr std::string_view utility_add_text = R"mir(m_add:module
export add
add:func i32,i32:a0,i32:a1
local i64:r0
add r0,a0,a1
ret r0
endfunc
endmodule
)mir";
inline Result<ir::Function *> append_api_sieve(ir::Context &context,std::string_view name,
                                             std::size_t flags = 819000,unsigned passes = 100) {
  using namespace ir;
  if (flags < 4 || flags > std::size_t(INT64_MAX) || !passes)
    return Error{ErrorCode::InvalidArgument,"invalid API sieve workload"};
  auto module = context.create_module(name); if (!module) return module.error();
  auto function = (*module)->create_function("sieve",{{Type::i64()},{}}); if (!function) return function.error();
  std::array<BasicBlock *,15> blocks{}; std::array<Label *,15> labels{};
  for (unsigned n = 0; n < blocks.size(); ++n) {
    auto block = (*function)->create_block(); if (!block) return block.error(); blocks[n] = *block;
    auto label = (*function)->create_label(**block,"L"+std::to_string(n)); if (!label) return label.error(); labels[n] = *label;
  }
  std::array<Register *,5> regs{}; constexpr std::array names{"iter","count","i","k","prime"};
  for (unsigned n = 0; n < regs.size(); ++n) { auto r = (*function)->create_register(Type::i64(),names[n]); if (!r) return r.error(); regs[n] = *r; }
  auto [iter,count,i,k,prime] = regs; IRBuilder b(*blocks[0]);
  auto memory = b.allocate_stack(Operand(std::int64_t(flags)),"flags"); if (!memory) return memory.error();
  std::optional<Error> error;
  auto check = [&](auto result) { if (!result && !error) error = result.error(); };
  auto point = [&](unsigned n) { check(b.set_insert_point(*blocks[n])); };
  auto value = [&](auto r) { return b.expression(*r); };
  auto increment = [&](auto r) { check(b.assign(*r,value(r)+b.literal(1))); };
  check(b.assign(*iter,0));
  point(1); check(b.branch(Comparison::GreaterEqual,value(iter),b.literal(std::int64_t(passes)),*labels[14]));
  point(2); check(b.assign(*count,0)); check(b.assign(*i,0));
  point(3); check(b.branch(Comparison::GreaterEqual,value(i),b.literal(std::int64_t(flags)),*labels[5]));
  point(4); check(b.store(Memory{Type(Type::Kind::U8),*memory,i},1)); increment(i); check(b.jump(*labels[3]));
  point(5); check(b.assign(*i,1));
  point(6); check(b.branch(Comparison::GreaterEqual,value(i),b.literal(std::int64_t(flags)),*labels[13]));
  point(7); check(b.branch(Comparison::Equal,b.expression(Memory{Type(Type::Kind::U8),*memory,i}),b.literal(Type(Type::Kind::U8),0),*labels[12]));
  point(8); check(b.assign(*prime,value(i)+b.literal(1))); check(b.assign(*k,value(i)+value(prime)));
  point(9); check(b.branch(Comparison::GreaterEqual,value(k),b.literal(std::int64_t(flags)),*labels[11]));
  point(10); check(b.store(Memory{Type(Type::Kind::U8),*memory,k},0)); check(b.assign(*k,value(k)+value(prime))); check(b.jump(*labels[9]));
  point(11); increment(count);
  point(12); increment(i); check(b.jump(*labels[6]));
  point(13); increment(iter); check(b.jump(*labels[1]));
  point(14); check(b.ret(Operand(*count)));
  if (error) return *error; return *function;
}
inline Result<std::unique_ptr<ir::Context>> utility_hello() {
  using namespace ir;
  auto context = std::make_unique<Context>(); auto module = context->create_module("hello"); if (!module) return module.error();
  InitializedData data{Type(Type::Kind::U8),{}}; constexpr char greetings[] = "world\0all\0\0";
  for (auto byte : greetings) data.values.emplace_back(static_cast<unsigned char>(byte));
  auto strings = (*module)->create_data("greetings",std::move(data)); if (!strings) return strings.error();
  auto callback = (*module)->prototype("cb",{{Type(Type::Kind::I32)},{Type::pointer()}}); if (!callback) return callback.error();
  auto function = (*module)->create_function("hello",{{Type(Type::Kind::I32)},{Type::pointer(),Type::pointer(),Type(Type::Kind::I32)}}); if (!function) return function.error();
  auto block = (*function)->create_block(); if (!block) return block.error(); IRBuilder b(**block); auto args = (*function)->arguments();
  auto base = b.address_of((*strings)->symbol()); if (!base) return base.error();
  auto offset = b.binary(Opcode::Mul,Operand(*args[2]),6); if (!offset) return offset.error();
  auto selected = b.binary(Opcode::Add,Operand(**base),Operand(**offset)); if (!selected) return selected.error();
  auto first = b.call_indirect(**callback,Operand(*args[1]),{Operand(*args[0])}); if (!first) return first.error();
  auto second = b.call_indirect(**callback,Operand(*args[1]),{Operand(**selected)}); if (!second) return second.error();
  auto left = first->value(),right = second->value(); if (!left) return left.error(); if (!right) return right.error();
  auto returned = b.ret(b.expression(**left)+b.expression(**right)); if (!returned) return returned.error();
  auto valid = context->verify(); if (!valid) return valid.error(); return context;
}
}
