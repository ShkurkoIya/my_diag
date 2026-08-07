#pragma once

/**
 * @file callbacks.hpp
 * @brief Optional observer hooks for Session / ScanController progress.
 */

#include "qmi_observer/error.hpp"
#include "qmi_observer/health.hpp"
#include "qmi_observer/types.hpp"

#include <functional>

namespace qmi_observer {

struct Callbacks {
  std::function<void(const ModemHealth&)> on_health_changed;
  std::function<void(ModemPhase from, ModemPhase to)> on_phase_changed;
  std::function<void(const Error&)> on_error;
  std::function<void(const CellSnapshot&)> on_snapshot;
};

}  // namespace qmi_observer
