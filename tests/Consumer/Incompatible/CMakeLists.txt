cmake_minimum_required(VERSION 3.14)
project(MIRNextIncompatibleLinkage LANGUAGES CXX)
find_package(MIRNext CONFIG REQUIRED)
add_executable(incompatible ../main.cpp)
# A second IR/Execution instance would split object and thread-local state.
# Reject this before compiling or running the consumer.
target_link_libraries(incompatible PRIVATE MIRNext::Execution MIRNext::SerializationShared)
