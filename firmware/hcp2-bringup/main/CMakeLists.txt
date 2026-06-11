set(HCP2_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../components/hcp2bridge/core")
set(HCP2_LP_MAIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../../hcp2-lp/main")
set(HCP2_IDF_LP_CORE_PRIVATE_INCLUDE "$ENV{IDF_PATH}/components/ulp/lp_core/lp_core/include")

idf_component_register(
  SRCS
    "hcp2_bringup_main.c"
    "${HCP2_CORE_DIR}/hcp2_crc.c"
    "${HCP2_CORE_DIR}/hcp2_frame.c"
    "${HCP2_CORE_DIR}/hcp2_engine.c"
    "${HCP2_CORE_DIR}/hcp2_mailbox.c"
    "${HCP2_CORE_DIR}/hcp2_supervisor.c"
  INCLUDE_DIRS
    "."
    "${HCP2_CORE_DIR}"
    "${HCP2_LP_MAIN_DIR}/lp_core"
    "${HCP2_IDF_LP_CORE_PRIVATE_INCLUDE}"
  REQUIRES
    driver
    esp_timer
    esp_system
    nvs_flash
    ulp
)

set(ulp_app_name hcp2_lp)
set(ulp_lp_core_sources
  "${HCP2_LP_MAIN_DIR}/lp_core/hcp2_lp_core.c"
  "${HCP2_CORE_DIR}/hcp2_crc.c"
  "${HCP2_CORE_DIR}/hcp2_frame.c"
  "${HCP2_CORE_DIR}/hcp2_engine.c"
  "${HCP2_CORE_DIR}/hcp2_mailbox.c"
)
set(ulp_exp_dep_srcs "hcp2_bringup_main.c")

ulp_embed_binary(${ulp_app_name} "${ulp_lp_core_sources}" "${ulp_exp_dep_srcs}")
