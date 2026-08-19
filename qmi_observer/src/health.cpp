#include <qcom/qmi/health.hpp>

#include <sstream>

namespace QCom::Qmi {

std::string_view to_string(ModemPhase p) noexcept {
  switch (p) {
    case ModemPhase::Absent: return "absent";
    case ModemPhase::Unavailable: return "unavailable";
    case ModemPhase::OfflineRf: return "offline_rf";
    case ModemPhase::OnlineIdle: return "online_idle";
    case ModemPhase::Searching: return "searching";
    case ModemPhase::Camped: return "camped";
    case ModemPhase::Fault: return "fault";
  }
  return "unknown";
}

std::string_view to_string(RegistrationKind r) noexcept {
  switch (r) {
    case RegistrationKind::Unknown: return "unknown";
    case RegistrationKind::NotRegistered: return "not_registered";
    case RegistrationKind::Searching: return "searching";
    case RegistrationKind::Registered: return "registered";
    case RegistrationKind::Denied: return "denied";
  }
  return "unknown";
}

std::string_view to_string(OperatingModeKind m) noexcept {
  switch (m) {
    case OperatingModeKind::Unknown: return "unknown";
    case OperatingModeKind::Online: return "online";
    case OperatingModeKind::LowPower: return "low_power";
    case OperatingModeKind::Offline: return "offline";
    case OperatingModeKind::Reset: return "reset";
    case OperatingModeKind::Other: return "other";
  }
  return "unknown";
}

bool mode_preference_covers(const std::vector<Rat>& have, const std::vector<Rat>& want) noexcept {
  if (want.empty()) {
    return true;
  }
  for (Rat w : want) {
    bool found = false;
    for (Rat h : have) {
      if (h == w) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

ModemHealth evaluate_health(HealthProbe probe) {
  ModemHealth h;
  h.probe = std::move(probe);
  auto& p = h.probe;

  if (!p.session_open || !p.device_node_present) {
    h.phase = p.device_node_present ? ModemPhase::Unavailable : ModemPhase::Absent;
    h.needs_recover = false;
    h.summary = "session/device not available";
    return h;
  }

  if (p.operating_mode == OperatingModeKind::Offline ||
      p.operating_mode == OperatingModeKind::Reset ||
      p.operating_mode == OperatingModeKind::LowPower) {
    h.phase = ModemPhase::OfflineRf;
    h.needs_recover = true;
    h.can_snapshot = false;
    h.can_force_search = false;
    h.can_change_ssp = false;
    h.summary = std::string{"RF not online ("} + std::string{to_string(p.operating_mode)} + ")";
    return h;
  }

  if (p.operating_mode != OperatingModeKind::Online) {
    h.phase = ModemPhase::Fault;
    h.needs_recover = true;
    h.summary = "unknown/unsupported operating mode";
    return h;
  }

  // Online RF.
  const bool sim_blocks = p.sim_known && !p.sim_ready;
  h.can_change_ssp = !sim_blocks;
  h.can_force_search = !sim_blocks;

  if (p.last_cell_location_ok ||
      (p.radio && *p.radio != Rat::Unknown &&
       (p.registration == RegistrationKind::Registered ||
        p.registration == RegistrationKind::Denied))) {
    h.phase = ModemPhase::Camped;
    h.can_snapshot = true;
    h.summary = "camped";
    if (p.registration == RegistrationKind::Denied) {
      h.summary = "camped (registration denied / limited — snapshot still OK)";
    }
    return h;
  }

  if (p.registration == RegistrationKind::Searching || p.last_cell_location_no_network) {
    h.phase = ModemPhase::Searching;
    h.can_snapshot = true;  // may return NoNetwork; caller decides
    h.summary = "searching (NoNetworkFound expected until camp)";
    return h;
  }

  h.phase = ModemPhase::OnlineIdle;
  h.can_snapshot = true;
  h.can_force_search = !sim_blocks;
  h.summary = "online, idle/unclassified";
  if (sim_blocks) {
    h.summary += " (SIM not ready)";
    h.can_change_ssp = false;
    h.can_force_search = false;
  }
  return h;
}

}  // namespace QCom::Qmi
