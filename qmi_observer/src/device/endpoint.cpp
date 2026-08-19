#include <qcom/qmi/device/endpoint.hpp>

#include <qcom/qmi/device/profile.hpp>

#include <sstream>

namespace QCom::Qmi::device {

std::string make_endpoint_id(uint16_t vid, uint16_t pid, std::string_view serial,
                             std::string_view usb_path, bool& serial_stable) {
  std::ostringstream oss;
  oss << std::hex << vid << ':' << pid << std::dec;
  // Известные заглушки serial у Qualcomm/SimCom
  const bool fake = serial.empty() || serial == "0123456789ABCDEF" || serial == "1234567890ABCDEF";
  if (!fake) {
    serial_stable = true;
    oss << ':' << serial;
  } else {
    serial_stable = false;
    oss << "@" << usb_path;
  }
  return oss.str();
}

std::optional<std::string> ModemEndpoint::qmi_path() const {
  for (const auto& p : ports) {
    if (p.role == PortRole::Qmi) {
      return p.path;
    }
  }
  return std::nullopt;
}

std::optional<std::string> ModemEndpoint::preferred_at_path() const {
  for (const auto& p : ports) {
    if (p.role == PortRole::At) {
      return p.path;
    }
  }
  return std::nullopt;
}

std::optional<std::string> ModemEndpoint::preferred_diag_path() const {
  for (const auto& p : ports) {
    if (p.role == PortRole::Diag) {
      return p.path;
    }
  }
  // Profile miss / unbound roles: Qualcomm DIAG is USB interface 0.
  std::optional<std::string> iface0;
  std::optional<std::string> first_tty;
  for (const auto& p : ports) {
    if (p.role == PortRole::Qmi || p.role == PortRole::Mbim) continue;
    if (p.path.find("tty") == std::string::npos) continue;
    if (!first_tty) first_tty = p.path;
    if (p.usb_interface && *p.usb_interface == 0) iface0 = p.path;
  }
  return iface0 ? iface0 : first_tty;
}

std::vector<std::string> ModemEndpoint::paths_with_role(PortRole role) const {
  std::vector<std::string> out;
  for (const auto& p : ports) {
    if (p.role == role) {
      out.push_back(p.path);
    }
  }
  return out;
}

Settings ModemEndpoint::to_qmi_settings(const ModemProfile* profile) const {
  Settings s;
  if (auto q = qmi_path()) {
    s.device_path = *q;
  }
  if (profile) {
    s.use_proxy = profile->quirks.prefer_qmi_proxy;
    s.open_auto = profile->quirks.open_auto;
    s.allow_dms_offline = profile->quirks.allow_dms_offline;
    s.preferred_modes = profile->preferred_modes;
  } else {
    s.use_proxy = false;
    s.allow_dms_offline = false;
  }
  return s;
}

}  // namespace QCom::Qmi::device
