# esprack-loader: the OTA loader of the `loader` partition scheme
# (docs/plans/lib-ota-loader.md). A plain ESP-IDF project, built per chip
# with PlatformIO (platformio.ini) or idf.py.
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.0")
# A dev-key loader says so in its version: the app reads it from the
# factory partition (OtaLoader::devKey(), "devkey") and warns. From the
# generated key header, re-read when it changes.
set(_pubkey "${CMAKE_CURRENT_LIST_DIR}/main/ota_pubkey.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_pubkey}")
file(STRINGS "${_pubkey}" _devkey REGEX "^#define ESPRACK_OTA_KEY_DEV +1")
if(_devkey)
  string(APPEND PROJECT_VER "+devkey")
endif()
# Only main and what it needs: no unused components in the image.
set(COMPONENTS main)
# PlatformIO's builder reads its flags from a stub component it adds.
if(EXISTS "$ENV{IDF_PATH}/components/__pio_env")
  list(APPEND COMPONENTS __pio_env)
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esprack-loader)
