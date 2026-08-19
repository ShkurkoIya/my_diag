#pragma once

/**
 * @file callbacks.hpp
 * @brief Optional observer hooks for Session / ScanController progress.
 */

#include <qcom/qmi/error.hpp>
#include <qcom/qmi/health.hpp>
#include <qcom/qmi/types.hpp>

#include <functional>

namespace QCom::Qmi {

struct Callbacks {
  std::function<void(const ModemHealth&)> on_health_changed;
  std::function<void(ModemPhase from, ModemPhase to)> on_phase_changed;
  std::function<void(const Error&)> on_error;
  std::function<void(const CellSnapshot&)> on_snapshot;
};

}  // namespace QCom::Qmi
