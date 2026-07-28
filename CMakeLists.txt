cmake_minimum_required(VERSION 3.22)
project(qcom_scanner CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Suppress warnings from srsRAN headers under GCC 16 / C++23
add_compile_options(
    -Wno-error -Wno-stringop-overflow -Wno-unused-parameter
    -Wno-unused-variable -Wno-deprecated-declarations -Wno-deprecated
)

# =============================================================================
# srsRAN isolated sysroot
# =============================================================================
set(SRSRAN_ROOT "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot")
set(SRSRAN_INCLUDE_DIR "${SRSRAN_ROOT}/include")

set(SRSRAN_LIBS
    "${SRSRAN_ROOT}/lib/librrc_asn1.a"
    "${SRSRAN_ROOT}/lib/librrc_nr_asn1.a"
    "${SRSRAN_ROOT}/lib/libasn1_utils.a"
    "${SRSRAN_ROOT}/lib/libsrsran_common.a"
    "${SRSRAN_ROOT}/lib/libsrslog.a"
)

# =============================================================================
# Library: qcom_parser (the core reusable component)
# =============================================================================
add_library(qcom_parser STATIC
    src/core/QualcomParser.cpp
    src/lte/LteParser.cpp
    src/lte/LteQcomBinary.cpp
    src/nr/NrParser.cpp
)

target_include_directories(qcom_parser PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${SRSRAN_INCLUDE_DIR}
)

target_link_libraries(qcom_parser PUBLIC
    ${SRSRAN_LIBS}
    pthread m dl
)

# =============================================================================
# Executable: scanner (CLI demo / manual testing)
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
    )

    target_link_libraries(tests PRIVATE
        qcom_parser
        Catch2::Catch2WithMain
    )

    include(CTest)
    include(Catch)
    catch_discover_tests(tests)
endif()
