# Host unit tests with any C++17 compiler (MSVC, GCC, Clang) — the same
# test/test_*/test_main.cpp suites `pio test -e native` runs on GCC hosts,
# built here against the self-contained Unity shim (shim/unity.h).
#
#   cmake -S test-host -B test-host/build            (MSVC: -G "Visual Studio 17 2022" -A x64)
#   cmake --build test-host/build --config Release
#   ctest --test-dir test-host/build -C Release --output-on-failure
#
# Every test_* directory becomes one executable; extra .cpp files in a test
# directory are compiled into it, headers next to the test and generated
# fixtures (fixtures/, scripts/gen_*_fixture.py) are visible.

cmake_minimum_required(VERSION 3.16)
project(esprack_test_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ESPRACK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

file(GLOB PURE_SRC CONFIGURE_DEPENDS ${ESPRACK_ROOT}/lib/ESPRackPure/src/*.cpp)
add_library(esprack_pure STATIC ${PURE_SRC})
target_include_directories(esprack_pure PUBLIC ${ESPRACK_ROOT}/lib/ESPRackPure/include)

if(MSVC)
  set(WARN /W4 /permissive- /utf-8 /wd4127)
else()
  set(WARN -Wall -Wextra -Wpedantic)
endif()
target_compile_options(esprack_pure PRIVATE ${WARN})

enable_testing()

file(GLOB TEST_DIRS LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/test/test_*)
foreach(dir ${TEST_DIRS})
  if(IS_DIRECTORY ${dir})
    get_filename_component(name ${dir} NAME)
    file(GLOB srcs CONFIGURE_DEPENDS ${dir}/*.cpp)
    add_executable(${name} ${srcs})
    target_include_directories(${name} PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/shim
      ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
      ${dir}
      ${ESPRACK_ROOT}/modules/digital-io/include)
    target_compile_options(${name} PRIVATE ${WARN})
    target_link_libraries(${name} PRIVATE esprack_pure)
    add_test(NAME ${name} COMMAND ${name})
  endif()
endforeach()
