/* Licensed under the MIT License. Shared owning binary/embedded program execution. */
#pragma once
#include "mirnext/Execution/Session.hpp"
namespace mirnext::tools {
Result<int> run_program(std::span<const std::byte>,int argc,char **argv,char **environment);
}
