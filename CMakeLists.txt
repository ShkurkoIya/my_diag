cmake_minimum_required(VERSION 3.22)
project(srsran_test_parser CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Глобальные флаги подавления варнингов старого кода srsRAN под GCC 16 / C++23
add_compile_options(
    -Wno-error
    -Wno-stringop-overflow
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-deprecated-declarations # Отключает спам про std::aligned_storage
    -Wno-deprecated               # Отключает спам про литералы C++23 и пробелы
)

# Пути к изолированному sysroot радиостека
set(SRSRAN_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot/include")
set(SRSRAN_ASN1_LIB    "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot/lib/librrc_asn1.a")
set(SRSRAN_UTILS_LIB   "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot/lib/libasn1_utils.a")
set(SRSRAN_COMMON_LIB  "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot/lib/libsrsran_common.a")
set(SRSRAN_LOG_LIB     "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot/lib/libsrslog.a")

# Создаем исполняемый таргет
add_executable(test_parser
  main.cpp
  QualcomParser.cpp
)

target_compile_features(test_parser PRIVATE cxx_std_23)
target_include_directories(test_parser PRIVATE ${SRSRAN_INCLUDE_DIR})

# СТРОГИЙ ПОРЯДОК ЛИНКОВКИ: От высокоуровневого RRC до низкоуровневых логов и системных либ
target_link_libraries(test_parser PRIVATE
  ${SRSRAN_ASN1_LIB}      # LTE RRC
  ${SRSRAN_UTILS_LIB}     # ASN1 bit_ref / json_writer
  ${SRSRAN_COMMON_LIB}    # Базовые утилиты srsRAN
  ${SRSRAN_LOG_LIB}       # Внутренний srslog (в самый низ перед системными!)
  pthread
  m
  dl
)
