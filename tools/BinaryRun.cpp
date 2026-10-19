/* Licensed under the MIT License. binfmt-style MIR executable entry. */
#include "Streams.hpp"
#include "Run.hpp"
#include <fstream>
int main(int argc,char **argv,char **environment) {
  using namespace mirnext::tools;
  if (argc < 3) { std::cerr << "usage: " << argv[0] << " <full-path> <name> [args...]\n"; return 1; }
  std::ifstream file(argv[1],std::ios::binary); if (!file) return report(invalid(std::string(argv[1])+": cannot open binary"));
  auto input = read_stream(file,argv[1]); if (!input) return report(input.error());
  auto result = run_program(bytes(*input),argc-2,argv+2,environment);
  return result ? *result : report(result.error());
}
