cmake_minimum_required(VERSION 3.22)
project(zettabridge LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

add_subdirectory(core)
add_subdirectory(cli/zbrun)

option(ZB_BUILD_TESTS "Build host tests" ON)
if (ZB_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/host)
endif()
