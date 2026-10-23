cmake_minimum_required(VERSION 3.16)
project(cuinspect LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find LLVM
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM: ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM Include Dir: ${LLVM_INCLUDE_DIRS}")
message(STATUS "LLVM Lib Dir: ${LLVM_LIBRARY_DIRS}")

# Required LLVM components for ELF/DWARF parsing
add_executable(cuinspect main.cpp)

target_link_libraries(cuinspect PRIVATE
    LLVM::LLVMObject          # ELF/DWARF parsing
    LLVM::LLVMBinaryFormat    # ELF constants
    LLVM::Support             # Core utilities
    LLVM::Demangle            # Symbol demangling
)

# Compile options
target_compile_options(cuinspect PRIVATE
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Install target (optional)
install(TARGETS cuinspect DESTINATION bin)