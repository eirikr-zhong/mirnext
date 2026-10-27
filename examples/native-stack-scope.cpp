/* Licensed under the MIT License. Canonical C++ IR and read-only serialization. */
#include <mirnext/Serialization/Format.hpp>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  auto failed = [] (const Error &error) {
    std::cerr << error.message << '\n';
    return 1;
  };
  Context context;
  auto module = context.create_module ("example");
  if (!module) return failed (module.error ());
  auto function = (*module)->create_function ("answer", {{Type::i64 ()}, {}});
  if (!function) return failed (function.error ());
  auto entry = (*function)->create_block ();
  if (!entry) return failed (entry.error ());
  IRBuilder builder (**entry);
  auto scope = builder.begin_stack_scope ("temporary");
  if (!scope) return failed (scope.error ());
  auto address = builder.allocate_stack (Operand (std::int64_t{16}));
  if (!address) return failed (address.error ());
  auto stored = builder.store (Memory{Type::i64 (), *address}, Operand (std::int64_t{42}));
  if (!stored) return failed (stored.error ());
  auto returned = builder.ret (Operand (Memory{Type::i64 (), *address}));
  if (!returned) return failed (returned.error ());
  auto ended = builder.end_stack_scope (**scope);
  if (!ended) return failed (ended.error ());
  auto text = serialization::write_text (context);
  if (!text) return failed (text.error ());
  auto bytes = serialization::write_binary (context);
  if (!bytes) return failed (bytes.error ());
  auto decoded = serialization::read_binary (*bytes);
  if (!decoded) return failed (decoded.error ());
  std::cout << *text;
  return (*decoded)->verify () && needs_materialization (context)
             && !needs_materialization (**decoded)
           ? 0
           : 1;
}
