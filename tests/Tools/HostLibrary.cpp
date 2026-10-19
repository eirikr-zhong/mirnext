/* Licensed under the MIT License. A real dynamically loaded CLI test symbol. */
#include <cstdint>
#ifdef _WIN32
#define MIRNEXT_TEST_EXPORT __declspec(dllexport)
#else
#define MIRNEXT_TEST_EXPORT __attribute__((visibility("default")))
#endif
extern "C" MIRNEXT_TEST_EXPORT std::int64_t mirnext_cli_add(std::int64_t a,std::int64_t b) { return a+b; }
