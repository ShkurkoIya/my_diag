#include <qcom/qmi/Qmi.hpp>

#include <chrono>
#include <iostream>
#include <string>

namespace {

void print_usage(const char* argv0) {
  std::cerr << "usage: " << argv0 << " [--device /dev/cdc-wdm0] [--no-proxy] [--collect]\n"
            << "\n"
            << "qmi_observer hello / smoke:\n"
            << "  - prints versions\n"
            << "  - with --device: open, identity, health\n"
            << "  - with --collect: ScanController::collect_once()\n";
}

void print_cell(const QCom::Qmi::CellObservation& c) {
  std::cout << "  " << QCom::Qmi::to_string(c.rat) << (c.serving ? " SERVING" : "") << " rf="
            << (c.rf_channel ? std::to_string(*c.rf_channel) : "-")
            << " phy=" << (c.phy_id ? std::to_string(*c.phy_id) : "-")
            << " cid=" << (c.cell_id ? std::to_string(*c.cell_id) : "-");
  if (c.plmn) {
    std::cout << " plmn=" << c.plmn->mcc << "-" << c.plmn->mnc;
  }
  if (c.rsrp_dbm) {
    std::cout << " rsrp/rscp=" << *c.rsrp_dbm;
  }
  std::cout << '\n';
}

}  // namespace

int main(int argc, char** argv) {
  using namespace QCom::Qmi;

  std::string device;
  bool use_proxy = true;
  bool do_collect = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    }
    if (arg == "--no-proxy") {
      use_proxy = false;
      continue;
    }
    if (arg == "--collect") {
      do_collect = true;
      continue;
    }
    if (arg == "--device") {
      if (i + 1 >= argc) {
        std::cerr << "--device requires a path\n";
        return 2;
      }
      device = argv[++i];
      continue;
    }
    std::cerr << "unknown arg: " << arg << '\n';
    print_usage(argv[0]);
    return 2;
  }

  std::cout << "qmi_observer " << version() << '\n';
  std::cout << "libqmi-glib headers " << libqmi_version_string() << '\n';

  if (device.empty()) {
    std::cout << "no --device; link OK\n";
    return 0;
  }

  Settings cfg;
  cfg.device_path = device;
  cfg.use_proxy = use_proxy;
  cfg.settle_time = std::chrono::milliseconds(0);
  cfg.camp_wait = std::chrono::seconds(20);

  Session session(cfg);
  session.set_callbacks(Callbacks{
      .on_health_changed =
          [](const ModemHealth& h) {
            std::cout << "[health] " << to_string(h.phase) << " — " << h.summary << '\n';
          },
      .on_error =
          [](const Error& e) {
            std::cerr << "[error] " << to_string(e.code) << ": " << e.message << '\n';
          },
  });

  if (auto opened = session.open(); !opened) {
    std::cerr << "open failed: " << opened.error().message << '\n';
    return 1;
  }

  if (auto id = session.identity(); id) {
    std::cout << "DMS manufacturer: " << id.value().manufacturer << '\n';
    std::cout << "DMS model:        " << id.value().model << '\n';
    std::cout << "DMS revision:     " << id.value().revision << '\n';
  }

  if (auto h = session.refresh_health(); h) {
    std::cout << "can_snapshot=" << h.value().can_snapshot
              << " can_force_search=" << h.value().can_force_search
              << " needs_recover=" << h.value().needs_recover << '\n';
  }

  if (do_collect) {
    ScanPolicy pol;
    pol.force_search = false;  // smoke: don't thrash RF
    pol.ensure_preferred_modes = false;
    pol.settle_time = std::chrono::milliseconds(0);
    pol.camp_wait = std::chrono::seconds(5);
    pol.treat_no_network_as_empty = true;
    session.scan().policy() = pol;

    if (auto cells = session.scan().collect_once(); cells) {
      std::cout << "collect_once: " << cells.value().cells.size() << " cells\n";
      for (const auto& c : cells.value().cells) {
        print_cell(c);
      }
      auto envs = to_rrc_envelopes(CellSnapshot{.cells = cells.value().cells});
      std::cout << "bridge envelopes: " << envs.size() << '\n';
    } else {
      std::cerr << "collect_once failed: " << cells.error().message << '\n';
    }
  } else {
    if (auto snap = session.nas().snapshot_cells(); snap) {
      std::cout << "snapshot: " << snap.value().cells.size() << " cells\n";
      for (const auto& c : snap.value().cells) {
        print_cell(c);
      }
    } else {
      std::cerr << "snapshot: " << to_string(snap.error().code) << " — " << snap.error().message
                << '\n';
    }
  }

  session.close();
  std::cout << "closed\n";
  return 0;
}
