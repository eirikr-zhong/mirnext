/* Licensed under the MIT License. One function registry, symbol image and load pipeline. */
#include "mirnext/Execution/detail/LoadedProgram.hpp"
#include "mirnext/CodeGen/BlockEntry.hpp"
#include <algorithm>
namespace mirnext::execution::detail {
namespace {
Error invalid(std::string text) { return {ErrorCode::InvalidArgument,std::move(text)}; }
bool interpreted(ExecutionEngine engine) { return engine == ExecutionEngine::Interpreter; }
std::uintptr_t reject_jump(LoadedProgram::JumpDescriptor *descriptor) {
  report_execution_error(descriptor->state.error(descriptor->state.bindings[descriptor->binding].source,
    "engine-jump",invalid("JCALL/JRET cannot cross interpreted and native execution domains")));
  return 0;
}
}
Result<ExecutionEngine> LoadedProgram::jit_engine(JITCompilation compilation) {
  switch (compilation) {
    case JITCompilation::Eager: return ExecutionEngine::EagerJIT;
    case JITCompilation::Lazy: return ExecutionEngine::LazyJIT;
    case JITCompilation::BlockVersioning: return ExecutionEngine::BlockVersioning;
  }
  return invalid("invalid JIT compilation policy");
}
ExecutionEngine LoadedProgram::engine(std::size_t index) const {
  auto &entry = binding(index);
  if (entry.function) return functions[*entry.function]->engine;
  // Imports have no selected definition. Mixed/interpreted host handles use
  // the checked interpreter dispatch, including typed callbacks and policy;
  // only a wholly native program dispatches them directly through the ABI.
  return has_native && !has_interpreted ? ExecutionEngine::EagerJIT : ExecutionEngine::Interpreter;
}
ExecutionMode LoadedProgram::mode() const {
  if (has_interpreted && has_native) return ExecutionMode::Mixed;
  return has_native ? ExecutionMode::JIT : ExecutionMode::Interpreter;
}
Result<void> LoadedProgram::register_functions(const EngineSelector &select) {
  for (auto &module : context.modules()) for (auto &f : module.functions()) {
    Result<ExecutionEngine> selected = select ? select(f) : Result<ExecutionEngine>(default_engine);
    if (!selected) return error(&f.symbol(),"engine",selected.error());
    switch (*selected) {
      case ExecutionEngine::Interpreter: has_interpreted = true; break;
      case ExecutionEngine::EagerJIT: case ExecutionEngine::LazyJIT: case ExecutionEngine::BlockVersioning: has_native = true; break;
      default: return error(&f.symbol(),"engine",invalid("invalid function execution engine"));
    }
    auto record = std::make_unique<Code>(); record->source = &f; record->engine = *selected;
    functions.push_back(std::move(record)); ++statistics.functions;
  }
  if (functions.empty()) {
    has_interpreted = interpreted(default_engine); has_native = !has_interpreted;
  }
  return {};
}
Result<void> LoadedProgram::prepare_hardware() {
  for (auto &record : functions) for (auto &r : record->source->registers()) if (!r.hardware_binding().empty()) {
    auto reg = target.find_register(r.hardware_binding());
    if (!reg) return error(&record->source->symbol(),"hardware",reg.error());
    if (!target.accepts(*reg,r.type())) return error(&record->source->symbol(),"hardware",invalid("invalid hardware binding type"));
    if (hardware_indices.try_emplace(reg->encoding(),hardware.size()).second) {
      auto info = target.register_info(*reg);
      auto type = info->bank == target::RegisterBank::Integer ? ir::Type::i64() : ir::Type(ir::Type::Kind::F64);
      hardware_bindings.emplace_back(type,info->name); hardware.emplace_back();
    }
  }
  return {};
}
Result<void> LoadedProgram::index_callees() {
  if (!has_interpreted) return {};
  ir::StringMap<std::uintptr_t> external_tokens;
  for (std::size_t n = 0; n < bindings.size(); ++n) {
    auto &entry = binding(n); if (entry.kind != Kind::Function) continue;
    if (!entry.address) {
      auto [at,inserted] = external_tokens.try_emplace(entry.source->name(),reinterpret_cast<std::uintptr_t>(&entry));
      entry.address = at->second; if (native_calls || options.native_calls) virtual_functions.insert(entry.address);
    }
    auto &targets = callees[entry.address]; auto index = resolved(n);
    if (std::find(targets.begin(),targets.end(),index) == targets.end()) targets.push_back(index);
  }
  return {};
}
Result<void> LoadedProgram::initialize() {
  for (auto &m : context.modules()) for (auto &item : m.items()) if (auto data = std::get_if<const ir::GlobalData *>(&item))
    if (auto expression = std::get_if<ir::ExpressionData>(&(*data)->initializer())) {
      auto values = invoke(identities.at(&expression->function->symbol()),{});
      if (!values) return error(&(*data)->symbol(),"initialize",values.error());
      if (values->size() != 1) return error(&(*data)->symbol(),"initialize",invalid("invalid expression result count"));
      auto &entry = binding(identities.at(&(*data)->symbol()));
      auto stored = store_scalar(expression->function->signature().results[0],entry.address,values->front());
      if (!stored) return error(&(*data)->symbol(),"initialize",stored.error()); ++statistics.expressions;
    }
  return {};
}
Result<void> LoadedProgram::protect_native_entries() {
  if (!native_boundary) return {};
  std::vector<NativeBoundaryTarget> targets; std::vector<std::size_t> indices;
  for (std::size_t n = 0; n < functions.size(); ++n) {
    auto &record = *functions[n]; auto &entry = bindings[identities.at(&record.source->symbol())];
    if (entry.jump) continue;
    unsigned x87 = 0;
    if (target.architecture() == target::Architecture::X86_64)
      for (auto type : record.source->signature().results)
        if (target.scalar_type(type).kind() == ir::Type::Kind::LongDouble) ++x87;
    targets.push_back({entry.address,x87}); indices.push_back(n);
  }
  auto entries = native_boundary->add_entries(targets); if (!entries) return error(nullptr,"native-boundary",entries.error());
  for (std::size_t n = 0; n < entries->size(); ++n)
    bindings[identities.at(&functions[indices[n]]->source->symbol())].address = (*entries)[n];
  return {};
}
Result<void> LoadedProgram::publish_jump_guards() {
  if (!has_native || !has_interpreted) return {};
  for (auto &record : functions) if (interpreted(record->engine)) {
    auto index = identities.at(&record->source->symbol());
    if (bindings[index].jump) jump_descriptors.push_back(std::make_unique<JumpDescriptor>(JumpDescriptor{*this,index}));
  }
  if (jump_descriptors.empty()) return {};
  auto resolver = codegen::make_block_resolver(target,reinterpret_cast<std::uintptr_t>(&reject_jump),native_boundary->recovery().recovery_address());
  if (!resolver) return error(nullptr,"engine-jump",resolver.error());
  std::vector<std::size_t> sizes(jump_descriptors.size()+1,codegen::block_entry_bytes); sizes[0] = resolver->size();
  auto reservation = CodeMemory(options.link.memory_provider).reserve(sizes); if (!reservation) return reservation.error();
  auto address = reservation->address(0); if (!address) return address.error();
  std::vector<std::vector<std::uint8_t>> buffers; buffers.push_back(std::move(*resolver));
  for (std::size_t n = 0; n < jump_descriptors.size(); ++n) {
    auto &descriptor = jump_descriptors[n];
    auto gate = reservation->address(n+1); if (!gate) return gate.error();
    descriptor->destination = *gate+codegen::block_entry_cold_offset;
    auto bytes = codegen::make_block_entry(target,{reinterpret_cast<std::uintptr_t>(&descriptor->destination),
      reinterpret_cast<std::uintptr_t>(descriptor.get()),*address});
    if (!bytes) return bytes.error(); buffers.push_back(std::move(*bytes));
  }
  std::vector<std::span<const std::uint8_t>> views; for (auto &b : buffers) views.emplace_back(b);
  auto published = reservation->publish(views); if (!published) return published.error();
  for (std::size_t n = 0; n < jump_descriptors.size(); ++n) {
    auto entry = (*published)[n+1].address(); bindings[jump_descriptors[n]->binding].address = entry; virtual_functions.insert(entry);
  }
  for (auto size : sizes) statistics.code_bytes += size; ++statistics.code_batches;
  jump_entries = std::move(*published); return {};
}
Result<void> LoadedProgram::load(EngineSelector select) {
  auto policy = jit_engine(options.link.compilation); if (!policy) return policy.error();
  if (!select && interpreted(default_engine) && options.link.compilation != JITCompilation::Eager)
    return invalid("JIT compilation policy requires JIT mode");
  auto registered = register_functions(select); if (!registered) return registered;
  auto bound = symbols(options.link,has_native && !has_interpreted); if (!bound) return bound;
  for (std::size_t n = 0; n < functions.size(); ++n) bindings[identities.at(&functions[n]->source->symbol())].function = n;
  if (has_native && has_interpreted) for (auto &record : functions) for (auto &block : record->source->blocks()) for (auto &i : block.instructions())
    if (i.opcode() == ir::Opcode::JCall) if (auto ref = i.operands()[1].get<ir::SymbolReference>()) {
      auto &callee = binding(identities.at(ref->symbol));
      if (callee.function && interpreted(record->engine) != interpreted(functions[*callee.function]->engine))
        return error(&record->source->symbol(),"engine-jump",invalid("JCALL/JRET cannot cross interpreted and native execution domains"));
    }
  auto allocated = data(); if (!allocated) return allocated;
  for (auto &entry : bindings) if (entry.kind == Kind::Data && !entry.alias && !entry.writable && entry.size)
    readonly.emplace_back(entry.address,entry.address+entry.size);
  std::sort(readonly.begin(),readonly.end()); std::size_t count = 0;
  for (auto range : readonly) {
    if (count && readonly[count-1].second >= range.first) readonly[count-1].second = std::max(readonly[count-1].second,range.second);
    else readonly[count++] = range;
  }
  readonly.resize(count);
  auto bank = prepare_hardware(); if (!bank) return bank;
  bool foreign = false;
  if (options.native_calls) for (auto &entry : bindings)
    foreign |= entry.kind == Kind::Function && !entry.source->function() && entry.address && !entry.interpret;
  if (has_native || foreign) native_calls = std::make_unique<NativeCalls>(options.link,hardware_bindings);
  if (has_native && has_interpreted) options.native_entries = true;
  auto prepared = prepare(); if (!prepared) return prepared;
  bool blocks = std::any_of(functions.begin(),functions.end(),[](auto &f) { return f->engine == ExecutionEngine::BlockVersioning; });
  if (blocks || (has_native && has_interpreted)) {
    auto boundary = NativeBoundary::create(options.link.memory_provider); if (!boundary) return boundary.error();
    native_boundary.emplace(std::move(*boundary));
  }
  if (blocks) { auto bbv = prepare_block_versioning(); if (!bbv) return bbv; }
  auto lazy = publish_lazy_entries(); if (!lazy) return lazy;
  auto eager = prepare_eager(); if (!eager) return eager.error();
  if (options.native_entries) { auto entries = publish_entries(); if (!entries) return entries; }
  auto guards = publish_jump_guards(); if (!guards) return guards;
  auto protect = protect_native_entries(); if (!protect) return protect;
  if (*eager) { auto publish = publish_eager(**eager); if (!publish) return publish; }
  auto roots = prepare_lazy_roots(); if (!roots) return roots;
  auto callees = index_callees(); if (!callees) return callees;
  auto refs = references([&](const ir::Label &l) -> Result<std::uint64_t> {
    if (engine(identities.at(&l.function().symbol())) != ExecutionEngine::Interpreter) return native_label_address(l);
    auto address = label_address(l); if (!address) return address.error(); return std::uint64_t(*address);
  }); if (!refs) return refs;
  auto initialized = initialize(); if (!initialized) return initialized;
  options.link.resolve = {};
  // An otherwise interpreted program can receive its first foreign function
  // pointer after loading. Retain the services for that cold ABI adapter.
  if (!has_native && !options.native_calls) { options.link.compilation_observer = {}; options.link.memory_provider.reset(); }
  return {};
}
Result<std::size_t> LoadedProgram::find_function(std::string_view module,std::string_view name) const {
  auto index = find_native(module,name); if (!index) return index.error();
  if (binding(*index).kind != Kind::Function) return Error{ErrorCode::InvalidOperand,"symbol is not a loaded function"}; return index;
}
const codegen::CompiledFunction *LoadedProgram::artifact(std::size_t index) const {
  auto f = binding(index).function;
  return f && functions[*f]->artifact ? &*functions[*f]->artifact : nullptr;
}
Result<std::vector<ScalarValue>> LoadedProgram::invoke(std::size_t index,std::span<const ExecutionArgument> args) {
  if (engine(index) != ExecutionEngine::Interpreter) {
    auto &f = binding(index);
    return native_calls->call(f.address,*f.signature,args,native_boundary ? &native_boundary->recovery() : nullptr,
      has_interpreted ? std::span(hardware) : std::span<HardwareSlot>{});
  }
  if (auto active = InvocationActivation::find(*this)) {
    auto result = active->invoke(index,args); if (!result) return result.error(); return std::move(result->values);
  }
  InterpreterInvocation invocation{*this}; auto result = invocation.invoke(index,args);
  if (!result) return result.error(); return std::move(result->values);
}
}
