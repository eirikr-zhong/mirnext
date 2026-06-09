/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#ifndef MIRNEXT_LEGACY_HPP
#define MIRNEXT_LEGACY_HPP

#include "mir.hpp"

#include <cstddef>
#include <cstdint>

extern "C" {
#include "mir-gen.h"
}

namespace mirnext {

class LegacyContext {
public:
  LegacyContext();
  ~LegacyContext();

  LegacyContext(const LegacyContext &) = delete;
  LegacyContext &operator=(const LegacyContext &) = delete;
  LegacyContext(LegacyContext &&) = delete;
  LegacyContext &operator=(LegacyContext &&) = delete;

  MIR_context_t raw() const noexcept;

private:
  MIR_context_t ctx_;
};

struct LegacyLoweredFunction {
  MIR_module_t module = nullptr;
  MIR_item_t function = nullptr;
};

Result<LegacyLoweredFunction> lower_to_legacy(LegacyContext &context, const Module &module,
                                              const Function &function);

Result<std::int64_t> interpret_i64(LegacyContext &context, MIR_module_t module, MIR_item_t function,
                                   const std::int64_t *args = nullptr,
                                   std::size_t arg_count = 0);

Result<std::int64_t> generate_and_call_i64(LegacyContext &context, MIR_module_t module,
                                           MIR_item_t function, const std::int64_t *args = nullptr,
                                           std::size_t arg_count = 0);

Result<double> interpret_double(LegacyContext &context, MIR_module_t module, MIR_item_t function,
                                const double *args = nullptr, std::size_t arg_count = 0);

Result<double> generate_and_call_double(LegacyContext &context, MIR_module_t module,
                                        MIR_item_t function, const double *args = nullptr,
                                        std::size_t arg_count = 0);

} // namespace mirnext

#endif
