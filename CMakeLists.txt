cmake_minimum_required(VERSION 3.15)
project(loglite VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Single source of truth: every solution, project and binary is generated
# inside the build tree. No .sln/.vcxproj or build output in the source dir.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

foreach(_cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${_cfg} _CFG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/bin/${_cfg}")
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/bin/${_cfg}")
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_CFG} "${CMAKE_BINARY_DIR}/lib/${_cfg}")
endforeach()

option(LOGLITE_BUILD_SHARED "Build loglite as a shared library" OFF)
option(LOGLITE_BUILD_TESTS  "Build and register the test suite" ON)
option(LOGLITE_BUILD_SAMPLE "Build and register the usage sample" ON)
option(LOGLITE_BUILD_BENCH  "Build the throughput benchmark (manual)" ON)

enable_testing()

# ---------------------------- library ----------------------------
if(LOGLITE_BUILD_SHARED)
  add_library(loglite SHARED src/loglite.cpp)
  target_compile_definitions(loglite PRIVATE LOGLITE_BUILD_SHARED)
  target_compile_definitions(loglite INTERFACE LOGLITE_USE_SHARED)
else()
  add_library(loglite STATIC src/loglite.cpp)
endif()

target_include_directories(loglite PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(loglite PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(loglite PUBLIC Threads::Threads)

if(MSVC)
  # /Zc:preprocessor: conforming preprocessor so variadic wrapper macros
  # forward every argument correctly.
  target_compile_options(loglite PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor)
endif()

# ---------------------------- executables ----------------------------
function(loglite_add_executable target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE loglite)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:preprocessor)
  endif()
endfunction()

if(LOGLITE_BUILD_TESTS)
  loglite_add_executable(loglite_tests tests/loglite_tests.cpp)
  add_test(NAME loglite_tests COMMAND loglite_tests)
endif()

if(LOGLITE_BUILD_SAMPLE)
  loglite_add_executable(loglite_sample sample/loglite_sample.cpp)
  add_test(NAME loglite_sample COMMAND loglite_sample)   # smoke test
endif()

if(LOGLITE_BUILD_BENCH)
  # Manual, long-running: deliberately NOT registered with ctest.
  loglite_add_executable(loglite_bench tests/loglite_bench.cpp)
endif()
