# The loader, plus the Arduino-free parts of the ESPRack core it shares
# with the app: the handoff records, CRC-32, the flash steps.
set(ESPRACK ${CMAKE_CURRENT_LIST_DIR}/../../lib/ESPRack)

idf_component_register(
  SRCS
    loader_main.cpp
    loader_net.cpp
    loader_pull.cpp
    loader_web.cpp
    http_lite.cpp
    image_writer.cpp
    image_rules.cpp
    sig_verify.cpp
    ${ESPRACK}/src/system/OtaHandoff.cpp
    ${ESPRACK}/src/system/FlashSteps.cpp
    ${ESPRACK}/src/persist/Crc32.cpp
  INCLUDE_DIRS . ${ESPRACK}/include
  PRIV_REQUIRES
    app_update bootloader_support esp_app_format esp_event esp_http_server esp_netif
    esp_partition esp_timer esp_wifi lwip mbedtls nvs_flash)

# The loader's own info lines stay in (IDF's are compiled out: warnings
# only); app_main switches its tag to info.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LOG_LOCAL_LEVEL=3)
