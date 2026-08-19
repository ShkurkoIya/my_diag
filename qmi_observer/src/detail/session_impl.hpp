#pragma once

#include <qcom/qmi/callbacks.hpp>
#include <qcom/qmi/error.hpp>
#include <qcom/qmi/health.hpp>
#include <qcom/qmi/session.hpp>
#include <qcom/qmi/settings.hpp>

#include "detail/runtime.hpp"

#include <libqmi-glib.h>

#include <memory>

namespace QCom::Qmi {

class HealthMonitor;
class ModemControl;
class NasReader;
class ScanController;

struct Session::Impl {
  Settings settings;
  Callbacks callbacks;
  ModemHealth last_health{};

  detail::GObjectPtr<QmiDevice> device;
  detail::GObjectPtr<QmiClientDms> dms;
  detail::GObjectPtr<QmiClientNas> nas;
  bool open{false};

  std::unique_ptr<HealthMonitor> health_facade;
  std::unique_ptr<ModemControl> control_facade;
  std::unique_ptr<NasReader> nas_facade;
  std::unique_ptr<ScanController> scan_facade;

  void emit_error(const Error& e) {
    if (callbacks.on_error) {
      callbacks.on_error(e);
    }
  }

  void publish_health(ModemHealth h) {
    const ModemPhase prev = last_health.phase;
    last_health = std::move(h);
    if (callbacks.on_phase_changed && prev != last_health.phase) {
      callbacks.on_phase_changed(prev, last_health.phase);
    }
    if (callbacks.on_health_changed) {
      callbacks.on_health_changed(last_health);
    }
  }
};

}  // namespace QCom::Qmi
