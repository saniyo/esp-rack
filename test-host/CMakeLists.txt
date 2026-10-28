# Host unit tests for the parts of the core that don't need Arduino/IDF
# (today: time/TimeArbiter, hw/HwClaims, hw/Rs485Framer, persist/*, power/PowerGuard, power/WriteGateCore,
# history/HistCore), with any C++17 compiler (MSVC, GCC, Clang) and a self-contained Unity-compatible shim
# (shim/unity.h). WriteGate itself runs on the host as shim/WriteGateHost.cpp (one thread, a set clock).
#
#   cmake -S test-host -B test-host/build            (MSVC: -G "Visual Studio 17 2022" -A x64)
#   cmake --build test-host/build --config Release
#   ctest --test-dir test-host/build -C Release --output-on-failure
#
# On Windows: powershell -File test-host/run-host-tests.ps1 [-BuildDir <short path>]
# Every test_* directory becomes one executable.

cmake_minimum_required(VERSION 3.16)
project(esprack_test_host CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ESPRACK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Arduino-free core sources only.
add_library(esprack_host STATIC
  ${ESPRACK_ROOT}/lib/ESPRack/src/time/TimeArbiter.cpp
  ${ESPRACK_ROOT}/lib/ESPRack/src/hw/HwClaims.cpp
  ${ESPRACK_ROOT}/lib/ESPRack/src/persist/Crc32.cpp
  ${ESPRACK_ROOT}/lib/ESPRack/src/persist/NvsBlob.cpp
  ${ESPRACK_ROOT}/lib/ESPRack/src/power/PowerGuard.cpp
  ${ESPRACK_ROOT}/lib/ESPRack/src/power/WriteGateCore.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/shim/WriteGateHost.cpp
  ${ESPRACK_ROOT}/lib/ESPRack/src/history/HistCore.cpp
  ${ESPRACK_ROOT}/lib/ESPRack/src/hw/Rs485Framer.cpp)
target_include_directories(esprack_host PUBLIC ${ESPRACK_ROOT}/lib/ESPRack/include ${CMAKE_CURRENT_SOURCE_DIR}/shim)

if(MSVC)
  set(WARN /W4 /permissive- /utf-8 /wd4127)
else()
  set(WARN -Wall -Wextra -Wpedantic)
endif()
target_compile_options(esprack_host PRIVATE ${WARN})

enable_testing()

file(GLOB TEST_DIRS LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/test/test_*)
foreach(dir ${TEST_DIRS})
  if(IS_DIRECTORY ${dir})
    get_filename_component(name ${dir} NAME)
    file(GLOB srcs CONFIGURE_DEPENDS ${dir}/*.cpp)
    add_executable(${name} ${srcs})
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim ${dir})
    target_compile_options(${name} PRIVATE ${WARN})
    target_link_libraries(${name} PRIVATE esprack_host)
    add_test(NAME ${name} COMMAND ${name})
  endif()
endforeach()
