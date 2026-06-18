set(HCP2_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../components/hcp2bridge/core")

idf_component_register(
  SRCS
    "hcp2_c6_dma_probe_main.c"
    "${HCP2_CORE_DIR}/hcp2_crc.c"
    "${HCP2_CORE_DIR}/hcp2_frame.c"
    "${HCP2_CORE_DIR}/hcp2_engine.c"
  INCLUDE_DIRS
    "."
    "${HCP2_CORE_DIR}"
  REQUIRES
    driver
    esp_driver_uart
    esp_timer
    ulp
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
  HCP2_ENABLE_PROTOCOL_EVENTS=0
  HCP2_ESP32_REALTIME_HOT_PATH=1
)
