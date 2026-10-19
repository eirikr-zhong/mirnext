/* Licensed under the MIT License. Preserve the original optimizer's backend address predicate. */
#include "mirnext/Target/Addressing.hpp"
#include <limits>
namespace mirnext::target {
Result<AddressingRules> AddressingRules::get (Architecture architecture) {
  if (architecture == Architecture::Native) {
#if defined(__aarch64__) || defined(_M_ARM64)
    architecture = Architecture::AArch64;
#elif defined(__x86_64__) || defined(_M_X64)
    architecture = Architecture::X86_64;
#else
    return Error{ErrorCode::InvalidArgument, "unsupported native addressing target"};
#endif
  }
  if (architecture != Architecture::X86_64 && architecture != Architecture::AArch64)
    return Error{ErrorCode::InvalidArgument, "unsupported addressing target"};
  return AddressingRules (architecture);
}
bool AddressingRules::accepts (ir::Type type, std::int64_t displacement, bool indexed,
                               unsigned scale) const {
  if (architecture_ == Architecture::X86_64)
    return (!indexed || scale == 1 || scale == 2 || scale == 4 || scale == 8)
           && displacement >= std::numeric_limits<std::int32_t>::min ()
           && displacement <= std::numeric_limits<std::int32_t>::max ();
  std::size_t size = 0;
  switch (type.kind ()) {
  case ir::Type::Kind::I8:
  case ir::Type::Kind::U8: size = 1; break;
  case ir::Type::Kind::I16:
  case ir::Type::Kind::U16: size = 2; break;
  case ir::Type::Kind::I32:
  case ir::Type::Kind::U32:
  case ir::Type::Kind::F32: size = 4; break;
  case ir::Type::Kind::Bool:
  case ir::Type::Kind::I64:
  case ir::Type::Kind::U64:
  case ir::Type::Kind::Pointer:
  case ir::Type::Kind::F64: size = 8; break;
  case ir::Type::Kind::LongDouble: size = sizeof (long double); break;
  default: return false;
  }
  unsigned shift = 0;
  while ((size >>= 1)) ++shift;
  // This optimizer predicate intentionally uses log2(size), not the machine
  // encoder's scaled displacement rule. A byte's zero divisor is rejected.
  if (!displacement && (!indexed || scale == 1 || scale == shift)) return true;
  return shift && !indexed && displacement >= 0 && displacement % shift == 0
         && displacement / shift < (1 << 12);
}
}  // namespace mirnext::target
