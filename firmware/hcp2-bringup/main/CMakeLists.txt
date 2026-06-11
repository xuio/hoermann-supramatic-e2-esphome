set(HCP2_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../components/hcp2bridge/core")

idf_component_register(
  SRCS
    "hcp2_bringup_main.c"
    "${HCP2_CORE_DIR}/hcp2_crc.c"
    "${HCP2_CORE_DIR}/hcp2_frame.c"
    "${HCP2_CORE_DIR}/hcp2_engine.c"
    "${HCP2_CORE_DIR}/hcp2_mailbox.c"
  INCLUDE_DIRS
    "."
    "${HCP2_CORE_DIR}"
  REQUIRES
    driver
    esp_timer
    esp_system
    nvs_flash
)
