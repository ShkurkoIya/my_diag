cmake_minimum_required(VERSION 3.22)

project(qmi_observer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(QMI_OBSERVER_BUILD_EXAMPLES "Build qmi_observer examples" ON)

find_package(PkgConfig REQUIRED)

set(_qmi_observer_sysroot "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/libqmi_sysroot")
if(EXISTS "${_qmi_observer_sysroot}/usr/lib64/pkgconfig/qmi-glib.pc")
  set(ENV{PKG_CONFIG_PATH}
      "${_qmi_observer_sysroot}/usr/lib64/pkgconfig:$ENV{PKG_CONFIG_PATH}")
endif()

pkg_check_modules(QMI_GLIB REQUIRED IMPORTED_TARGET qmi-glib)

if(NOT TARGET qcom_model)
  add_library(qcom_model INTERFACE)
  target_include_directories(qcom_model INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  )
endif()

add_library(qmi_observer STATIC
  src/error.cpp
  src/version.cpp
  src/types.cpp
  src/health.cpp
  src/session.cpp
  src/health_monitor.cpp
  src/modem_control.cpp
  src/nas_reader.cpp
  src/scan_controller.cpp
  src/bridge.cpp
  src/device/profile_registry.cpp
  src/device/endpoint.cpp
  src/device/sysfs_enumerate.cpp
  src/device/dossier_store.cpp
  src/device/probe.cpp
  src/device/catalog.cpp
)

add_library(qmi_observer::qmi_observer ALIAS qmi_observer)

target_include_directories(qmi_observer
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(qmi_observer
  PUBLIC
    PkgConfig::QMI_GLIB
    qcom_model
)

target_compile_options(qmi_observer PRIVATE
  -Wall -Wextra -Wpedantic
)

if(QMI_OBSERVER_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()
