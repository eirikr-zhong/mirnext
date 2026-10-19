/* Algorithms derived from MIR runtime builtins. Licensed under the MIT License. */
#include "mirnext/Execution/Builtins.hpp"
#include <array>
#include <cmath>
#include <cstring>
namespace mirnext::execution {
namespace {
struct SysVState { std::uint32_t gp_offset,fp_offset; std::uintptr_t stack,registers; };
struct ArmState { std::uintptr_t stack,gr_top,vr_top; std::int32_t gr_offset,vr_offset; };
struct PointerState { std::uintptr_t arguments; };
thread_local std::optional<Error> *invocation = nullptr;
Error invalid(std::string reason) { return {ErrorCode::InvalidOperand,"builtin: " + std::move(reason)}; }
void report(Error error) { if (invocation && !*invocation) *invocation = std::move(error); }
template<class T> T read(const std::byte *p) { T v; std::memcpy(&v,p,sizeof v); return v; }
template<class T> void write(std::byte *p,const T &v) { std::memcpy(p,&v,sizeof v); }
Result<std::uintptr_t> add(std::uintptr_t a,std::size_t n) { if (n > UINTPTR_MAX - a) return invalid("variadic address overflow"); return a + n; }
Result<std::uintptr_t> align(std::uintptr_t a,unsigned n) { auto v = add(a,n-1); if (!v) return v.error(); return *v & ~std::uintptr_t(n-1); }
Result<std::uintptr_t> saved(std::uintptr_t a,std::int32_t offset) {
  if (offset >= 0) return add(a,offset);
  auto n = static_cast<std::uint32_t>(-static_cast<std::int64_t>(offset)); if (a < n) return invalid("variadic save-area underflow"); return a - n;
}
Result<void> copy(std::span<std::byte> destination,std::uintptr_t source,std::size_t n) {
  if (!n || destination.empty()) return {};
  if (destination.size() < n || !source) return invalid("invalid variadic copy range");
  auto end = add(source,n); if (!end) return end.error(); std::memcpy(destination.data(),reinterpret_cast<const std::byte *>(source),n); return {};
}
Result<void> validate(target::ABI abi,std::span<std::byte> state) {
  auto size = variadic_state_size(abi); if (!size || !state.data() || state.size() < size) return invalid("invalid variadic state or ABI"); return {};
}
target::ABI native_abi() {
#if defined(__aarch64__) && defined(__APPLE__)
  return target::ABI::AArch64MacOS;
#elif defined(__aarch64__)
  return target::ABI::AArch64Linux;
#elif defined(_WIN32)
  return target::ABI::X86Windows;
#else
  return target::ABI::X86SysV;
#endif
}
Result<ir::Type> scalar_tag(std::uint64_t n) {
  using K = ir::Type::Kind;
  constexpr std::array tags{K::I8,K::U8,K::I16,K::U16,K::I32,K::U32,K::I64,K::U64,K::F32,K::F64,K::LongDouble,K::Pointer};
  if (n >= tags.size()) return invalid("invalid variadic scalar tag"); return ir::Type(tags[n]);
}
std::byte *va_argument(std::byte *state,std::uint64_t tag) {
  auto type = scalar_tag(tag); if (!type) { report(type.error()); return nullptr; }
  auto address = advance_variadic_scalar(native_abi(),{state,state ? variadic_state_size(native_abi()) : 0},*type);
  if (!address) { report(address.error()); return nullptr; } return reinterpret_cast<std::byte *>(*address);
}
void va_block(std::byte *destination,std::byte *state,std::size_t size,std::uint64_t classification) {
  if (classification > 4) { report(invalid("invalid variadic block class")); return; }
  auto result = advance_variadic_block(native_abi(),{state,state ? variadic_state_size(native_abi()) : 0},
    {destination,destination ? size : 0},size,static_cast<ir::Type::BlockClass>(classification)); if (!result) report(result.error());
}
void block_move(std::byte *destination,const std::byte *source,std::uint64_t words) {
  if (words > SIZE_MAX/8 || (words && (!destination || !source))) { report(invalid("invalid block-copy range")); return; }
  for (; words; --words,destination += 8,source += 8) { auto value = read<std::uint64_t>(source); write(destination,value); }
}
std::byte *argument_copy(std::byte *destination,const std::byte *source,std::size_t bytes) {
  if (bytes && (!destination || !source)) { report(invalid("invalid argument-copy range")); return destination; }
  if (bytes) std::memcpy(destination,source,bytes); return destination;
}
std::int64_t ld2i(long double value) {
  auto limit = std::ldexp(1.0L,63);
#if defined(__x86_64__) || defined(_M_X64)
  if (!std::isfinite(value) || value >= limit || value < -limit) return INT64_MIN;
#else
  if (std::isnan(value)) return 0;
  if (value >= limit) return INT64_MAX; if (value < -limit) return INT64_MIN;
#endif
  return static_cast<std::int64_t>(value);
}
float ui2f(std::uint64_t n) { return static_cast<float>(n); }
double ui2d(std::uint64_t n) { return static_cast<double>(n); }
long double ui2ld(std::uint64_t n) { return static_cast<long double>(n); }
long double i2ld(std::int64_t n) { return static_cast<long double>(n); }
long double f2ld(float n) { return n; } long double d2ld(double n) { return n; }
float ld2f(long double n) { return static_cast<float>(n); } double ld2d(long double n) { return static_cast<double>(n); }
long double ldadd(long double a,long double b) { return a+b; } long double ldsub(long double a,long double b) { return a-b; }
long double ldmul(long double a,long double b) { return a*b; } long double lddiv(long double a,long double b) { return a/b; }
long double ldneg(long double a) { return -a; }
std::int64_t ldeq(long double a,long double b) { return a==b; } std::int64_t ldne(long double a,long double b) { return a!=b; }
std::int64_t ldlt(long double a,long double b) { return a<b; } std::int64_t ldle(long double a,long double b) { return a<=b; }
std::int64_t ldgt(long double a,long double b) { return a>b; } std::int64_t ldge(long double a,long double b) { return a>=b; }
}
ExecutionInvocation::ExecutionInvocation() : previous_(invocation) { invocation = &error_; }
ExecutionInvocation::~ExecutionInvocation() { invocation = previous_; }
Result<void> ExecutionInvocation::status() const { return error_ ? Result<void>(*error_) : Result<void>{}; }
void detail::report_execution_error(Error error) { report(std::move(error)); }
std::size_t variadic_state_size(target::ABI abi) {
  switch (abi) { case target::ABI::X86SysV: return sizeof(SysVState); case target::ABI::AArch64Linux: return sizeof(ArmState);
    case target::ABI::X86Windows: case target::ABI::AArch64MacOS: return sizeof(PointerState); } return 0;
}
Result<std::uintptr_t> advance_variadic_scalar(target::ABI abi,std::span<std::byte> state,ir::Type type,unsigned ld_size) {
  auto valid = validate(abi,state); if (!valid) return valid.error();
  if (!type.valid() || !type.is_scalar() || (ld_size != 8 && ld_size != 16)) return invalid("invalid scalar variadic type");
  using K = ir::Type::Kind; bool ld = type.kind() == K::LongDouble, fp = type.kind() == K::F32 || type.kind() == K::F64;
  std::uintptr_t address;
  if (abi == target::ABI::X86SysV) {
    auto va = read<SysVState>(state.data());
    if ((fp && va.fp_offset <= 160) || (!fp && !ld && va.gp_offset <= 40)) if (!va.registers) return invalid("null variadic register save area");
    if (fp && va.fp_offset <= 160) { auto p = add(va.registers,va.fp_offset); if (!p) return p.error(); address = *p; va.fp_offset += 16; }
    else if (!fp && !ld && va.gp_offset <= 40) { auto p = add(va.registers,va.gp_offset); if (!p) return p.error(); address = *p; va.gp_offset += 8; }
    else { auto p = ld && ld_size == 16 ? align(va.stack,16) : Result<std::uintptr_t>(va.stack); if (!p) return p.error(); address = *p;
      auto next = add(address,ld ? ld_size : 8); if (!next) return next.error(); va.stack = *next; }
    if (!address) return invalid("null variadic argument address"); write(state.data(),va);
  } else if (abi == target::ABI::AArch64Linux) {
    auto va = read<ArmState>(state.data()); fp |= ld;
    if (fp && va.vr_offset < 0) { auto p = saved(va.vr_top,va.vr_offset); if (!p) return p.error(); address = *p; va.vr_offset += 16; }
    else if (!fp && va.gr_offset < 0) { auto p = saved(va.gr_top,va.gr_offset); if (!p) return p.error(); address = *p; va.gr_offset += 8; }
    else { auto p = ld && ld_size == 16 ? align(va.stack,16) : Result<std::uintptr_t>(va.stack); if (!p) return p.error(); address = *p;
      auto next = add(address,ld ? ld_size : 8); if (!next) return next.error(); va.stack = *next; }
    if (!address) return invalid("null variadic argument address"); write(state.data(),va);
  } else {
    auto va = read<PointerState>(state.data()); address = va.arguments; if (!address) return invalid("null variadic argument address");
    auto next = add(address,abi == target::ABI::AArch64MacOS && ld ? ld_size : 8); if (!next) return next.error(); va.arguments = *next; write(state.data(),va);
  }
  return address;
}
Result<void> advance_variadic_block(target::ABI abi,std::span<std::byte> state,std::span<std::byte> destination,std::size_t size,ir::Type::BlockClass category) {
  auto valid = validate(abi,state); if (!valid) return valid;
  auto classification = static_cast<unsigned>(category);
  if (classification > 4 || size > SIZE_MAX - 7 || (!destination.empty() && destination.size() < size)) return invalid("invalid block variadic metadata");
  if (!size) return {}; auto rounded = (size + 7)/8*8;
  if (abi == target::ABI::X86SysV) {
    if (classification && rounded > 16) return invalid("SysV register block exceeds two words");
    auto va = read<SysVState>(state.data()); std::array<std::uint64_t,2> words{};
    unsigned slots = classification ? rounded/8 : 0;
    unsigned gp = classification == 1 ? slots : classification == 3 ? 1u : classification == 4 && slots > 1 ? 1u : 0u;
    unsigned fp = classification == 2 ? slots : classification == 4 ? 1u : classification == 3 && slots > 1 ? 1u : 0u;
    if (classification && (!gp || va.gp_offset <= 48-gp*8) && (!fp || va.fp_offset <= 176-fp*16)) {
      if (!va.registers) return invalid("null variadic register save area");
      for (unsigned word = 0; word < slots; ++word) {
        bool floating = classification == 2 || (classification == 3 && word == 1) || (classification == 4 && word == 0);
        auto p = add(va.registers,floating ? va.fp_offset : va.gp_offset); if (!p) return p.error(); if (!*p) return invalid("null variadic save area");
        words[word] = read<std::uint64_t>(reinterpret_cast<const std::byte *>(*p)); if (floating) va.fp_offset += 16; else va.gp_offset += 8;
      }
      if (!destination.empty()) std::memcpy(destination.data(),words.data(),size);
    } else { auto next = add(va.stack,rounded); if (!next) return next.error(); auto copied = copy(destination,va.stack,size); if (!copied) return copied; va.stack = *next; }
    write(state.data(),va); return {};
  }
  if (abi == target::ABI::AArch64Linux) {
    auto va = read<ArmState>(state.data()); std::uintptr_t address; auto consumed = size > 16 ? std::size_t{8} : rounded;
    if (size <= 16 && static_cast<std::int64_t>(va.gr_offset) + static_cast<std::int64_t>(consumed) > 0) {
      if (va.gr_offset > INT32_MAX - static_cast<std::int32_t>(consumed)) return invalid("variadic register offset overflow");
      address = va.stack; auto next = add(address,consumed); if (!next) return next.error(); va.stack = *next; va.gr_offset += static_cast<std::int32_t>(consumed);
    } else if (va.gr_offset < 0) { auto p = saved(va.gr_top,va.gr_offset); if (!p) return p.error(); address = *p; va.gr_offset += static_cast<std::int32_t>(consumed); }
    else { address = va.stack; auto next = add(address,consumed); if (!next) return next.error(); va.stack = *next; }
    if (size > 16) { if (!address) return invalid("null indirect variadic block"); address = read<std::uintptr_t>(reinterpret_cast<const std::byte *>(address)); }
    auto copied = copy(destination,address,size); if (!copied) return copied; write(state.data(),va); return {};
  }
  auto va = read<PointerState>(state.data()); auto address = va.arguments; bool indirect = size > (abi == target::ABI::AArch64MacOS ? 16u : 8u);
  auto next = add(address,indirect ? 8 : rounded); if (!next) return next.error();
  if (indirect) { if (!address) return invalid("null indirect variadic block"); address = read<std::uintptr_t>(reinterpret_cast<const std::byte *>(address)); }
  auto copied = copy(destination,address,size); if (!copied) return copied; va.arguments = *next; write(state.data(),va); return {};
}
Result<void> initialize_variadic_state(target::ABI abi,std::span<std::byte> destination,std::span<const std::byte> source) {
  auto valid = validate(abi,destination); if (!valid) return valid;
  auto size = variadic_state_size(abi); if (!source.data() || source.size() < size) return invalid("invalid variadic source"); std::memmove(destination.data(),source.data(),size); return {};
}
Result<std::uintptr_t> builtin_address(codegen::Builtin id) {
  using codegen::Builtin;
#define ADDRESS(name, function) case Builtin::name: return reinterpret_cast<std::uintptr_t>(&function)
  switch (id) {
    ADDRESS(CopyBlock,block_move); ADDRESS(CopyArgument,argument_copy); ADDRESS(VariadicScalar,va_argument); ADDRESS(VariadicBlock,va_block);
    ADDRESS(FloatToLongDouble,f2ld); ADDRESS(DoubleToLongDouble,d2ld); ADDRESS(IntegerToLongDouble,i2ld); ADDRESS(UnsignedToLongDouble,ui2ld);
    ADDRESS(UnsignedToFloat,ui2f); ADDRESS(UnsignedToDouble,ui2d); ADDRESS(LongDoubleToFloat,ld2f); ADDRESS(LongDoubleToDouble,ld2d); ADDRESS(LongDoubleToInteger,ld2i);
    ADDRESS(LongDoubleAdd,ldadd); ADDRESS(LongDoubleSub,ldsub); ADDRESS(LongDoubleMul,ldmul); ADDRESS(LongDoubleDiv,lddiv); ADDRESS(LongDoubleNeg,ldneg);
    ADDRESS(LongDoubleEq,ldeq); ADDRESS(LongDoubleNe,ldne); ADDRESS(LongDoubleLt,ldlt); ADDRESS(LongDoubleLe,ldle); ADDRESS(LongDoubleGt,ldgt); ADDRESS(LongDoubleGe,ldge);
  }
#undef ADDRESS
  return Error{ErrorCode::UnknownName,"builtin: unknown typed helper"};
}
}
