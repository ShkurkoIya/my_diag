cmake_minimum_required(VERSION 3.22)
project(qcom_scanner CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

# =============================================================================
# srsRAN isolated sysroot
# =============================================================================
set(SRSRAN_ROOT "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot")

set(SRSRAN_LIBS
    "${SRSRAN_ROOT}/lib/librrc_asn1.a"
    "${SRSRAN_ROOT}/lib/librrc_nr_asn1.a"
    "${SRSRAN_ROOT}/lib/libasn1_utils.a"
    "${SRSRAN_ROOT}/lib/libsrsran_common.a"
    "${SRSRAN_ROOT}/lib/libsrslog.a"
)

# =============================================================================
# Target 1: qcom_model (INTERFACE — header-only data model, zero deps)
# =============================================================================
add_library(qcom_model INTERFACE)
target_include_directories(qcom_model INTERFACE ${CMAKE_SOURCE_DIR}/src)

# =============================================================================
# Target 2: qcom_protocol (INTERFACE — CRC16, HDLC, DIAG commands, zero OS deps)
# =============================================================================
add_library(qcom_protocol INTERFACE)
target_include_directories(qcom_protocol INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(qcom_protocol INTERFACE qcom_model Threads::Threads)

# =============================================================================
# Target 3: qcom_parser (STATIC — ASN.1 + binary parsers, depends on srsRAN)
# =============================================================================
add_library(qcom_parser STATIC
    src/core/QualcomParser.cpp
    src/lte/LteParser.cpp
    src/lte/LteQcomBinary.cpp
    src/nr/NrParser.cpp
    src/gsm/GsmParser.cpp
    src/wcdma/WcdmaParser.cpp
)

target_include_directories(qcom_parser PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${SRSRAN_ROOT}/include
)

target_compile_options(qcom_parser PRIVATE
    -Wno-error -Wno-stringop-overflow -Wno-unused-parameter
    -Wno-unused-variable -Wno-deprecated-declarations -Wno-deprecated
)

target_link_libraries(qcom_parser PUBLIC
    qcom_model
    ${SRSRAN_LIBS}
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

# =============================================================================
# Executable: scanner (CLI demo)
# =============================================================================
add_executable(scanner app/main.cpp)
target_link_libraries(scanner PRIVATE qcom_parser)

# =============================================================================
# Tests: Catch2 via FetchContent
# =============================================================================
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.8.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    add_executable(tests
        tests/test_utils.cpp
        tests/test_lte_binary.cpp
        tests/test_cell_tracker.cpp
        tests/test_lte_asn1.cpp
        tests/test_protocol.cpp
    )

    target_link_libraries(tests PRIVATE
        qcom_parser
        qcom_protocol
        Catch2::Catch2WithMain
    )

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(CTest)
    include(Catch)
    catch_discover_tests(tests)
endif()
