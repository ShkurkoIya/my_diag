#include <qcom/qmi/device/profile.hpp>

#include <algorithm>
#include <cctype>

namespace QCom::Qmi::device {
namespace {

bool contains_ci(std::string_view hay, std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  if (hay.size() < needle.size()) {
    return false;
  }
  auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                        [](char a, char b) {
                          return std::tolower(static_cast<unsigned char>(a)) ==
                                 std::tolower(static_cast<unsigned char>(b));
                        });
  return it != hay.end();
}

ModemProfile make_simcom_83xx() {
  ModemProfile p;
  p.id = "simcom_83xx_qmi";
  p.display_name = "SimCom SIM82xx/83xx (QMI)";
  p.match.vid = 0x1e0e;
  p.match.pid = 0x9001;
  p.match.product_substr = "SDXPRAIRIE";
  p.interface_roles = {
      {0, PortRole::Diag}, {1, PortRole::Nmea}, {2, PortRole::At},
      {3, PortRole::Modem}, {4, PortRole::Audio}, {5, PortRole::Qmi},
  };
  p.preferred_at_interface = 2;
  p.quirks.allow_dms_offline = false;
  p.quirks.prefer_qmi_proxy = false;
  p.quirks.at_probe_safe = true;
  p.preferred_modes = {Rat::Lte, Rat::Wcdma};
  p.notes =
      "Типичная раскладка SimCom: ttyUSB0 DIAG, ttyUSB1 NMEA (GNSS), ttyUSB2 AT, "
      "ttyUSB3 modem/PPP, ttyUSB4 audio, cdc-wdm на interface 5 (qmi_wwan). "
      "DMS offline на SDX55 опасен.";
  return p;
}

ModemProfile make_generic() {
  ModemProfile p;
  p.id = "generic_qmi";
  p.display_name = "Generic Qualcomm QMI WWAN";
  p.interface_roles = {};
  p.preferred_at_interface = std::nullopt;
  p.quirks.allow_dms_offline = false;
  p.quirks.prefer_qmi_proxy = true;
  p.notes = "Fallback: любое устройство с cdc-wdm. AT iface неизвестен — не пробовать вслепую.";
  return p;
}

}  // namespace

ProfileRegistry::ProfileRegistry() {
  profiles_.push_back(make_simcom_83xx());
  // Можно расширять: quectel_*, fibocom_*, ...
}

const ModemProfile& ProfileRegistry::generic_qmi_profile() noexcept {
  static const ModemProfile g = make_generic();
  return g;
}

const ModemProfile* ProfileRegistry::find_by_id(std::string_view id) const noexcept {
  for (const auto& p : profiles_) {
    if (p.id == id) {
      return &p;
    }
  }
  if (id == generic_qmi_profile().id) {
    return &generic_qmi_profile();
  }
  return nullptr;
}

const ModemProfile* ProfileRegistry::match(uint16_t vid, uint16_t pid,
                                           std::string_view manufacturer,
                                           std::string_view product) const noexcept {
  const ModemProfile* best = nullptr;
  int best_score = -1;

  for (const auto& p : profiles_) {
    int score = 0;
    if (p.match.vid && *p.match.vid != vid) {
      continue;
    }
    if (p.match.pid && *p.match.pid != pid) {
      continue;
    }
    if (p.match.vid) {
      score += 10;
    }
    if (p.match.pid) {
      score += 20;
    }
    if (!p.match.product_substr.empty()) {
      if (contains_ci(product, p.match.product_substr)) {
        score += 5;
      } else if (!(p.match.vid && p.match.pid)) {
        continue;
      }
    }
    if (!p.match.manufacturer_substr.empty()) {
      if (!contains_ci(manufacturer, p.match.manufacturer_substr)) {
        continue;
      }
      score += 3;
    }
    if (score > best_score) {
      best_score = score;
      best = &p;
    }
  }
  return best;
}

}  // namespace QCom::Qmi::device
