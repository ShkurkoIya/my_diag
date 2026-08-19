#pragma once

/**
 * @file health_monitor.hpp
 * @brief Probes DMS/NAS/UIM and evaluates @ref ModemHealth.
 */

#include <qcom/qmi/error.hpp>
#include <qcom/qmi/health.hpp>

namespace QCom::Qmi {

class Session;

class HealthMonitor {
 public:
  explicit HealthMonitor(Session& session);

  /// Fill @ref HealthProbe from the live modem and run @ref evaluate_health.
  [[nodiscard]] Result<ModemHealth> refresh();

 private:
  Session& session_;
};

}  // namespace QCom::Qmi
