/* Licensed under the MIT License. The explicitly fatal helper runs in a child. */
#include "mirnext/Support/Result.hpp"
#include <string_view>
int main (int argc, char **argv) {
  if (argc != 2) return 3;
  std::string_view mode (argv[1]);
  if (mode != "success" && mode != "value-failure" && mode != "void-failure") return 3;
  int calls = 0;
  auto diagnostic = [&] () {
    return mirnext::Error{mirnext::ErrorCode::UnknownName,
                          calls == 1 ? "first diagnostic" : "repeated factory evaluation"};
  };
  if (mode == "void-failure") {
    auto produce = [&] () -> mirnext::Result<void> {
      ++calls;
      return diagnostic ();
    };
    MIRNEXT_RESULT_FAULT (produce ());
  } else {
    auto produce = [&] () -> mirnext::Result<int> {
      ++calls;
      if (mode == "success") return 42;
      return diagnostic ();
    };
    MIRNEXT_RESULT_FAULT (produce ());
  }
  return mode == "success" && calls == 1 ? 0 : 2;
}
