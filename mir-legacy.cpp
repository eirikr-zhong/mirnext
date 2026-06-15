/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir-legacy.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace mirnext {
namespace {

struct BinaryReadState {
  const std::byte *data = nullptr;
  std::size_t size = 0;
  std::size_t pos = 0;
};

thread_local BinaryReadState *current_binary_read = nullptr;

int binary_reader(MIR_context_t) {
  if (current_binary_read == nullptr || current_binary_read->pos >= current_binary_read->size) {
    return EOF;
  }
  return std::to_integer<int>(current_binary_read->data[current_binary_read->pos++]);
}

MIR_module_t find_latest_module(MIR_context_t ctx, std::string_view name) {
  MIR_module_t found = nullptr;
  for (MIR_module_t current = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(ctx));
       current != nullptr; current = DLIST_NEXT(MIR_module_t, current)) {
    if (std::string_view(current->name) == name) found = current;
  }
  return found;
}

MIR_item_t find_function_item(MIR_context_t ctx, MIR_module_t module, std::string_view name) {
  if (module == nullptr) return nullptr;
  for (MIR_item_t item = DLIST_HEAD(MIR_item_t, module->items); item != nullptr;
       item = DLIST_NEXT(MIR_item_t, item)) {
    if (item->item_type == MIR_func_item && std::string_view(MIR_item_name(ctx, item)) == name) {
      return item;
    }
  }
  return nullptr;
}

bool module_has_linked_functions(MIR_module_t module) {
  if (module == nullptr) return false;
  for (MIR_item_t item = DLIST_HEAD(MIR_item_t, module->items); item != nullptr;
       item = DLIST_NEXT(MIR_item_t, item)) {
    if (item->item_type == MIR_func_item && item->data != nullptr) return true;
  }
  return false;
}

void load_and_link_once(MIR_context_t ctx, MIR_module_t module,
                        void (*set_interface)(MIR_context_t, MIR_item_t),
                        void *import_resolver(const char *)) {
  if (module_has_linked_functions(module)) return;
  MIR_load_module(ctx, module);
  MIR_link(ctx, set_interface, import_resolver);
}

bool module_needs_link(MIR_module_t module) {
  return !module_has_linked_functions(module);
}

} // namespace

LegacyContext::LegacyContext() : ctx_(MIR_init()) {}

LegacyContext::~LegacyContext() {
  if (ctx_ != nullptr) MIR_finish(ctx_);
}

MIR_context_t LegacyContext::raw() const noexcept { return ctx_; }

Result<LegacyLoweredFunction> lower_to_legacy(LegacyContext &context, const Module &module,
                                              const Function &function) {
  MIR_context_t ctx = context.raw();
  if (ctx == nullptr) return Error{ErrorCode::InvalidArgument, "missing legacy context"};

  std::string_view requested_name;
  bool found_requested_function = false;
  for (const auto &current_function : module.functions()) {
    if (current_function.get() == &function) {
      requested_name = current_function->name();
      found_requested_function = true;
      break;
    }
  }
  if (!found_requested_function) return Error{ErrorCode::UnknownName, "function is not in module"};

  auto bytes_result = module.encode_binary();
  MIRNEXT_RESULT_RET(bytes_result);
  auto bytes = *bytes_result;
  BinaryReadState read_state{bytes.data(), bytes.size(), 0};
  current_binary_read = &read_state;
  MIR_read_with_func(ctx, binary_reader);
  current_binary_read = nullptr;

  MIR_module_t legacy_module = find_latest_module(ctx, module.name());
  if (legacy_module == nullptr) return Error{ErrorCode::UnknownName, "decoded module not found"};
  MIR_item_t requested_function = find_function_item(ctx, legacy_module, requested_name);
  if (requested_function == nullptr) {
    return Error{ErrorCode::UnknownName, "decoded function not found"};
  }
  return LegacyLoweredFunction{legacy_module, requested_function};
}

Result<std::int64_t> interpret_i64(LegacyContext &context, MIR_module_t module, MIR_item_t function,
                                   const std::int64_t *args, std::size_t arg_count) {
  if (context.raw() == nullptr || module == nullptr || function == nullptr) {
    return Error{ErrorCode::InvalidArgument, "invalid legacy execution input"};
  }
  std::vector<MIR_val_t> values(arg_count);
  for (std::size_t i = 0; i < arg_count; ++i) values[i].i = args[i];
  load_and_link_once(context.raw(), module, MIR_set_interp_interface, nullptr);
  MIR_val_t result;
  MIR_interp_arr(context.raw(), function, &result, arg_count, values.data());
  return result.i;
}

Result<std::int64_t> generate_and_call_i64(LegacyContext &context, MIR_module_t module,
                                           MIR_item_t function, const std::int64_t *args,
                                           std::size_t arg_count) {
  if (context.raw() == nullptr || module == nullptr || function == nullptr || arg_count > 1) {
    return Error{ErrorCode::InvalidArgument, "invalid legacy generation input"};
  }
  const bool needs_link = module_needs_link(module);
  if (needs_link) MIR_load_module(context.raw(), module);
  MIR_gen_init(context.raw());
  MIR_gen_set_optimize_level(context.raw(), 2);
  if (needs_link) MIR_link(context.raw(), MIR_set_gen_interface, nullptr);
  void *addr = MIR_gen(context.raw(), function);
  std::int64_t result = 0;
  if (arg_count == 0) {
    result = reinterpret_cast<std::int64_t (*)()>(addr)();
  } else {
    result = reinterpret_cast<std::int64_t (*)(std::int64_t)>(addr)(args[0]);
  }
  MIR_gen_finish(context.raw());
  return result;
}

Result<std::uint64_t> interpret_u64(LegacyContext &context, MIR_module_t module, MIR_item_t function,
                                    const std::uint64_t *args, std::size_t arg_count) {
  if (context.raw() == nullptr || module == nullptr || function == nullptr) {
    return Error{ErrorCode::InvalidArgument, "invalid legacy execution input"};
  }
  std::vector<MIR_val_t> values(arg_count);
  for (std::size_t i = 0; i < arg_count; ++i) values[i].u = args[i];
  load_and_link_once(context.raw(), module, MIR_set_interp_interface, nullptr);
  MIR_val_t result;
  MIR_interp_arr(context.raw(), function, &result, arg_count, values.data());
  return result.u;
}

Result<std::uint64_t> generate_and_call_u64(LegacyContext &context, MIR_module_t module,
                                            MIR_item_t function, const std::uint64_t *args,
                                            std::size_t arg_count) {
  if (context.raw() == nullptr || module == nullptr || function == nullptr || arg_count > 1) {
    return Error{ErrorCode::InvalidArgument, "invalid legacy generation input"};
  }
  const bool needs_link = module_needs_link(module);
  if (needs_link) MIR_load_module(context.raw(), module);
  MIR_gen_init(context.raw());
  MIR_gen_set_optimize_level(context.raw(), 2);
  if (needs_link) MIR_link(context.raw(), MIR_set_gen_interface, nullptr);
  void *addr = MIR_gen(context.raw(), function);
  std::uint64_t result = 0;
  if (arg_count == 0) {
    result = reinterpret_cast<std::uint64_t (*)()>(addr)();
  } else {
    result = reinterpret_cast<std::uint64_t (*)(std::uint64_t)>(addr)(args[0]);
  }
  MIR_gen_finish(context.raw());
  return result;
}

Result<double> interpret_double(LegacyContext &context, MIR_module_t module, MIR_item_t function,
                                const double *args, std::size_t arg_count) {
  if (context.raw() == nullptr || module == nullptr || function == nullptr) {
    return Error{ErrorCode::InvalidArgument, "invalid legacy execution input"};
  }
  std::vector<MIR_val_t> values(arg_count);
  for (std::size_t i = 0; i < arg_count; ++i) values[i].d = args[i];
  load_and_link_once(context.raw(), module, MIR_set_interp_interface, nullptr);
  MIR_val_t result;
  MIR_interp_arr(context.raw(), function, &result, arg_count, values.data());
  return result.d;
}

Result<double> generate_and_call_double(LegacyContext &context, MIR_module_t module,
                                        MIR_item_t function, const double *args,
                                        std::size_t arg_count) {
  if (context.raw() == nullptr || module == nullptr || function == nullptr || arg_count > 1) {
    return Error{ErrorCode::InvalidArgument, "invalid legacy generation input"};
  }
  const bool needs_link = module_needs_link(module);
  if (needs_link) MIR_load_module(context.raw(), module);
  MIR_gen_init(context.raw());
  MIR_gen_set_optimize_level(context.raw(), 2);
  if (needs_link) MIR_link(context.raw(), MIR_set_gen_interface, nullptr);
  void *addr = MIR_gen(context.raw(), function);
  double result = 0.0;
  if (arg_count == 0) {
    result = reinterpret_cast<double (*)()>(addr)();
  } else {
    result = reinterpret_cast<double (*)(double)>(addr)(args[0]);
  }
  MIR_gen_finish(context.raw());
  return result;
}

} // namespace mirnext
