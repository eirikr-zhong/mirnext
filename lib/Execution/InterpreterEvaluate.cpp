/* Licensed under the MIT License. Canonical IR evaluation; no C dispatch. */
#include "mirnext/Execution/detail/LoadedProgram.hpp"
#include "mirnext/Execution/Builtins.hpp"
#include <bit>
namespace mirnext::execution::detail {
namespace {
using O = ir::Opcode;
Error invalid(std::string reason) { return {ErrorCode::InvalidOperand,std::move(reason)}; }
ir::Type role_type(char c) {
  using K = ir::Type::Kind; return ir::Type(c == 'f' ? K::F32 : c == 'd' ? K::F64 : c == 'e' ? K::LongDouble : K::I64);
}
bool inside(const InterpreterInvocation &invocation,std::uintptr_t address,std::size_t size) {
  for (auto area : invocation.variadic_areas) {
    auto begin = reinterpret_cast<std::uintptr_t>(area.data());
    if (address >= begin && address-begin <= area.size() && size <= area.size()-(address-begin)) return true;
  } return false;
}
}
Result<InterpreterOutcome> InterpreterFrame::run() {
  auto &state = invocation.state; std::size_t pc = 0;
  while (pc < body.operations.size()) {
    auto current = pc++; auto &operation = body.operations[current]; auto &i = *operation.source;
    auto op = i.opcode(); auto &p = operation.operands; auto descriptor = operation.descriptor;
    auto failure = [&](Error e) -> Result<InterpreterOutcome> {
      e.message = "instruction " + std::to_string(current) + " (" + std::string(descriptor->name) + "): " + e.message; return e;
    };
    if (state.options.instruction_limit) {
      if (invocation.steps >= state.options.instruction_limit) return failure(invalid("interpreter instruction limit exceeded"));
      ++invocation.steps;
    }
    auto integer = [&](std::size_t n) -> Result<std::uint64_t> {
      auto v = read(p[n],ir::Type::i64()); if (!v) return v.error(); return v->as_u64();
    };
    // All scalar arithmetic, conversions, comparisons and direct comparison
    // branches share typed semantics with separately checked input reads.
    if (operation.scalar_inputs) {
      std::array<ScalarValue,2> args; auto count = operation.scalar_inputs;
      for (std::size_t n = 0; n < count; ++n) {
        auto v = read(p[n+1],role_type(descriptor->roles[n+1])); if (!v) return failure(v.error()); args[n] = std::move(*v);
      }
      auto value = evaluate_scalar(op,std::span(args).first(count),overflow); if (!value) return failure(value.error());
      if (*value) {
        if (descriptor->flow == ir::ControlFlow::Branch) { auto condition = (**value).as_u64(); if (!condition) return failure(condition.error()); if (*condition) pc = p[0].pc; }
        else { auto assigned = write(p[0],**value); if (!assigned) return failure(assigned.error()); }
        continue;
      }
    }
    switch (op) {
      case O::Label: case O::PrSet: case O::VaEnd: break;
      case O::Jmp: pc = p[0].pc; break;
      case O::Bo: if (overflow.signed_value) pc = p[0].pc; break;
      case O::Bno: if (!overflow.signed_value) pc = p[0].pc; break;
      case O::UBo: if (overflow.unsigned_value) pc = p[0].pc; break;
      case O::UBno: if (!overflow.unsigned_value) pc = p[0].pc; break;
      case O::PrBeq: case O::PrBne: {
        auto value = p[2].source->get<std::int64_t>();
        if (value && (op == O::PrBeq ? *value == 0 : *value != 0)) pc = p[0].pc; break;
      }
      case O::LAddr: {
        auto address = reinterpret_cast<std::uintptr_t>(body.operations.data()+p[1].pc);
        auto saved = write(p[0],ScalarValue::pointer(address)); if (!saved) return failure(saved.error()); break;
      }
      case O::JmpIndirect: {
        auto address = integer(0); if (!address) return failure(address.error()); auto found = state.locations.find(*address);
        if (found == state.locations.end() || found->second.body != &body) return failure(invalid("indirect jump does not target a label in this function"));
        pc = found->second.pc; break;
      }
      case O::Switch: {
        auto index = integer(0); if (!index) return failure(index.error());
        if (*index >= p.size()-1) return failure(invalid("switch index is outside its table")); pc = p[*index+1].pc; break;
      }
      case O::Addr: case O::Addr8: case O::Addr16: case O::Addr32: {
        auto address = reinterpret_cast<std::uintptr_t>(registers[p[1].reg]->bytes.data());
        if constexpr (std::endian::native == std::endian::big) {
          if (op != O::Addr) address += 8-(op == O::Addr8 ? 1 : op == O::Addr16 ? 2 : 4);
        }
        auto saved = write(p[0],ScalarValue::pointer(address)); if (!saved) return failure(saved.error()); break;
      }
      case O::Ret: {
        InterpreterOutcome result;
        for (std::size_t n = 0; n < p.size(); ++n) { auto value = read(p[n],body.source->signature().results[n]); if (!value) return failure(value.error()); result.values.push_back(*value); }
        return result;
      }
      case O::JRet: { auto address = integer(0); if (!address) return failure(address.error()); return InterpreterOutcome{{},*address}; }
      case O::Call: case O::Inline: case O::JCall: {
        auto ref = p[0].source->get<ir::SymbolReference>(); auto &signature = *ref->symbol->signature(); auto nres = signature.results.size();
        auto target = integer(1); if (!target) return failure(target.error()); auto found = state.callees.find(*target);
        std::optional<std::size_t> callee;
        if (found != state.callees.end()) {
          for (auto index : found->second) { auto &entry = state.binding(index); if (entry.signature && *entry.signature == signature && entry.jump == (op == O::JCall)) { callee = index; break; } }
          if (!callee) return failure(invalid("indirect call signature or convention mismatch"));
        } else if (op == O::JCall) return failure(invalid("external jcall requires a nonstandard ABI entry; no native adapter is available"));
        std::vector<ExecutionArgument> args;
        for (std::size_t n = nres+2; n < p.size(); ++n) {
          if (auto memory = p[n].source->get<ir::Memory>(); memory && memory->type.is_block()) {
            auto a = address(p[n]); if (!a) return failure(a.error()); if (!*a) return failure(invalid("null block argument"));
            auto size = memory->type.block_size();
            if (memory->type.kind() == ir::Type::Kind::ReturnBlock) {
              auto writable = state.writable(*a,size); if (!writable) return failure(writable.error());
              args.push_back(ExecutionArgument::return_buffer({reinterpret_cast<std::byte *>(*a),size}));
            } else args.push_back(ExecutionArgument::block({reinterpret_cast<const std::byte *>(*a),size},memory->type.block_class()));
          } else {
            auto argument = n-nres-2; ir::Type type = ir::Type::i64();
            if (argument < signature.parameters.size()) type = signature.parameters[argument];
            else if (p[n].reg != no_slot) type = body.registers[p[n].reg]->type();
            else if (p[n].source->get<double>()) type = ir::Type(ir::Type::Kind::F64);
            else if (p[n].source->get<long double>()) type = ir::Type(ir::Type::Kind::LongDouble);
            else if (p[n].source->get<float>()) type = ir::Type(ir::Type::Kind::F32);
            auto v = read(p[n],type); if (!v) return failure(v.error()); args.emplace_back(*v);
          }
        }
        auto called = callee ? invocation.invoke(*callee,args,op == O::JCall) : invocation.invoke_foreign(*target,signature,args);
        if (!called) return failure(called.error());
        if (op == O::JCall) {
          auto next = state.locations.find(*called->jump);
          if (next == state.locations.end() || next->second.body != &body) return failure(invalid("jret does not target a label in its caller"));
          pc = next->second.pc;
        } else for (std::size_t n = 0; n < nres; ++n) { auto saved = write(p[n+2],called->values[n]); if (!saved) return failure(saved.error()); }
        break;
      }
      case O::Alloca: {
        auto size = integer(1); if (!size) return failure(size.error()); if (*size > INT64_MAX) return failure(invalid("negative stack allocation"));
        auto address = stack.allocate(*size); if (!address) return failure(address.error());
        auto saved = write(p[0],ScalarValue::pointer(*address)); if (!saved) return failure(saved.error()); break;
      }
      case O::BStart: {
        auto address = stack.save(); if (!address) return failure(address.error());
        auto saved = write(p[0],ScalarValue::pointer(*address)); if (!saved) return failure(saved.error()); break;
      }
      case O::BEnd: {
        auto address = integer(0); if (!address) return failure(address.error()); auto restored = stack.restore(*address); if (!restored) return failure(restored.error()); break;
      }
      case O::VaStart: { auto destination = integer(0); if (!destination) return failure(destination.error()); auto started = start_variadics(*destination); if (!started) return failure(started.error()); break; }
      case O::VaArg: case O::VaBlockArg: {
        auto list = integer(1); if (!list) return failure(list.error()); auto abi = state.target.abi(); auto size = variadic_state_size(abi);
        auto writable = state.writable(*list,size); if (!writable) return failure(writable.error());
        std::array<std::byte,32> temporary{}; std::memcpy(temporary.data(),reinterpret_cast<const std::byte *>(*list),size);
        const bool native = invocation.native_variadic_cursor(std::span(temporary).first(size));
        if (op == O::VaArg) {
          auto type = p[2].source->get<ir::Memory>()->type;
          auto address = advance_variadic_scalar(abi,std::span(temporary).first(size),type); if (!address) return failure(address.error());
          auto layout = state.target.layout(type); if (!layout) return failure(layout.error());
          if (!native && !inside(invocation,*address,layout->size)) return failure(invalid("variadic argument exceeds its invocation area"));
          auto saved = write(p[0],ScalarValue::pointer(*address)); if (!saved) return failure(saved.error());
        } else {
          auto destination = integer(0),bytes = integer(2),category = integer(3);
          if (!destination) return failure(destination.error()); if (!bytes) return failure(bytes.error()); if (!category) return failure(category.error());
          writable = state.writable(*destination,*bytes); if (!writable) return failure(writable.error());
          std::uintptr_t cursor; std::memcpy(&cursor,temporary.data()+(abi == target::ABI::X86SysV ? 8 : 0),8);
          bool indirect = abi != target::ABI::X86SysV && *bytes > (abi == target::ABI::X86Windows ? 8u : 16u);
          if (*bytes > SIZE_MAX-7 || (!native && *bytes && !inside(invocation,cursor,indirect ? 8 : (*bytes+7)/8*8))) return failure(invalid("variadic block exceeds its invocation area"));
          if (*category > 4) return failure(invalid("invalid variadic block class"));
          auto copied = advance_variadic_block(abi,std::span(temporary).first(size),{reinterpret_cast<std::byte *>(*destination),*bytes},*bytes,static_cast<ir::Type::BlockClass>(*category));
          if (!copied) return failure(copied.error());
        }
        std::memcpy(reinterpret_cast<std::byte *>(*list),temporary.data(),size); break;
      }
      default: return failure(invalid("unhandled public interpreter opcode"));
    }
  }
  return invalid("execution fell through the end of its function");
}
}
