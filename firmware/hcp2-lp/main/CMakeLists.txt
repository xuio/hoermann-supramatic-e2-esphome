set(HCP2_CORE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../components/hcp2bridge/core")
set(HCP2_IDF_LP_CORE_PRIVATE_INCLUDE "$ENV{IDF_PATH}/components/ulp/lp_core/lp_core/include")

idf_component_register(
  SRCS "hcp2_lp_main.c" "${HCP2_CORE_DIR}/hcp2_mailbox.c"
  INCLUDE_DIRS "." "lp_core" "${HCP2_CORE_DIR}" "${HCP2_IDF_LP_CORE_PRIVATE_INCLUDE}"
  REQUIRES ulp
)

set(ulp_app_name hcp2_lp)
set(ulp_lp_core_sources
  "lp_core/hcp2_lp_core.c"
  "${HCP2_CORE_DIR}/hcp2_crc.c"
  "${HCP2_CORE_DIR}/hcp2_frame.c"
  "${HCP2_CORE_DIR}/hcp2_engine.c"
  "${HCP2_CORE_DIR}/hcp2_mailbox.c"
  "${HCP2_CORE_DIR}/hcp2_responder_runtime.c"
)
set(ulp_exp_dep_srcs "hcp2_lp_main.c")

ulp_embed_binary(${ulp_app_name} "${ulp_lp_core_sources}" "${ulp_exp_dep_srcs}")

set(hcp2_lp_artifact_dir "${CMAKE_CURRENT_BINARY_DIR}/${ulp_app_name}")
set(hcp2_lp_root_artifacts
  "${CMAKE_BINARY_DIR}/hcp2_lp.bin"
  "${CMAKE_BINARY_DIR}/hcp2_lp.elf"
  "${CMAKE_BINARY_DIR}/hcp2_lp.map"
  "${CMAKE_BINARY_DIR}/hcp2_lp.sym"
)

add_custom_command(
  OUTPUT ${hcp2_lp_root_artifacts}
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${hcp2_lp_artifact_dir}/hcp2_lp.bin" "${CMAKE_BINARY_DIR}/hcp2_lp.bin"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${hcp2_lp_artifact_dir}/hcp2_lp.elf" "${CMAKE_BINARY_DIR}/hcp2_lp.elf"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${hcp2_lp_artifact_dir}/hcp2_lp.map" "${CMAKE_BINARY_DIR}/hcp2_lp.map"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${hcp2_lp_artifact_dir}/hcp2_lp.sym" "${CMAKE_BINARY_DIR}/hcp2_lp.sym"
  DEPENDS ${ulp_app_name}
  VERBATIM
)

add_custom_target(copy_hcp2_lp_artifacts ALL DEPENDS ${hcp2_lp_root_artifacts})
