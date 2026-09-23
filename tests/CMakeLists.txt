# Standalone build of the mod-worldscale tests.
#
# This exists so the tests can be built and run *without AzerothCore*: the
# code under test (../src/WorldScaleMath.h) deliberately includes nothing but
# the standard library, so all this needs is a compiler and googletest. That
# is what the GitHub workflow uses - cloning and compiling the core to check
# some arithmetic would take an hour per run and need ~30 GB.
#
#   cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests
#
# Inside a full AzerothCore tree this file is not used at all: there,
# ../mod-worldscale.cmake registers the same test sources with the core's own
# unit_tests target, so they run alongside AzerothCore's suite.

cmake_minimum_required(VERSION 3.16)
project(mod_worldscale_tests CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
  GIT_SHALLOW    TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

file(GLOB TEST_SOURCES "${CMAKE_CURRENT_LIST_DIR}/*.cpp")

add_executable(worldscale_tests ${TEST_SOURCES})
target_include_directories(worldscale_tests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
target_link_libraries(worldscale_tests PRIVATE gtest_main)

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(worldscale_tests PRIVATE -Wall -Wextra -Werror)
endif()

enable_testing()
include(GoogleTest)
gtest_discover_tests(worldscale_tests)
