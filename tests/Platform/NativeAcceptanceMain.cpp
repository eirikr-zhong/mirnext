/* Licensed under the MIT License. Linkage identity belongs to this executable. */
#include <string_view>
int mirnext_native_acceptance(int, char **, std::string_view);
int main(int argc, char **argv) {
  return mirnext_native_acceptance(argc, argv, MIRNEXT_TEST_LINKAGE);
}
