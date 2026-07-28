cmake_minimum_required(VERSION 3.22)
project(qcom_scanner CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Suppress warnings from old srsRAN code under GCC 16 / C++23
add_compile_options(
    -Wno-error
    -Wno-stringop-overflow
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-deprecated-declarations
    -Wno-deprecated
)

# srsRAN isolated sysroot paths
set(SRSRAN_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot/include")
set(SRSRAN_ASN1_LIB    "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot/lib/librrc_asn1.a")
set(SRSRAN_NR_ASN1_LIB "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot/lib/librrc_nr_asn1.a")
set(SRSRAN_UTILS_LIB   "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot/lib/libasn1_utils.a")
set(SRSRAN_COMMON_LIB  "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot/lib/libsrsran_common.a")
set(SRSRAN_LOG_LIB     "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot/lib/libsrslog.a")

add_executable(scanner
  main.cpp
  QualcomParser.cpp
  lte/LteParser.cpp
  lte/LteQcomBinary.cpp
  nr/NrParser.cpp
)

target_compile_features(scanner PRIVATE cxx_std_23)
target_include_directories(scanner PRIVATE
  ${CMAKE_SOURCE_DIR}
  ${SRSRAN_INCLUDE_DIR}
)

# Link order matters: high-level RRC -> ASN.1 utils -> common -> srslog -> system
target_link_libraries(scanner PRIVATE
  ${SRSRAN_ASN1_LIB}
  ${SRSRAN_NR_ASN1_LIB}
  ${SRSRAN_UTILS_LIB}
  ${SRSRAN_COMMON_LIB}
  ${SRSRAN_LOG_LIB}
  pthread
  m
  dl
)
