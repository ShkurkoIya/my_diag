#include <qmi_observer/device/catalog.hpp>
#include <qmi_observer/device/dossier_store.hpp>
#include <qmi_observer/device/endpoint.hpp>
#include <qmi_observer/device/profile.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace qmi_observer::device;
namespace fs = std::filesystem;

TEST_CASE("make_endpoint_id uses serial when real", "[device][id]") {
  bool stable = false;
  const auto id = make_endpoint_id(0x1e0e, 0x9001, "REALSERIAL01", "1-4", stable);
  REQUIRE(stable);
  REQUIRE(id == "1e0e:9001:REALSERIAL01");
}

TEST_CASE("make_endpoint_id falls back on fake serial", "[device][id]") {
  bool stable = true;
  const auto id = make_endpoint_id(0x1e0e, 0x9001, "0123456789ABCDEF", "1-4", stable);
  REQUIRE_FALSE(stable);
  REQUIRE(id == "1e0e:9001@1-4");
}

TEST_CASE("profile match simcom 83xx", "[device][profile]") {
  ProfileRegistry reg;
  const auto* p = reg.match(0x1e0e, 0x9001, "QCOM", "SDXPRAIRIE-MTP _SN:20D0AD04");
  REQUIRE(p != nullptr);
  REQUIRE(p->id == "simcom_83xx_qmi");
  REQUIRE(p->preferred_at_interface.has_value());
  REQUIRE(*p->preferred_at_interface == 2);
  REQUIRE_FALSE(p->quirks.allow_dms_offline);
}

TEST_CASE("dossier serialize parse roundtrip", "[device][dossier]") {
  std::unordered_map<std::string, ModemDossier> map;
  ModemDossier d;
  d.endpoint_id = "1e0e:9001@1-4";
  d.matched_profile_id = "simcom_83xx_qmi";
  d.qmi_path = "/dev/cdc-wdm0";
  d.at_paths = {"/dev/ttyUSB2"};
  d.qmi_open_ok = true;
  d.at_ok = true;
  d.dms_model = "0";
  d.dms_manufacturer = "QUALCOMM INCORPORATED";
  d.deepest_probe = ProbeLevel::Identity;
  d.probed_at_unix = 1700000000;
  d.last_phase = qmi_observer::ModemPhase::Camped;
  map[d.endpoint_id] = d;

  const auto json = serialize_dossiers(map);
  auto parsed = parse_dossiers(json);
  REQUIRE(parsed);
  REQUIRE(parsed.value().size() == 1);
  const auto& got = parsed.value().at(d.endpoint_id);
  REQUIRE(got.qmi_open_ok);
  REQUIRE(got.at_ok);
  REQUIRE(got.qmi_path == "/dev/cdc-wdm0");
  REQUIRE(got.dms_manufacturer == "QUALCOMM INCORPORATED");
  REQUIRE(got.at_paths.size() == 1);
  REQUIRE(got.at_paths[0] == "/dev/ttyUSB2");
  REQUIRE(got.last_phase.has_value());
  REQUIRE(*got.last_phase == qmi_observer::ModemPhase::Camped);
}

TEST_CASE("enumerate synthetic sysfs fixture", "[device][sysfs]") {
  const auto root = fs::temp_directory_path() / "qmi_obs_sysfs_test";
  fs::remove_all(root);
  const auto sys = root / "sys";
  const auto dev = root / "dev";
  const auto usb_dev = sys / "bus" / "usb" / "devices" / "1-9";
  const auto iface0 = usb_dev / "1-9:1.0";
  const auto iface2 = usb_dev / "1-9:1.2";
  const auto iface5 = usb_dev / "1-9:1.5";
  fs::create_directories(iface0 / "ttyUSB0");
  fs::create_directories(iface2 / "ttyUSB2");
  fs::create_directories(iface5);
  fs::create_directories(sys / "class" / "usbmisc" / "cdc-wdm0");
  fs::create_directories(dev);

  auto write = [](const fs::path& p, std::string_view v) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << v;
  };
  write(usb_dev / "idVendor", "1e0e");
  write(usb_dev / "idProduct", "9001");
  write(usb_dev / "serial", "0123456789ABCDEF");
  write(usb_dev / "manufacturer", "QCOM");
  write(usb_dev / "product", "SDXPRAIRIE-MTP");
  write(iface0 / "bInterfaceNumber", "00");
  write(iface2 / "bInterfaceNumber", "02");
  write(iface5 / "bInterfaceNumber", "05");

  // driver symlinks
  fs::create_directories(sys / "bus" / "usb" / "drivers" / "option");
  fs::create_directories(sys / "bus" / "usb" / "drivers" / "qmi_wwan");
  fs::create_symlink(sys / "bus" / "usb" / "drivers" / "option", iface0 / "driver");
  fs::create_symlink(sys / "bus" / "usb" / "drivers" / "option", iface2 / "driver");
  fs::create_symlink(sys / "bus" / "usb" / "drivers" / "qmi_wwan", iface5 / "driver");
  fs::create_symlink(iface5, sys / "class" / "usbmisc" / "cdc-wdm0" / "device");

  // fake /dev nodes (empty files)
  std::ofstream(dev / "ttyUSB0") << "";
  std::ofstream(dev / "ttyUSB2") << "";
  std::ofstream(dev / "cdc-wdm0") << "";

  ProfileRegistry reg;
  EnumerateOptions opts;
  opts.sysfs_root = sys;
  opts.dev_root = dev;
  auto got = enumerate_sysfs(opts, reg);
  REQUIRE(got);
  REQUIRE(got.value().size() == 1);
  const auto& ep = got.value()[0];
  REQUIRE(ep.matched_profile_id == "simcom_83xx_qmi");
  REQUIRE(ep.qmi_path().has_value());
  REQUIRE(*ep.qmi_path() == (dev / "cdc-wdm0").string());
  REQUIRE(ep.preferred_at_path().has_value());
  REQUIRE(*ep.preferred_at_path() == (dev / "ttyUSB2").string());
  REQUIRE(ep.preferred_diag_path().has_value());
  REQUIRE(*ep.preferred_diag_path() == (dev / "ttyUSB0").string());

  fs::remove_all(root);
}

TEST_CASE("to_qmi_settings from endpoint+profile", "[device][settings]") {
  ProfileRegistry reg;
  const auto* p = reg.find_by_id("simcom_83xx_qmi");
  REQUIRE(p);
  ModemEndpoint ep;
  ep.ports.push_back(PortDesc{.path = "/dev/cdc-wdm0", .role = PortRole::Qmi});
  auto s = ep.to_qmi_settings(p);
  REQUIRE(s.device_path == "/dev/cdc-wdm0");
  REQUIRE_FALSE(s.allow_dms_offline);
  REQUIRE_FALSE(s.use_proxy);
}
