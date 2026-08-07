#pragma once

#pragma once

#include "qmi_observer/callbacks.hpp"
#include "qmi_observer/error.hpp"
#include "qmi_observer/health.hpp"
#include "qmi_observer/session.hpp"
#include "qmi_observer/settings.hpp"

#include "detail/runtime.hpp"

#include <libqmi-glib.h>

#include <memory>

namespace qmi_observer {

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

}  // namespace qmi_observer
