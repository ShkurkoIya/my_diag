#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
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
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include <observer/model/BandInfo.h>
#include <observer/model/CellIdentity.h>
#include <observer/lte/LteHopPlanner.h>
#include "observer/AtParse.h"

#include <initializer_list>

namespace Observer {

using QCom::Lte::HopTarget;

struct WcdmaHopTarget {
  uint32_t uarfcn{0};
  uint16_t psc{0};  ///< 0 = UARFCN-only seed (SIB6)
  float rscp{-160.0f};
  uint16_t mcc{0};
  uint16_t mnc{0};
  bool has_full{false};
  bool camped{false};
  int plmn_camp_n{0};
};

[[nodiscard]] inline bool cell_is_full_wcdma(const QCom::CellIdentity& c) {
  return c.rat == QCom::RatType::WCDMA && c.passport.cell_id != 0 && c.passport.tac != 0 &&
         c.passport.mcc != 0 && c.radio.pci_bsic() <= 511 && c.radio.freq() > 0 &&
         c.radio.freq() <= 16383;
}

/// Walk WCDMA UARFCN|PSC from DIAG 0x4005 / serving RF + SIB6 UARFCN seeds.
/// Priority: new PLMN → incomplete → uncamped FULL → stronger RSCP.
[[nodiscard]] inline std::vector<WcdmaHopTarget> pick_wcdma_walk_targets(
    const std::vector<QCom::CellIdentity>& cells, std::size_t max_n) {
  std::map<std::pair<uint16_t, uint16_t>, int> camped_per_plmn;
  for (const auto& c : cells) {
    if (c.rat != QCom::RatType::WCDMA || !c.ever_serving || c.passport.mcc == 0) continue;
    ++camped_per_plmn[{c.passport.mcc, c.passport.mnc}];
  }

  std::map<std::pair<uint32_t, uint16_t>, WcdmaHopTarget> best;
  auto upsert = [&](WcdmaHopTarget t) {
    if (t.uarfcn == 0 || t.uarfcn > 16383) return;
    if (t.psc > 511) return;
    if (t.camped && t.psc != 0) return;
    t.plmn_camp_n = (t.mcc != 0) ? camped_per_plmn[{t.mcc, t.mnc}] : 0;
    const auto key = std::pair{t.uarfcn, t.psc};
    auto it = best.find(key);
    if (it == best.end()) {
      best.emplace(key, t);
      return;
    }
    if (t.has_full && !it->second.has_full) {
      t.rscp = std::max(t.rscp, it->second.rscp);
      it->second = t;
    } else if (t.mcc != 0 && it->second.mcc == 0) {
      t.rscp = std::max(t.rscp, it->second.rscp);
      it->second = t;
    } else if (t.rscp > it->second.rscp) {
      t.has_full = t.has_full || it->second.has_full;
      if (t.mcc == 0) {
        t.mcc = it->second.mcc;
        t.mnc = it->second.mnc;
      }
      it->second = t;
    } else {
      if (it->second.mcc == 0 && t.mcc != 0) {
        it->second.mcc = t.mcc;
        it->second.mnc = t.mnc;
      }
      if (t.has_full) it->second.has_full = true;
      it->second.plmn_camp_n = t.plmn_camp_n;
    }
  };

  for (const auto& c : cells) {
    // LTE SIB6 → UARFCN seeds (no PSC).
    if (c.rat == QCom::RatType::LTE) {
      for (const auto& u : c.radio.utra_neighbors) {
        if (u.uarfcn == 0 || u.uarfcn > 16383) continue;
        upsert(WcdmaHopTarget{.uarfcn = u.uarfcn,
                              .psc = 0,
                              .rscp = -158.0f,
                              .mcc = c.passport.mcc,
                              .mnc = c.passport.mnc});
      }
      continue;
    }
    if (c.rat != QCom::RatType::WCDMA) continue;
    const uint32_t uarfcn = c.radio.freq();
    const uint16_t psc = c.radio.pci_bsic();
    if (uarfcn == 0 || uarfcn > 16383) continue;
    float rscp = -160.0f;
    if (auto* s = c.signal.get_if<QCom::WcdmaSignalParams>()) {
      if (s->rscp > -120.0f && s->rscp < 0.0f) rscp = s->rscp;
    }
    if (psc <= 511) {
      upsert(WcdmaHopTarget{.uarfcn = uarfcn,
                            .psc = psc,
                            .rscp = rscp,
                            .mcc = c.passport.mcc,
                            .mnc = c.passport.mnc,
                            .has_full = cell_is_full_wcdma(c),
                            .camped = c.ever_serving});
    }
    for (const auto& n : c.radio.wcdma_neighbors) {
      if (n.uarfcn == 0 || n.uarfcn > 16383 || n.psc > 511) continue;
      float nr = (n.rscp != 0) ? static_cast<float>(n.rscp) : -155.0f;
      upsert(WcdmaHopTarget{.uarfcn = n.uarfcn,
                            .psc = n.psc,
                            .rscp = nr,
                            .mcc = c.passport.mcc,
                            .mnc = c.passport.mnc});
    }
  }

  std::vector<WcdmaHopTarget> ranked;
  ranked.reserve(best.size());
  for (const auto& [_, t] : best) ranked.push_back(t);
  std::sort(ranked.begin(), ranked.end(), [](const WcdmaHopTarget& a, const WcdmaHopTarget& b) {
    // Prefer concrete PSC over UARFCN-only seeds.
    const bool a_seed = a.psc == 0;
    const bool b_seed = b.psc == 0;
    if (a_seed != b_seed) return !a_seed && b_seed;
    if (a.plmn_camp_n != b.plmn_camp_n) return a.plmn_camp_n < b.plmn_camp_n;
    if (a.has_full != b.has_full) return !a.has_full && b.has_full;
    if (a.rscp != b.rscp) return a.rscp > b.rscp;
    if (a.uarfcn != b.uarfcn) return a.uarfcn < b.uarfcn;
    return a.psc < b.psc;
  });
  if (ranked.size() > max_n) ranked.resize(max_n);
  return ranked;
}

[[nodiscard]] inline std::string bands_csv_from_uarfcns(const std::vector<WcdmaHopTarget>& targets) {
  std::set<uint16_t> bands;
  for (const auto& t : targets) {
    auto bi = QCom::BandInfo::umts_from_uarfcn(t.uarfcn);
    if (bi.band) bands.insert(bi.band);
  }
  for (uint16_t b : std::initializer_list<uint16_t>{1, 8}) bands.insert(b);
  std::ostringstream os;
  bool first = true;
  for (uint16_t b : bands) {
    if (!first) os << ':';
    first = false;
    os << b;
  }
  return os.str();
}

/// Single-band clip for sticky WCDMA grind (narrower than frontier-wide list).
[[nodiscard]] inline std::string band_csv_for_uarfcn(uint32_t uarfcn) {
  auto bi = QCom::BandInfo::umts_from_uarfcn(uarfcn);
  if (bi.band) return std::to_string(bi.band);
  return "1:8";
}

[[nodiscard]] inline std::optional<std::string> parse_csyssel_named_band_list(std::string_view resp,
                                                                      std::string_view key) {
  auto pos = resp.find(key);
  if (pos == std::string_view::npos) return std::nullopt;
  auto comma = resp.find(',', pos);
  if (comma == std::string_view::npos) return std::nullopt;
  std::string rest(resp.substr(comma + 1));
  if (auto nl = rest.find_first_of("\r\n"); nl != std::string::npos) rest.resize(nl);
  while (!rest.empty() && (rest.front() == ' ' || rest.front() == '"')) rest.erase(rest.begin());
  while (!rest.empty() && (rest.back() == ' ' || rest.back() == '"' || rest.back() == '\r'))
    rest.pop_back();
  if (rest.empty() || !std::isdigit(static_cast<unsigned char>(rest.front()))) return std::nullopt;
  return rest;
}

[[nodiscard]] inline std::string bands_csv_from_earfcns(const std::vector<HopTarget>& targets) {
  std::set<uint16_t> bands;
  for (const auto& t : targets) {
    auto bi = QCom::BandInfo::lte_from_earfcn(t.earfcn);
    if (bi.band) bands.insert(bi.band);
  }
  // Keep common RU/EU bands so we don't strand the modem on one band.
  for (uint16_t b : std::initializer_list<uint16_t>{1, 3, 7, 8, 20, 38, 40}) bands.insert(b);
  std::ostringstream os;
  bool first = true;
  for (uint16_t b : bands) {
    if (!first) os << ':';
    first = false;
    os << b;
  }
  return os.str();
}

[[nodiscard]] inline std::optional<std::string> parse_csyssel_lte_band_list(std::string_view resp) {
  auto pos = resp.find("lte_band");
  if (pos == std::string_view::npos) return std::nullopt;
  auto q1 = resp.find('"', pos);
  if (q1 == std::string_view::npos) return std::nullopt;
  // After "lte_band" the list may be: ,"1:2:3" or "lte_band",1:2:3
  auto comma = resp.find(',', pos);
  if (comma == std::string_view::npos) return std::nullopt;
  std::string rest(resp.substr(comma + 1));
  if (auto nl = rest.find_first_of("\r\n"); nl != std::string::npos) rest.resize(nl);
  // Trim quotes/spaces
  while (!rest.empty() && (rest.front() == ' ' || rest.front() == '"')) rest.erase(rest.begin());
  while (!rest.empty() && (rest.back() == ' ' || rest.back() == '"' || rest.back() == '\r'))
    rest.pop_back();
  if (rest.empty() || !std::isdigit(static_cast<unsigned char>(rest.front()))) return std::nullopt;
  return rest;
}

}  // namespace Observer
