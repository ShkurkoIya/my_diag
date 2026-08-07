#pragma once

/**
 * @file health_monitor.hpp
 * @brief Probes DMS/NAS/UIM and evaluates @ref ModemHealth.
 */

#include "qmi_observer/error.hpp"
#include "qmi_observer/health.hpp"

namespace qmi_observer {

class Session;

class HealthMonitor {
 public:
  explicit HealthMonitor(Session& session);

  /// Fill @ref HealthProbe from the live modem and run @ref evaluate_health.
  [[nodiscard]] Result<ModemHealth> refresh();

 private:
  Session& session_;
};

}  // namespace qmi_observer
