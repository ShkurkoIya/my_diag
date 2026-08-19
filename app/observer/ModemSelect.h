#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <qcom/qmi/device/catalog.hpp>

namespace Observer {

struct SelectedModem {
  std::string id;
  std::string diag;
  std::optional<std::string> qmi;
  std::optional<std::string> at;
};

inline bool wwan_module_loaded(const char* name) {
  return std::filesystem::exists(std::string("/sys/module/") + name);
}

inline bool option_ko_installed() {
  utsname u{};
  if (::uname(&u) != 0) return false;
  const std::string base =
      std::string("/lib/modules/") + u.release + "/kernel/drivers/usb/serial/option.ko";
  return std::filesystem::exists(base) || std::filesystem::exists(base + ".xz") ||
         std::filesystem::exists(base + ".zst");
}

[[nodiscard]] inline bool device_user_rw(const std::string& path) {
  return ::access(path.c_str(), R_OK | W_OK) == 0;
}

/// qmi_wwan nodes are often root:root 0600 after plug. dialout is not enough.
/// chmod locally (works if we are root); otherwise one polkit `chmod a+rw` when a
/// display session can show the agent. Path must be `/dev/cdc-wdm*`.
inline bool try_make_qmi_rw(const std::string& path) {
  if (path.find("/dev/cdc-wdm") != 0) return false;
  if (device_user_rw(path)) return true;
  (void)::chmod(path.c_str(), 0666);
  if (device_user_rw(path)) return true;
  const bool allow_pkexec = std::getenv("OBSERVER_SKIP_QMI_PKEXEC") == nullptr;
  const bool have_ui = std::getenv("DISPLAY") || std::getenv("WAYLAND_DISPLAY");
  if (!allow_pkexec || !have_ui || ::geteuid() == 0) return device_user_rw(path);

  std::cerr << "QMI " << path << " is not user-rw — polkit chmod a+rw…\n";
  const pid_t pid = ::fork();
  if (pid < 0) return false;
  if (pid == 0) {
    execlp("pkexec", "pkexec", "chmod", "a+rw", path.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  int st = 0;
  if (::waitpid(pid, &st, 0) < 0) return false;
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) return false;
  return device_user_rw(path);
}

/// Load option/qmi_wwan if missing. Succeeds only with CAP_SYS_MODULE (root/pkexec).
inline void try_load_wwan_modules() {
  if (wwan_module_loaded("option") && wwan_module_loaded("qmi_wwan")) return;
  (void)::system("modprobe option usb_wwan qmi_wwan >/dev/null 2>&1");
}

inline void print_unbound_hint(const QCom::Qmi::device::ModemEndpoint& ep) {
  std::cerr << "USB " << ep.id << " has no ttyUSB";
  utsname u{};
  if (::uname(&u) == 0 && !option_ko_installed()) {
    std::cerr << " — sudo dnf install kernel-modules-" << u.release
              << " && sudo modprobe option qmi_wwan";
  } else {
    std::cerr << " — sudo modprobe option usb_wwan qmi_wwan";
  }
  std::cerr << "\n";
}

inline bool pick_from_catalog(QCom::Qmi::device::DeviceCatalog& catalog,
                              const std::optional<std::string>& diag_override,
                              const std::optional<std::string>& qmi_override, SelectedModem& out) {
  for (const auto& ep : catalog.endpoints()) {
    auto diag = diag_override ? diag_override : ep.preferred_diag_path();
    if (!diag || diag->empty()) continue;
    out.id = ep.id;
    out.diag = *diag;
    out.at = ep.preferred_at_path();
    if (qmi_override && !qmi_override->empty()) {
      out.qmi = *qmi_override;
    } else {
      out.qmi = ep.qmi_path();
    }
    std::cout << "Selected modem " << ep.id << " diag=" << out.diag;
    if (out.at) std::cout << " at=" << *out.at;
    if (out.qmi) std::cout << " qmi=" << *out.qmi;
    std::cout << "\n";
    return true;
  }
  return false;
}

inline std::optional<SelectedModem> resolve_modem(const std::optional<std::string>& diag_override,
                                           const std::optional<std::string>& qmi_override) {
  QCom::Qmi::device::DeviceCatalog catalog;
  auto refreshed = catalog.refresh();
  if (!refreshed) {
    std::cerr << "catalog refresh failed: " << refreshed.error().message << "\n";
    return std::nullopt;
  }

  SelectedModem out;
  if (pick_from_catalog(catalog, diag_override, qmi_override, out)) return out;

  try_load_wwan_modules();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  refreshed = catalog.refresh();
  if (refreshed && pick_from_catalog(catalog, diag_override, qmi_override, out)) return out;

  if (diag_override && !diag_override->empty()) {
    out.diag = *diag_override;
    out.qmi = qmi_override;
    out.id = "manual";
    return out;
  }

  if (!catalog.endpoints().empty()) {
    for (const auto& ep : catalog.endpoints()) print_unbound_hint(ep);
  } else {
    std::cerr << "No modem with Diag port found. Pass --device /dev/ttyUSBx\n";
  }
  return std::nullopt;
}

inline int list_modems() {
  QCom::Qmi::device::DeviceCatalog catalog;
  auto refreshed = catalog.refresh();
  if (!refreshed) {
    std::cerr << "catalog refresh failed: " << refreshed.error().message << "\n";
    return 1;
  }

  const auto& eps = catalog.endpoints();
  std::cout << "Found " << eps.size() << " modem(s):\n";
  for (const auto& ep : eps) {
    std::cout << "  " << ep.id << "  " << ep.product;
    if (!ep.matched_profile_id.empty()) std::cout << "  profile=" << ep.matched_profile_id;
    std::cout << "\n";
    if (ep.ports.empty()) {
      std::cout << "    (no ttyUSB/cdc-wdm — serial/QMI drivers not bound)\n";
    }
    for (const auto& p : ep.ports) {
      std::cout << "    " << QCom::Qmi::device::to_string(p.role) << "  " << p.path << "\n";
    }
  }
  return 0;
}

}  // namespace Observer
