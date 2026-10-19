/* Target rules derived from MIR, Copyright (C) 2018-2024 Vladimir Makarov.
   Licensed under the MIT License. Direct lowering of owning machine IR. */
#include "mirnext/CodeGen/Detail/TargetLowering.hpp"
#include "mirnext/Target/X86ABI.hpp"
#include <algorithm>
#include <limits>

namespace mirnext::codegen {
namespace {
using ir::Type; using ir::Opcode; using K = Type::Kind; using R = target::X86Register;
using Operand = MachineOperand; using Instruction = MachineInstruction;
Opcode move(Type t) { return t.kind() == K::F32 ? Opcode::FMov : t.kind() == K::F64 ? Opcode::DMov : t.kind() == K::LongDouble ? Opcode::LDMov : Opcode::Mov; }
std::optional<Opcode> extension(Type t) {
  switch (t.kind()) {
  case K::I8: return Opcode::Ext8; case K::U8: return Opcode::UExt8;
  case K::I16: return Opcode::Ext16; case K::U16: return Opcode::UExt16;
  case K::I32: return Opcode::Ext32; case K::U32: return Opcode::UExt32;
  default: return {};
  }
}
std::optional<MachineRegister> register_value(const Operand &o) {
  if (auto v = o.get<const MachineVirtualRegister *>()) return *v;
  if (auto p = o.get<target::PhysicalRegister>()) return *p;
  return {};
}
std::int64_t variadic_tag(Type t) {
  switch (t.kind()) {
  case K::I8: return 0; case K::U8: return 1; case K::I16: return 2; case K::U16: return 3;
  case K::I32: return 4; case K::U32: return 5; case K::U64: return 7; case K::F32: return 8;
  case K::F64: return 9; case K::LongDouble: return 10; case K::Pointer: return 11; default: return 6;
  }
}
struct BuiltinDescription { Builtin id; std::string_view name; ir::Signature signature; };
std::optional<BuiltinDescription> builtin(Opcode code) {
  auto i = Type::i64(), f = Type(K::F32), d = Type(K::F64), ld = Type(K::LongDouble);
  switch (code) {
  case Opcode::UI2F: return BuiltinDescription{Builtin::UnsignedToFloat,"mir.ui2f",{{f},{i}}};
  case Opcode::UI2D: return BuiltinDescription{Builtin::UnsignedToDouble,"mir.ui2d",{{d},{i}}};
  case Opcode::UI2LD: return BuiltinDescription{Builtin::UnsignedToLongDouble,"mir.ui2ld",{{ld},{i}}};
  case Opcode::LD2I: return BuiltinDescription{Builtin::LongDoubleToInteger,"mir.ld2i",{{i},{ld}}};
  case Opcode::VaArg: return BuiltinDescription{Builtin::VariadicScalar,"mir.va_arg",{{i},{i,i}}};
  case Opcode::VaBlockArg: return BuiltinDescription{Builtin::VariadicBlock,"mir.va_block_arg",{{},{i,i,i,i}}};
  default: return {};
  }
}
class X86Lowering {
public:
  explicit X86Lowering(MachineFunction &function) : f(function), rw(f), state(f.state()), abi(f.target().abi()), win(abi == target::ABI::X86Windows) {}
  Result<void> run();
private:
  MachineFunction &f; MachineRewriter rw; MachineFunctionState state; target::ABI abi; bool win;
  std::optional<Error> error;
  void fail(std::string why) { if (!error) error = Error{ErrorCode::InvalidOperand,std::string(f.module_name()) + '/' + std::string(f.name()) + " [" + std::string(f.target().name()) + ", machinize]: " + std::move(why)}; }
  void checked(Result<void> result) { if (!result) fail(result.error().message); }
  Operand integer(std::size_t n) { return Operand(static_cast<std::int64_t>(n)); }
  target::PhysicalRegister physical_register(R n) { return f.target().registers()[static_cast<unsigned>(n)].reg; }
  Operand physical(R n) { return Operand(physical_register(n)); }
  Operand temporary(Type t = Type::i64()) {
    if (error) return Operand(std::int64_t(0));
    auto reg = f.create_register(t); if (!reg) { fail(reg.error().message); return Operand(std::int64_t(0)); } return Operand(**reg);
  }
  Instruction *before(Instruction &at, MachineOpcode code, std::vector<Operand> operands) {
    if (error) return &at; auto result = rw.insert_before(at,code,std::move(operands));
    if (!result) { fail(result.error().message); return &at; } return *result;
  }
  Instruction *after(Instruction &at, MachineOpcode code, std::vector<Operand> operands) {
    if (error) return &at; auto result = rw.insert_after(at,code,std::move(operands));
    if (!result) { fail(result.error().message); return &at; } return *result;
  }
  void replace(Instruction &at, MachineOpcode code, std::vector<Operand> operands) { if (!error) checked(rw.replace(at,code,std::move(operands))); }
  void erase(Instruction &at) { if (!error) checked(rw.erase(at)); }
  Operand memory(Type t, std::int64_t offset, const Operand &base) {
    auto reg = register_value(base); if (!reg) { fail("memory base must be a register"); return integer(0); }
    return Operand(MachineMemory{t,*reg,{},offset,1,{},{}});
  }
  Operand block_marker(const Operand &base, std::int64_t offset = 0, Type t = Type(K::Undef)) { return memory(t,offset,base); }
  Operand address(const MachineMemory &m) {
    if (!m.base) { fail("block argument needs a base register"); return integer(0); }
    return std::visit([](auto value) { return Operand(value); },*m.base);
  }
  const MachineSymbol *prototype(const BuiltinDescription &b) {
    auto p = f.intern_symbol({"$builtin",std::string(b.name) + ".p",MachineSymbol::Kind::BuiltinPrototype,b.signature,false,{},b.id});
    if (!p) { fail(p.error().message); return nullptr; } return *p;
  }
  Type argument_type(const ir::Signature &,std::size_t,const Operand &) const;
  Result<target::X86ArgumentLayout> call_layout(const Instruction &) const;
  Result<void> preflight();
  void parameters(Instruction &);
  void lower_call(Instruction &);
  void lower_return(Instruction &);
  void lower_builtin(Instruction &,Opcode,const BuiltinDescription &);
  void lower_variadic_start(Instruction &);
};
Type X86Lowering::argument_type(const ir::Signature &signature,std::size_t n,const Operand &o) const {
  if (n < signature.parameters.size()) return signature.parameters[n];
  if (auto m = o.get<MachineMemory>()) return m->type;
  if (auto r = o.get<const MachineVirtualRegister *>()) return (*r)->type();
  if (auto p = o.get<target::PhysicalRegister>()) return f.target().register_info(*p)->bank == target::RegisterBank::Integer ? Type::i64() : Type(K::F64);
  return Type::i64();
}
Result<target::X86ArgumentLayout> X86Lowering::call_layout(const Instruction &at) const {
  auto &s = *at.operands()[0].get<MachineSymbolReference>()->symbol->signature;
  std::vector<Type> types;
  for (std::size_t n = 2 + s.results.size(); n < at.operands().size(); ++n) types.push_back(argument_type(s,n - 2 - s.results.size(),at.operands()[n]));
  return target::layout_x86_arguments(abi,types);
}
Result<void> X86Lowering::preflight() {
  auto valid = f.verify(); if (!valid) { fail(valid.error().message); return *error; }
  if (state.machinized) { fail("machine function is already lowered"); return *error; }
  auto args = target::layout_x86_arguments(abi,f.signature().parameters); if (!args) fail(args.error().message);
  auto results = target::layout_x86_results(abi,f.signature().results); if (!results) fail(results.error().message);
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) {
    auto code = i.opcode(); auto op = std::get_if<Opcode>(&code); if (!op) continue;
    if (*op == Opcode::Call || *op == Opcode::Inline || *op == Opcode::JCall) {
      if (i.has_lowered_call()) { fail("call is already lowered"); continue; }
      auto &s = *i.operands()[0].get<MachineSymbolReference>()->symbol->signature;
      auto returns = target::layout_x86_results(abi,s.results); if (!returns) fail(returns.error().message);
      auto layout = call_layout(i); if (!layout) { fail(layout.error().message); continue; }
      std::size_t copy_bytes = win ? std::max<std::size_t>(32,layout->arguments.size() * 8) : 0;
      for (std::size_t n = 2 + s.results.size(); n < i.operands().size(); ++n) {
        auto &o = i.operands()[n]; auto type = argument_type(s,n - 2 - s.results.size(),o);
        if (type.is_block()) {
          auto m = o.get<MachineMemory>();
          if (!m || !m->base || m->index || m->displacement) fail("block arguments require a base register without an offset or index");
          if (win && type.kind() == K::Block && type.block_size() > 8) {
            auto size = (type.block_size() + 7) / 8 * 8;
            if (size > std::size_t(INT64_MAX) - 63 - copy_bytes) fail("argument copy area overflow"); else copy_bytes += size;
          }
        } else if (!register_value(o)) fail("call scalar operands must be simplified to registers");
        if (n - 2 - s.results.size() >= s.parameters.size() && type.kind() == K::F32) fail("passing float variadic argument (must be promoted to double)");
      }
    }
    if (*op == Opcode::VaStart && !f.signature().variadic) fail("va_start requires a variadic function");
    if (*op == Opcode::Ret) for (auto &o : i.operands()) if (!register_value(o)) fail("return operands must be simplified to registers");
  }
  return error ? Result<void>(*error) : Result<void>();
}
void X86Lowering::parameters(Instruction &anchor) {
  auto layout = target::layout_x86_arguments(abi,f.signature().parameters); if (!layout) { fail(layout.error().message); return; }
  Instruction *front = &anchor;
  for (std::size_t n = 0; n < layout->arguments.size() && !error; ++n) {
    auto &loc = layout->arguments[n]; auto type = f.signature().parameters[n]; auto arg = Operand(*f.arguments()[n]);
    Instruction *first = nullptr;
    auto emit = [&](Opcode code,std::vector<Operand> operands) { auto i = before(*front,code,std::move(operands)); if (!first) first = i; };
    if (loc.aggregate && !win && loc.register_count) {
      emit(Opcode::Alloca,{arg,integer(loc.size)});
      for (unsigned p = 0; p < loc.register_count; ++p) {
        auto scalar = static_cast<unsigned>(loc.registers[p]) >= static_cast<unsigned>(R::XMM0) ? Type(K::F64) : Type::i64();
        emit(move(scalar),{memory(scalar,p * 8,arg),physical(loc.registers[p])});
      }
    } else if (loc.aggregate && loc.by_value) {
      state.requires_frame_pointer = state.has_block_arguments = true;
      auto offset = loc.stack_offset + 16;
      if (loc.register_count) {
        // Windows incoming home slot: BP + 16 + argument slot * 8.
        offset = 16 + n * 8;
        emit(Opcode::Mov,{memory(Type::i64(),offset,physical(R::BP)),physical(loc.registers[0])});
      }
      emit(Opcode::Add,{arg,physical(R::BP),integer(offset)});
    } else if (loc.register_count) emit(move(type),{arg,physical(loc.registers[0])});
    else {
      state.requires_frame_pointer = state.has_block_arguments = true;
      auto scalar = type.is_float() ? type : Type::i64();
      emit(move(scalar),{arg,memory(scalar,loc.stack_offset + 16,physical(R::BP))});
    }
    if (first) front = first; // The C prologue prepends argument groups.
  }
}
void X86Lowering::lower_call(Instruction &at) {
  auto &s = *at.operands()[0].get<MachineSymbolReference>()->symbol->signature;
  auto layout = call_layout(at); if (!layout) { fail(layout.error().message); return; }
  auto results = target::layout_x86_results(abi,s.results); if (!results) { fail(results.error().message); return; }
  std::vector<Operand> ops(at.operands().begin(),at.operands().end()); auto start = 2 + s.results.size();
  auto boundary = before(at,MachinePseudoOpcode::Use,{});
  std::vector<target::PhysicalRegister> extra;
  auto add_extra = [&](R r) { auto p = physical_register(r); if (std::find(extra.begin(),extra.end(),p) == extra.end()) extra.push_back(p); };
  auto early = [&](Opcode code,std::vector<Operand> args) { return after(*boundary,code,std::move(args)); };
  auto stack_bytes = layout->stack_bytes;
  auto copy_offset = win ? std::max<std::size_t>(32,layout->arguments.size() * 8) : 0;
  if (!register_value(ops[1]) && !ops[1].get<MachineSymbolReference>() && !ops[1].get<BuiltinReference>()) {
    auto target = temporary(); before(at,Opcode::Mov,{target,ops[1]}); ops[1] = target;
  }
  for (std::size_t n = 0; n < layout->arguments.size() && !error; ++n) {
    auto &loc = layout->arguments[n]; auto type = argument_type(s,n,ops[start + n]); auto source = ops[start + n];
    std::optional<std::pair<Opcode,Operand>> extend;
    if (auto code = extension(type)) { auto t = temporary(); extend = std::pair{*code,source}; source = t; }
    auto emit_extension = [&](bool before_call) {
      if (extend) {
        if (before_call) before(at,extend->first,{source,extend->second});
        else early(extend->first,{source,extend->second});
      }
    };
    if (loc.aggregate && !win && loc.register_count) {
      auto base = address(*source.get<MachineMemory>());
      for (unsigned p = 0; p < loc.register_count; ++p) {
        auto scalar = static_cast<unsigned>(loc.registers[p]) >= static_cast<unsigned>(R::XMM0) ? Type(K::F64) : Type::i64();
        before(at,move(scalar),{physical(loc.registers[p]),memory(scalar,p * 8,base)}); add_extra(loc.registers[p]);
      }
      auto marker = *source.get<MachineMemory>(); marker.type = Type(K::Undef); marker.base.reset();
      marker.displacement = static_cast<std::int64_t>(type.block_size()); ops[start + n] = Operand(std::move(marker));
      continue;
    }
    if (loc.aggregate) {
      auto base = address(*source.get<MachineMemory>());
      if (win && loc.by_value) {
        auto t = temporary(); early(Opcode::Mov,{t,memory(Type::i64(),0,base)}); source = t;
      } else {
        auto destination = win ? copy_offset : loc.stack_offset;
        if (win) copy_offset += loc.size;
        if (loc.size <= 16) {
          auto t = temporary();
          for (std::size_t offset = 0; offset < loc.size; offset += 8) {
            auto load = early(Opcode::Mov,{t,memory(Type::i64(),offset,base)});
            after(*load,Opcode::Mov,{memory(Type::i64(),destination + offset,physical(R::SP)),t});
          }
          ops[start + n] = block_marker(physical(R::SP),destination);
          if (win) { source = temporary(); before(at,Opcode::Add,{source,physical(R::SP),integer(destination)}); }
        } else {
          auto callee = temporary(), dest = temporary();
          BuiltinDescription b{Builtin::CopyArgument,"mir.arg_memcpy",{{},{Type::i64(),Type::i64(),Type::i64()}}};
          auto proto = prototype(b); if (!proto) return;
          auto r0 = *target::x86_integer_argument_register(abi,0), r1 = *target::x86_integer_argument_register(abi,1), r2 = *target::x86_integer_argument_register(abi,2);
          auto call = early(Opcode::Call,{Operand(MachineSymbolReference{proto}),callee,physical(r0),physical(r1),physical(r2)});
          early(Opcode::Mov,{physical(r2),integer(loc.size)}); early(Opcode::Mov,{physical(r1),base}); early(Opcode::Mov,{physical(r0),dest});
          early(Opcode::Add,{dest,physical(R::SP),integer(destination)}); early(Opcode::Mov,{callee,Operand(BuiltinReference{b.id})});
          if (!error) checked(rw.lower_call(*call,Opcode::Call,std::vector<Operand>(call->operands().begin(),call->operands().end())));
          // Keep the copied block alive until the outer call, with an explicit
          // typed location instead of an unallocated pseudo in a public record.
          ops[start + n] = block_marker(dest,static_cast<std::int64_t>(type.block_size()));
          if (win) source = dest;
        }
      }
      if (!win) continue;
    }
    if (loc.register_count) {
      emit_extension(true); auto reg = physical(loc.registers[0]);
      if (type.kind() == K::ReturnBlock) {
        auto base = address(*source.get<MachineMemory>()); before(at,Opcode::Mov,{reg,base});
        ops[start + n] = block_marker(reg,static_cast<std::int64_t>(type.block_size()),type);
      } else {
        auto scalar = loc.aggregate ? Type::i64() : type;
        before(at,move(scalar),{reg,source}); ops[start + n] = reg;
      }
      if (win && s.variadic && type.kind() == K::F64) {
        auto slot = static_cast<unsigned>(loc.registers[0]) - static_cast<unsigned>(R::XMM0);
        auto integer_reg = *target::x86_integer_argument_register(abi,slot); add_extra(integer_reg);
        before(at,Opcode::DMov,{memory(Type(K::F64),8,physical(R::SP)),source});
        before(at,Opcode::Mov,{physical(integer_reg),memory(Type::i64(),8,physical(R::SP))});
      }
    } else {
      if (type.kind() == K::ReturnBlock) source = address(*source.get<MachineMemory>());
      auto scalar = type.is_float() ? type : Type::i64(); auto dest = memory(scalar,loc.stack_offset,physical(R::SP));
      early(move(scalar),{dest,source}); emit_extension(false); ops[start + n] = dest;
    }
  }
  if (!win && s.variadic) { add_extra(R::AX); before(at,Opcode::Mov,{physical(R::AX),integer(layout->variadic_xmm_count)}); }
  // Each insert-after is anchored at the call, matching reverse result order.
  for (std::size_t n = 0; n < s.results.size() && !error; ++n) {
    auto destination = ops[2 + n], source = physical((*results)[n]);
    auto copy = after(at,move(s.results[n]),{destination,source}); ops[2 + n] = source;
    if (auto ext = extension(s.results[n])) after(*copy,*ext,{destination,destination});
  }
  stack_bytes = std::max(stack_bytes,copy_offset);
  if (stack_bytes) {
    stack_bytes = (stack_bytes + 15) / 16 * 16;
    early(Opcode::Sub,{physical(R::SP),physical(R::SP),integer(stack_bytes)});
    after(at,Opcode::Add,{physical(R::SP),physical(R::SP),integer(stack_bytes)});
    state.requires_frame_pointer = true;
  }
  auto code = std::get<Opcode>(at.opcode()); if (code == Opcode::Inline) code = Opcode::Call;
  if (!error) checked(rw.lower_call(at,code,std::move(ops)));
  std::sort(extra.begin(),extra.end(),[](auto a,auto b) { return a.encoding() < b.encoding(); });
  if (!error) checked(rw.set_call_arguments(at,extra));
  erase(*boundary); state.is_leaf = false;
}
void X86Lowering::lower_return(Instruction &at) {
  auto regs = target::layout_x86_results(abi,f.signature().results); if (!regs) { fail(regs.error().message); return; }
  std::vector<Operand> ops(at.operands().begin(),at.operands().end());
  for (std::size_t n = 0; n < ops.size(); ++n) { auto r = physical((*regs)[n]); before(at,move(f.signature().results[n]),{r,ops[n]}); ops[n] = r; }
  replace(at,Opcode::Ret,std::move(ops));
}
void X86Lowering::lower_builtin(Instruction &at,Opcode code,const BuiltinDescription &b) {
  auto proto = prototype(b); if (!proto) return;
  auto ops = std::vector<Operand>(at.operands().begin(),at.operands().end()); auto callee = temporary();
  before(at,Opcode::Mov,{callee,Operand(BuiltinReference{b.id})});
  std::vector<Operand> args{Operand(MachineSymbolReference{proto}),callee};
  if (code == Opcode::VaArg) {
    auto tag = temporary(); before(at,Opcode::Mov,{tag,Operand(variadic_tag(ops[2].get<MachineMemory>()->type))});
    args.insert(args.end(),{ops[0],ops[1],tag});
  } else args.insert(args.end(),ops.begin(),ops.end());
  auto call = before(at,Opcode::Call,std::move(args)); erase(at);
  if (!error) lower_call(*call);
}
void X86Lowering::lower_variadic_start(Instruction &at) {
  auto temp = temporary(), va = at.operands()[0];
  if (win) {
    before(at,Opcode::Add,{temp,physical(R::BP),integer(16 + f.arguments().size() * 8)});
    before(at,Opcode::Mov,{memory(Type::i64(),0,va),temp});
  } else {
    std::size_t gp = 0, fp = 48, overflow = 0;
    // Preserve the original MIR cursor calculation, independently of argument
    // assignment. Corrections require a separately demonstrated reference defect.
    for (auto type : f.signature().parameters) {
      if (type.kind() == K::F32 || type.kind() == K::F64) { fp += 16; if (gp >= 176) overflow += 8; }
      else if (type.kind() == K::LongDouble) overflow += 16;
      else if (type.kind() == K::Block) overflow += type.block_size();
      else { gp += 8; if (gp >= 48) overflow += 8; }
    }
    before(at,Opcode::Mov,{memory(Type(K::U32),0,va),integer(gp)});
    before(at,Opcode::Mov,{memory(Type(K::U32),4,va),integer(fp)});
    before(at,Opcode::Add,{temp,physical(R::BP),integer(16 + overflow)});
    before(at,Opcode::Mov,{memory(Type::i64(),8,va),temp});
    before(at,Opcode::Add,{temp,physical(R::BP),Operand(std::int64_t(-176))});
    before(at,Opcode::Mov,{memory(Type::i64(),16,va),temp});
  }
  erase(at);
}
Result<void> X86Lowering::run() {
  auto valid = preflight(); if (!valid) return valid;
  state.has_block_arguments = false; state.requires_frame_pointer = f.signature().variadic;
  std::get<MachineFunctionState::X86>(state.target).initial_sp_from_bp = 8;
  std::get<MachineFunctionState::X86>(state.target).variadic_save_bytes = !win && f.signature().variadic ? 176 : 0;
  auto &entry = *f.blocks().begin(); auto anchor = rw.append(entry,MachinePseudoOpcode::Use,{}); if (!anchor) return anchor.error();
  checked(rw.set_entry_setup(**anchor));
  auto first = &*entry.instructions().begin(); if (first != *anchor) checked(rw.move_before(**anchor,*first));
  parameters(**anchor); erase(**anchor); if (error) return *error;
  state.uses_alloca = false; state.is_leaf = true;
  std::vector<Instruction *> work; work.reserve(f.instruction_count());
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) work.push_back(&i);
  for (auto at : work) {
    if (error) break; auto machine_code = at->opcode(); auto op = std::get_if<Opcode>(&machine_code); if (!op) continue; auto code = *op;
    if (auto b = builtin(code)) { lower_builtin(*at,code,*b); continue; }
    if (code == Opcode::Call || code == Opcode::Inline || code == Opcode::JCall) { lower_call(*at); continue; }
    if (code == Opcode::VaStart) { lower_variadic_start(*at); continue; }
    if (code == Opcode::VaEnd) { erase(*at); continue; }
    if (code == Opcode::Alloca) { state.requires_frame_pointer = state.uses_alloca = true; continue; }
    if (code == Opcode::Ret) { lower_return(*at); continue; }
    std::vector<Operand> ops(at->operands().begin(),at->operands().end());
    switch (code) {
    case Opcode::Lsh: case Opcode::Rsh: case Opcode::URsh: case Opcode::Lshs: case Opcode::Rshs: case Opcode::URshs:
      before(*at,Opcode::Mov,{physical(R::CX),ops[2]}); ops[2] = physical(R::CX); replace(*at,code,std::move(ops)); break;
    case Opcode::UMulo: case Opcode::UMulos: case Opcode::Div: case Opcode::UDiv: case Opcode::Divs: case Opcode::UDivs:
    case Opcode::Mod: case Opcode::UMod: case Opcode::Mods: case Opcode::UMods: {
      auto result = code == Opcode::Mod || code == Opcode::UMod || code == Opcode::Mods || code == Opcode::UMods ? R::DX : R::AX;
      before(*at,Opcode::Mov,{physical(R::AX),ops[1]}); after(*at,Opcode::Mov,{ops[0],physical(result)});
      ops[0] = physical(result); ops[1] = physical(R::AX); replace(*at,code,std::move(ops)); break;
    }
    default: {
      auto desc = ir::instruction_descriptor(code);
      if (desc->roles == "Iii" || desc->roles == "Iff" || desc->roles == "Idd") {
        const bool comparison = code >= Opcode::Eq && code <= Opcode::DGe;
        if (comparison) after(*at,Opcode::UExt8,{ops[0],ops[0]});
      }
      std::optional<Opcode> reversed;
      switch (code) {
      case Opcode::FLt: reversed = Opcode::FGt; break; case Opcode::FLe: reversed = Opcode::FGe; break;
      case Opcode::DLt: reversed = Opcode::DGt; break; case Opcode::DLe: reversed = Opcode::DGe; break;
      case Opcode::LDLt: reversed = Opcode::LDGt; break; case Opcode::LDLe: reversed = Opcode::LDGe; break;
      case Opcode::FBlt: reversed = Opcode::FBgt; break; case Opcode::FBle: reversed = Opcode::FBge; break;
      case Opcode::DBlt: reversed = Opcode::DBgt; break; case Opcode::DBle: reversed = Opcode::DBge; break;
      case Opcode::LDBlt: reversed = Opcode::LDBgt; break; case Opcode::LDBle: reversed = Opcode::LDBge; break;
      default: break;
      }
      if (reversed) { std::swap(ops[1],ops[2]); replace(*at,*reversed,std::move(ops)); }
      break;
    }
    }
  }
  if (error) return *error;
  state.machinized = true; checked(rw.set_state(state)); if (error) return *error; return f.verify();
}
}
Result<void> machinize_x86(MachineFunction &f) {
  if (f.target().architecture() != target::Architecture::X86_64) return Error{ErrorCode::InvalidArgument,"x86 lowering requires an x86 target"};
  return X86Lowering(f).run();
}
}
