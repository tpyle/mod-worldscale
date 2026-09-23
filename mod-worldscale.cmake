# Included inline by modules/CMakeLists.txt during configuration (it does
# `include(modules/<name>/<name>.cmake OPTIONAL)` for every module), so no
# change to AzerothCore is needed to get a module's tests built.
#
# Only <module>/src is compiled into the server - CollectSourceFiles is given
# that path and nothing else - so tests/ here is invisible to the worldserver
# build and is picked up solely by the unit_tests target, and then only when
# the build is configured with -DBUILD_TESTING=ON.

file(GLOB_RECURSE MOD_WORLDSCALE_TESTS "${CMAKE_CURRENT_LIST_DIR}/tests/*.cpp")

if (MOD_WORLDSCALE_TESTS)
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_SOURCES ${MOD_WORLDSCALE_TESTS})
  set_property(GLOBAL APPEND PROPERTY ACORE_MODULE_TEST_INCLUDES "${CMAKE_CURRENT_LIST_DIR}/src")
endif()
