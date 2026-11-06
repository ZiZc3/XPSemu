# fexrun — on-device harness for the native core's FEX fork.
#
# Runs FEX's 32-bit ASM unit tests (and tests/native/sysops) through our own
# embedding of FEXCore (no Linux syscall front end), with guest memory either
# identity-mapped or behind the guest memory-base register. Cross-compiled
# with the NDK and run from /data/local/tmp. See tools/native/fexrun-suite.sh.
cmake_minimum_required(VERSION 3.20)
project(fexrun C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../../native/cmake/fex.cmake")

add_executable(fexrun fexrun.cpp)
target_link_libraries(fexrun PRIVATE nc_fex)
target_compile_options(fexrun PRIVATE -Wall -Wno-unused-parameter)
