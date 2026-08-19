#include <qcom/qmi/device/catalog.hpp>
#include <qcom/qmi/device/probe.hpp>
#include <qcom/qmi/version.hpp>

#include <iostream>
#include <string>

namespace {

void print_usage(const char* argv0) {
  std::cerr
      << "usage: " << argv0
      << " [--sysfs /sys] [--dev /dev] [--dossier PATH] [--probe] [--radio]\n"
      << "\n"
      << "Быстрый список USB-модемов (sysfs). Опционально — probe QMI/AT.\n"
      << "Документация: qmi_observer/docs/ru/device_manager.md\n";
}

void print_endpoint(const QCom::Qmi::device::ModemReport& r) {
  const auto& e = r.endpoint;
  std::cout << "id=" << e.id << "  " << std::hex << e.vid << ':' << e.pid << std::dec
            << "  usb=" << e.usb_path << '\n';
  std::cout << "  product=\"" << e.product << "\" manuf=\"" << e.manufacturer << "\"\n";
  std::cout << "  profile=" << e.matched_profile_id
            << (e.serial_is_stable ? "" : "  (unstable id)") << '\n';
  if (auto q = e.qmi_path()) {
    std::cout << "  qmi=" << *q << '\n';
  }
  if (auto a = e.preferred_at_path()) {
    std::cout << "  at=" << *a << '\n';
  }
  for (const auto& p : e.ports) {
    std::cout << "    port " << QCom::Qmi::device::to_string(p.role) << " " << p.path;
    if (p.usb_interface) {
      std::cout << " iface=" << unsigned(*p.usb_interface);
    }
    std::cout << " drv=" << p.driver << '\n';
  }
  if (r.dossier) {
    const auto& d = *r.dossier;
    std::cout << "  dossier: qmi_ok=" << d.qmi_open_ok << " at_ok=" << d.at_ok
              << " probe=" << QCom::Qmi::device::to_string(d.deepest_probe) << '\n';
    if (d.dms_model) {
      std::cout << "    dms model=" << *d.dms_model << '\n';
    }
    if (d.last_phase) {
      std::cout << "    phase=" << QCom::Qmi::to_string(*d.last_phase) << '\n';
    }
    if (!d.last_error.empty()) {
      std::cout << "    err=" << d.last_error << '\n';
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  using namespace QCom::Qmi::device;

  EnumerateOptions en;
  std::string dossier;
  bool do_probe = false;
  bool radio = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    }
    if (arg == "--sysfs" && i + 1 < argc) {
      en.sysfs_root = argv[++i];
      continue;
    }
    if (arg == "--dev" && i + 1 < argc) {
      en.dev_root = argv[++i];
      continue;
    }
    if (arg == "--dossier" && i + 1 < argc) {
      dossier = argv[++i];
      continue;
    }
    if (arg == "--probe") {
      do_probe = true;
      continue;
    }
    if (arg == "--radio") {
      radio = true;
      continue;
    }
    std::cerr << "unknown arg: " << arg << '\n';
    print_usage(argv[0]);
    return 2;
  }

  std::cout << "qmi_observer " << QCom::Qmi::version() << " — device catalog\n";

  DeviceCatalog catalog;
  if (!dossier.empty()) {
    catalog.set_dossier_path(dossier);
    (void)catalog.load_dossiers();
  }

  if (auto r = catalog.refresh(en); !r) {
    std::cerr << "refresh failed: " << r.error().message << '\n';
    return 1;
  }

  std::cout << "found " << catalog.endpoints().size() << " modem endpoint(s)\n";

  if (do_probe) {
    ProbeOptions po;
    po.level = radio ? ProbeLevel::Radio : ProbeLevel::Identity;
    po.use_proxy = false;
    for (const auto& ep : catalog.endpoints()) {
      std::cout << "probing " << ep.id << "...\n";
      if (auto d = catalog.probe(ep.id, po); !d) {
        std::cerr << "  probe error: " << d.error().message << '\n';
      }
    }
  }

  for (const auto& rep : catalog.list_reports()) {
    print_endpoint(rep);
    std::cout << '\n';
  }
  return 0;
}
