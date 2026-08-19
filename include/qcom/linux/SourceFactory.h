/// @file SourceFactory.h
/// @brief Pick platform DIAG IDataSource; Android stays stub until DCI is wired.
#pragma once

#include <memory>

#include <qcom/android/AndroidSource.h>
#include <qcom/protocol/DiagSourceConfig.h>
#include <observer/io/DataSourceInterface.h>

#if defined(__linux__) && !defined(__ANDROID__)
#  include <qcom/linux/LinuxSource.h>
#endif

namespace QCom {

/// Create the best available live DIAG source for this build.
/// On desktop Linux → LinuxSource; on Android NDK → AndroidSource stub.
[[nodiscard]] inline std::unique_ptr<IDataSource> make_diag_source(DiagSourceConfig cfg = {}) {
#if defined(__ANDROID__)
  return std::make_unique<AndroidSource>(std::move(cfg));
#elif defined(__linux__)
  return std::make_unique<LinuxSource>(std::move(cfg));
#else
  return std::make_unique<AndroidSource>(std::move(cfg));
#endif
}

}  // namespace QCom
