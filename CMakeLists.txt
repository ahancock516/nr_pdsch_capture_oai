# CMakeLists.txt — nr_pdsch_capture OAI nrUE plugin
#
# Builds libpdsch_capture.so, loaded at runtime via dlopen("libpdsch_capture.so").
#
# Integrate into OAI by adding to the root CMakeLists.txt:
#   add_subdirectory(/path/to/nr_pdsch_capture_oai
#                    ${CMAKE_BINARY_DIR}/nr_pdsch_capture)

cmake_minimum_required(VERSION 3.10)

add_library(pdsch_capture MODULE
    src/nr_pdsch_capture.c
)

# Mirror the include paths used by SCHED_NR_UE_LIB so all OAI headers resolve.
target_include_directories(pdsch_capture PRIVATE
    ${OPENAIR_DIR}
    ${OPENAIR1_DIR}
    ${OPENAIR2_DIR}
    ${OPENAIR_DIR}/openair3/S1AP
    ${OPENAIR_DIR}/openair3/NGAP
    ${OPENAIR_DIR}/openair3/COMMON
    ${OPENAIR_DIR}/radio/COMMON
    ${CMAKE_BINARY_DIR}
    ${OPENAIR_DIR}/executables
    ${OPENAIR_DIR}/openair2/COMMON
    ${OPENAIR_DIR}/openair2/UTIL
    ${NFAPI_DIR}/nfapi/public_inc
    ${NFAPI_DIR}/common/public_inc
    ${NFAPI_DIR}/nfapi/inc
    ${NFAPI_DIR}/oai_integration
    ${OPENAIR_DIR}/openair2/NR_UE_PHY_INTERFACE
    ${OPENAIR_DIR}/openair2/LAYER2
    ${OPENAIR1_DIR}/SCHED_NR_UE
    ${OPENAIR_DIR}/common/utils
    ${OPENAIR_DIR}/common/utils/barrier
    ${OPENAIR_DIR}/common/utils/LOG
    ${OPENAIR_DIR}/common/utils/threadPool
    ${OPENAIR_DIR}/common/utils/ocp_itti
    ${OPENAIR_DIR}/common/utils/actor
    ${OPENAIR_DIR}/common/utils/T
    ${CMAKE_BINARY_DIR}/common/utils/T
    ${OPENAIR_DIR}/common
    ${CMAKE_BINARY_DIR}/openair2/RRC/NR/MESSAGES
    ${CMAKE_BINARY_DIR}/openair2/RRC/LTE/MESSAGES
)

target_compile_definitions(pdsch_capture PRIVATE
    ${T_TRACER_DEF}
    MAX_NUM_CCs=1
)

target_link_libraries(pdsch_capture PRIVATE pthread)

set_target_properties(pdsch_capture PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    OUTPUT_NAME pdsch_capture
)

message(STATUS "PDSCH capture plugin: libpdsch_capture.so -> ${CMAKE_BINARY_DIR}")
