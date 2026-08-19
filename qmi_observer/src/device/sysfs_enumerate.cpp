#include <qcom/qmi/device/catalog.hpp>

#include <cctype>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace QCom::Qmi::device {
namespace {

std::string read_trim(const std::filesystem::path& p) {
  std::ifstream in(p);
  if (!in) {
    return {};
  }
  std::string s;
  std::getline(in, s);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) {
    s.pop_back();
  }
  return s;
}

uint16_t parse_hex_u16(std::string_view s) {
  uint16_t v = 0;
  for (char c : s) {
    v <<= 4;
    if (c >= '0' && c <= '9') {
      v |= static_cast<uint16_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v |= static_cast<uint16_t>(10 + c - 'a');
    } else if (c >= 'A' && c <= 'F') {
      v |= static_cast<uint16_t>(10 + c - 'A');
    }
  }
  return v;
}

std::string driver_name(const std::filesystem::path& iface_dir) {
  std::error_code ec;
  auto link = std::filesystem::read_symlink(iface_dir / "driver", ec);
  if (ec) {
    return {};
  }
  return link.filename().string();
}

std::optional<uint8_t> read_iface_number(const std::filesystem::path& iface_dir) {
  const auto s = read_trim(iface_dir / "bInterfaceNumber");
  if (s.empty()) {
    return std::nullopt;
  }
  return static_cast<uint8_t>(std::stoul(s, nullptr, 16));
}

PortRole role_from_profile(const ModemProfile* profile, uint8_t iface_num, const std::string& driver) {
  if (profile) {
    for (const auto& [num, role] : profile->interface_roles) {
      if (num == iface_num) {
        return role;
      }
    }
  }
  if (driver == "qmi_wwan" || driver == "qmi_wwan_simcom") {
    return PortRole::Qmi;
  }
  if (driver == "cdc_mbim") {
    return PortRole::Mbim;
  }
  return PortRole::Unknown;
}

void collect_tty_under(const std::filesystem::path& iface_dir, const std::filesystem::path& dev_root,
                       uint8_t iface_num, PortRole role, const std::string& driver,
                       std::vector<PortDesc>& ports) {
  std::error_code ec;
  if (!std::filesystem::exists(iface_dir, ec)) {
    return;
  }
  for (const auto& ent : std::filesystem::directory_iterator(iface_dir, ec)) {
    if (ec) {
      break;
    }
    const auto name = ent.path().filename().string();
    if (name.rfind("ttyUSB", 0) == 0 || name.rfind("ttyACM", 0) == 0) {
      PortDesc p;
      p.path = (dev_root / name).string();
      p.role = role;
      p.usb_interface = iface_num;
      p.driver = driver;
      p.sysfs_iface = iface_dir.string();
      ports.push_back(std::move(p));
    }
  }
  // Sometimes tty lives in iface/tty/ttyUSBx
  const auto tty_dir = iface_dir / "tty";
  if (std::filesystem::exists(tty_dir, ec)) {
    for (const auto& ent : std::filesystem::directory_iterator(tty_dir, ec)) {
      if (ec) {
        break;
      }
      const auto name = ent.path().filename().string();
      if (name.rfind("tty", 0) == 0) {
        PortDesc p;
        p.path = (dev_root / name).string();
        p.role = role;
        p.usb_interface = iface_num;
        p.driver = driver;
        p.sysfs_iface = iface_dir.string();
        ports.push_back(std::move(p));
      }
    }
  }
}

void collect_cdc_wdm(const std::filesystem::path& iface_dir, const std::filesystem::path& sysfs_root,
                     const std::filesystem::path& dev_root, uint8_t iface_num,
                     const std::string& driver, std::vector<PortDesc>& ports) {
  const auto usbmisc = sysfs_root / "class" / "usbmisc";
  std::error_code ec;
  if (!std::filesystem::exists(usbmisc, ec)) {
    return;
  }
  const auto iface_canon = std::filesystem::weakly_canonical(iface_dir, ec);
  if (ec) {
    return;
  }
  for (const auto& ent : std::filesystem::directory_iterator(usbmisc, ec)) {
    if (ec) {
      break;
    }
    const auto name = ent.path().filename().string();
    if (name.rfind("cdc-wdm", 0) != 0) {
      continue;
    }
    auto resolved = std::filesystem::weakly_canonical(ent.path() / "device", ec);
    if (ec) {
      continue;
    }
    if (resolved != iface_canon) {
      continue;
    }
    PortDesc p;
    p.path = (dev_root / name).string();
    p.role = PortRole::Qmi;
    p.usb_interface = iface_num;
    p.driver = driver.empty() ? "qmi_wwan" : driver;
    p.sysfs_iface = iface_dir.string();
    ports.push_back(std::move(p));
  }
}

}  // namespace

Result<std::vector<ModemEndpoint>> enumerate_sysfs(const EnumerateOptions& opts,
                                                   const ProfileRegistry& profiles) {
  std::vector<ModemEndpoint> out;
  const auto devices_root = opts.sysfs_root / "bus" / "usb" / "devices";
  std::error_code ec;
  if (!std::filesystem::exists(devices_root, ec)) {
    return Error::from(Errc::Internal, "sysfs usb devices missing: " + devices_root.string());
  }

  for (const auto& ent : std::filesystem::directory_iterator(devices_root, ec)) {
    if (ec) {
      break;
    }
    if (!ent.is_directory()) {
      continue;
    }
    const auto name = ent.path().filename().string();
    // Skip hubs roots like usb1, and interfaces like 1-4:1.0
    if (name.find(':') != std::string::npos) {
      continue;
    }
    if (name.rfind("usb", 0) == 0 && name.find('-') == std::string::npos) {
      continue;
    }
    if (name.find('-') == std::string::npos) {
      continue;
    }

    const auto vid_s = read_trim(ent.path() / "idVendor");
    const auto pid_s = read_trim(ent.path() / "idProduct");
    if (vid_s.empty() || pid_s.empty()) {
      continue;
    }

    ModemEndpoint ep;
    ep.vid = parse_hex_u16(vid_s);
    ep.pid = parse_hex_u16(pid_s);
    ep.usb_path = name;
    ep.serial = read_trim(ent.path() / "serial");
    ep.manufacturer = read_trim(ent.path() / "manufacturer");
    ep.product = read_trim(ent.path() / "product");
    ep.id = make_endpoint_id(ep.vid, ep.pid, ep.serial, ep.usb_path, ep.serial_is_stable);

    const ModemProfile* profile =
        profiles.match(ep.vid, ep.pid, ep.manufacturer, ep.product);

    // Scan interfaces  name:1.N
    for (const auto& child : std::filesystem::directory_iterator(ent.path(), ec)) {
      if (ec) {
        break;
      }
      const auto cname = child.path().filename().string();
      if (cname.rfind(name + ":", 0) != 0) {
        continue;
      }
      const auto iface_num = read_iface_number(child.path());
      if (!iface_num) {
        continue;
      }
      const auto drv = driver_name(child.path());
      PortRole role = role_from_profile(profile, *iface_num, drv);

      if (drv == "qmi_wwan" || drv == "qmi_wwan_simcom" || role == PortRole::Qmi) {
        collect_cdc_wdm(child.path(), opts.sysfs_root, opts.dev_root, *iface_num, drv, ep.ports);
        // ensure role Qmi on collected
        for (auto& p : ep.ports) {
          if (p.sysfs_iface == child.path().string()) {
            p.role = PortRole::Qmi;
          }
        }
      }

      if (drv == "option" || drv == "usb_wwan" || drv == "cdc_acm" || drv == "qcserial") {
        if (role == PortRole::Unknown && profile && profile->preferred_at_interface &&
            *profile->preferred_at_interface == *iface_num) {
          role = PortRole::At;
        }
        collect_tty_under(child.path(), opts.dev_root, *iface_num, role, drv, ep.ports);
      }
    }

    if (profile) {
      ep.matched_profile_id = profile->id;
    }

    if (ep.ports.empty()) {
      // Known VID/PID with no tty/QMI yet (option.ko not loaded). Still report so
      // the GUI can say "modem present, drivers missing" instead of "no modem".
      if (!profile) {
        continue;
      }
    }

    const bool has_qmi = ep.qmi_path().has_value();
    if (opts.only_with_qmi && !has_qmi) {
      continue;
    }

    if (ep.matched_profile_id.empty() && has_qmi) {
      ep.matched_profile_id = ProfileRegistry::generic_qmi_profile().id;
    }

    // Dedup ports by path
    std::unordered_set<std::string> seen;
    std::vector<PortDesc> uniq;
    for (auto& p : ep.ports) {
      if (seen.insert(p.path).second) {
        uniq.push_back(std::move(p));
      }
    }
    ep.ports = std::move(uniq);
    out.push_back(std::move(ep));
  }

  return out;
}

}  // namespace QCom::Qmi::device
