cmake_minimum_required(VERSION 3.22)
project(observer CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

include(FetchContent)

# App/tools JSON + CLI + formatting (not srsRAN's bundled fmt).
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG        11.2.0
    GIT_SHALLOW    TRUE
)
set(CLI11_PRECOMPILED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(CLI11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.5.0
    GIT_SHALLOW    TRUE
)
set(glaze_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG        v7.9.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(fmt CLI11 glaze)

set(SRSRAN_ROOT "${CMAKE_SOURCE_DIR}/thirdparty/srsran_sysroot")
set(SRSRAN_LIBS
    "${SRSRAN_ROOT}/lib/librrc_asn1.a"
    "${SRSRAN_ROOT}/lib/librrc_nr_asn1.a"
    "${SRSRAN_ROOT}/lib/libasn1_utils.a"
    "${SRSRAN_ROOT}/lib/libsrsran_common.a"
    "${SRSRAN_ROOT}/lib/libsrslog.a"
)

# -----------------------------------------------------------------------------
# observer::model — 3GPP/RF domain (not Qualcomm)
# -----------------------------------------------------------------------------
add_library(observer_model INTERFACE)
add_library(observer::model ALIAS observer_model)
target_include_directories(observer_model INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# -----------------------------------------------------------------------------
# qcom::protocol — DIAG/HDLC wire
# -----------------------------------------------------------------------------
add_library(qcom_protocol INTERFACE)
add_library(qcom::protocol ALIAS qcom_protocol)
target_include_directories(qcom_protocol INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(qcom_protocol INTERFACE observer::model Threads::Threads)

# -----------------------------------------------------------------------------
# qcom::parser — log-code parsers; srsRAN stays PRIVATE
# -----------------------------------------------------------------------------
add_library(qcom_parser STATIC
    src/core/QualcomParser.cpp
    src/lte/LteParser.cpp
    src/lte/LteQcomBinary.cpp
    src/nr/NrParser.cpp
    src/gsm/GsmParser.cpp
    src/wcdma/WcdmaParser.cpp
)
add_library(qcom::parser ALIAS qcom_parser)
target_include_directories(qcom_parser
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${SRSRAN_ROOT}/include
)
target_compile_options(qcom_parser PRIVATE
    -Wno-error -Wno-stringop-overflow -Wno-unused-parameter
    -Wno-unused-variable -Wno-deprecated-declarations -Wno-deprecated
)
target_link_libraries(qcom_parser
    PUBLIC
        observer::model
        qcom::protocol
        ${SRSRAN_LIBS}
        Threads::Threads
        ${CMAKE_DL_LIBS}
)

# -----------------------------------------------------------------------------
# observer::engine — survey facade (ingest still uses qcom parser)
# -----------------------------------------------------------------------------
add_library(observer_engine INTERFACE)
add_library(observer::engine ALIAS observer_engine)
target_include_directories(observer_engine INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(observer_engine INTERFACE observer::model qcom::parser)

# -----------------------------------------------------------------------------
# qcom::io — Linux/Android/journal + SIMCOM AT
# -----------------------------------------------------------------------------
add_library(qcom_io INTERFACE)
add_library(qcom::io ALIAS qcom_io)
target_include_directories(qcom_io INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(qcom_io INTERFACE qcom::protocol observer::engine)

# -----------------------------------------------------------------------------
# qcom::qmi — QMI NAS (impl in qmi_observer/, headers include/qcom/qmi/)
# -----------------------------------------------------------------------------
option(BUILD_QMI_OBSERVER "Build qcom::qmi (libqmi NAS adapter)" ON)
if(BUILD_QMI_OBSERVER)
    add_subdirectory(qmi_observer)
endif()

# -----------------------------------------------------------------------------
# Observer process
# -----------------------------------------------------------------------------
add_executable(observer_app
    app/main.cpp
    app/observer/Options.cpp
    app/observer/Runtime.cpp
    app/observer/SurveyHop.cpp
    app/observer/SurveyProc.cpp
)
add_executable(observer::app ALIAS observer_app)
add_executable(scanner ALIAS observer_app)
add_executable(live_scanner ALIAS observer_app)

target_include_directories(observer_app PRIVATE
    ${CMAKE_SOURCE_DIR}/app
    ${CMAKE_SOURCE_DIR}/tools/journal_replay
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(observer_app PRIVATE
    observer::engine
    qcom::io
    qcom::parser
    qcom::protocol
    glaze::glaze
    CLI11::CLI11
    fmt::fmt
)
if(TARGET qcom::qmi)
    target_link_libraries(observer_app PRIVATE qcom::qmi)
else()
    message(FATAL_ERROR "observer_app needs qcom::qmi (enable BUILD_QMI_OBSERVER)")
endif()
set_target_properties(observer_app PROPERTIES OUTPUT_NAME live_scanner)

# -----------------------------------------------------------------------------
# Offline tools
# -----------------------------------------------------------------------------
option(BUILD_TOOLS "Build offline tools (journal_replay)" ON)

if(BUILD_TOOLS)
    add_executable(journal_replay tools/journal_replay/main.cpp)
    target_include_directories(journal_replay PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/journal_replay
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(journal_replay PRIVATE observer::engine qcom::parser glaze::glaze fmt::fmt)

    add_executable(dump_diag_codes tools/dump_diag_codes.cpp)
    target_link_libraries(dump_diag_codes PRIVATE qcom::io qcom::parser qcom::protocol)

    add_executable(synth_demo tools/synth_demo.cpp)
    target_link_libraries(synth_demo PRIVATE qcom::parser)
endif()

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.8.0
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)

    set(_test_sources
        tests/test_utils.cpp
        tests/test_lte_binary.cpp
        tests/test_nr_binary.cpp
        tests/test_wcdma_binary.cpp
        tests/test_cell_tracker.cpp
        tests/test_lte_asn1.cpp
        tests/test_at_cereg_parse.cpp
        tests/test_at_cmgrmi.cpp
        tests/test_protocol.cpp
        tests/test_dump_fixtures.cpp
        tests/test_lte_hop_planner.cpp
        tests/test_survey_engine.cpp
        tests/test_options.cpp
        app/observer/Options.cpp
    )
    if(BUILD_QMI_OBSERVER)
        list(APPEND _test_sources
            qmi_observer/tests/test_qmi_observer.cpp
            qmi_observer/tests/test_device_catalog.cpp
        )
    endif()

    add_executable(tests ${_test_sources})
    target_include_directories(tests PRIVATE
        ${CMAKE_SOURCE_DIR}/app
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/tools/journal_replay
        ${SRSRAN_ROOT}/include
    )
    target_link_libraries(tests PRIVATE
        observer::engine
        qcom::parser
        qcom::protocol
        qcom::io
        Catch2::Catch2WithMain
        fmt::fmt
        CLI11::CLI11
        glaze::glaze
    )
    if(BUILD_QMI_OBSERVER)
        target_link_libraries(tests PRIVATE qcom::qmi)
    endif()

    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(CTest)
    include(Catch)
    catch_discover_tests(tests)
endif()
