# Host tests of the loader's IDF-free parts (main/image_rules, http_lite,
# sig_verify) with esp-rack's unity shim (test-host/shim) and the mbedtls
# of an ESP-IDF tree, built here with its default config: the signature
# check runs the same mbedtls calls as on the chip.
#
#   cmake -S loader/test-host -B <build> -G "Visual Studio 17 2022" -A x64
#   cmake --build <build> --config Release
#   ctest --test-dir <build> -C Release --output-on-failure
#
# MBEDTLS_DIR: <idf>/components/mbedtls/mbedtls; default: PlatformIO's
# framework-espidf package.

cmake_minimum_required(VERSION 3.16)
project(esprack_loader_test_host C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(MAIN ${CMAKE_CURRENT_SOURCE_DIR}/../main)

if(NOT MBEDTLS_DIR)
  file(TO_CMAKE_PATH "$ENV{USERPROFILE}$ENV{HOME}" _home)
  file(GLOB _found LIST_DIRECTORIES true "${_home}/.platformio/packages/framework-espidf*/components/mbedtls/mbedtls")
  if(NOT _found)
    message(FATAL_ERROR "mbedtls not found: pass -DMBEDTLS_DIR=<idf>/components/mbedtls/mbedtls")
  endif()
  list(GET _found 0 MBEDTLS_DIR)
endif()
message(STATUS "mbedtls: ${MBEDTLS_DIR}")

file(GLOB MBEDTLS_SRC ${MBEDTLS_DIR}/library/*.c)
add_library(mbedtls_host STATIC ${MBEDTLS_SRC})
target_include_directories(mbedtls_host PUBLIC ${MBEDTLS_DIR}/include PRIVATE ${MBEDTLS_DIR}/library)
if(WIN32)
  target_link_libraries(mbedtls_host PUBLIC bcrypt)
endif()

add_executable(test_loader
  test_loader.cpp
  ${MAIN}/image_rules.cpp
  ${MAIN}/http_lite.cpp
  ${MAIN}/sig_verify.cpp
  ${ROOT}/lib/ESPRack/src/system/OtaHandoff.cpp
  ${ROOT}/lib/ESPRack/src/persist/Crc32.cpp)
target_include_directories(test_loader PRIVATE ${MAIN} ${ROOT}/lib/ESPRack/include ${ROOT}/test-host/shim)
target_link_libraries(test_loader PRIVATE mbedtls_host)
if(MSVC)
  target_compile_options(test_loader PRIVATE /W4 /permissive- /utf-8 /wd4127)
else()
  target_compile_options(test_loader PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME test_loader COMMAND test_loader)
