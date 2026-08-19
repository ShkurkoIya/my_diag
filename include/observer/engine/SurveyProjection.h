/// @file SurveyProjection.h
/// @brief Pure projection: tracker snapshot (physical cells) → survey domain.
///
/// This is the honest-counts / dedup / operator-grouping logic that used to be
/// re-derived inline in the app (write_live_json) and the JSON tool. Centralising
/// it here means the facade, JSON export and any UI report the *same* numbers.
///
/// Pure functions over `std::vector<CellIdentity>`; no locks, no I/O.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include <observer/model/CellIdentity.h>
#include <observer/model/Utils.h>
#include <observer/engine/SurveyDomain.h>
#include <observer/lte/LteHopPlanner.h>  // QCom::Lte::cell_is_full_lte (single source of "FULL")

namespace QCom::Engine {

/// Plausible LTE RF detection: real EARFCN + PCI in range, not modem padding.
[[nodiscard]] inline bool is_lte_rf(const CellIdentity& c) noexcept {
  if (c.rat != RatType::LTE) return false;
  const uint32_t earfcn = c.radio.freq();
  const uint16_t pci = c.radio.pci_bsic();
  if (!Utils::valid_lte_earfcn(earfcn) || pci == 0 || pci > 503) return false;
  // Reject sentinel PLMN padding leaking into RF rows.
  if (c.passport.mcc == 0xFFFF || c.passport.mnc == 0xFFFF || c.passport.mcc > 999) return false;
  return true;
}

/// FULL identified LTE carrier — reuses the same predicate the hop planner uses,
/// so "what counts as a tower" never diverges between survey walk and reporting.
[[nodiscard]] inline bool is_lte_full(const CellIdentity& c) noexcept {
  return Lte::cell_is_full_lte(c);
}

[[nodiscard]] inline Tower tower_from_cell(const CellIdentity& c) {
  Tower t;
  t.mcc = c.passport.mcc;
  t.mnc = c.passport.mnc;
  t.mnc_digits = c.passport.mnc_digits;
  t.tac = c.passport.tac;
  t.eci = c.passport.cell_id;
  t.earfcn = c.radio.freq();
  t.pci = c.radio.pci_bsic();
  t.serving = c.is_serving;
  t.ever_serving = c.ever_serving;
  if (const auto* r = c.radio_as_if<LteRadioParams>()) t.band = r->freq_band_ind;
  if (const auto* s = c.signal_as_if<LteSignalParams>()) {
    if (Utils::valid_lte_rsrp(s->rsrp)) {
      t.rsrp_dbm = s->rsrp;
      t.has_rsrp = true;
    }
  }
  return t;
}

/// Project a tracker snapshot into the LTE survey domain (towers + operators +
/// honest stats). One pass; deterministic ordering (by EARFCN, then PCI).
[[nodiscard]] inline SurveyResult project_lte(const std::vector<CellIdentity>& cells) {
  SurveyResult out;

  // Operator aggregation: PLMN → (tower count, distinct eNB set).
  struct OpAgg {
    uint8_t mnc_digits{0};
    std::size_t towers{0};
    std::set<uint32_t> sites;
  };
  std::map<std::pair<uint16_t, uint16_t>, OpAgg> ops;
  std::set<uint32_t> all_sites;

  for (const auto& c : cells) {
    if (c.rat != RatType::LTE) continue;
    if (is_lte_rf(c)) ++out.stats.lte_rf_unique;
    if (c.is_serving) ++out.stats.lte_serving;
    if (!is_lte_full(c)) continue;

    Tower t = tower_from_cell(c);
    all_sites.insert(t.enb_id());
    auto& agg = ops[{t.mcc, t.mnc}];
    agg.mnc_digits = t.mnc_digits;
    ++agg.towers;
    agg.sites.insert(t.enb_id());
    out.towers.push_back(t);
  }

  std::sort(out.towers.begin(), out.towers.end(), [](const Tower& a, const Tower& b) {
    if (a.earfcn != b.earfcn) return a.earfcn < b.earfcn;
    return a.pci < b.pci;
  });

  out.stats.lte_full = out.towers.size();
  out.stats.lte_sites = all_sites.size();

  out.operators.reserve(ops.size());
  for (const auto& [plmn, agg] : ops) {
    out.operators.push_back(Operator{.mcc = plmn.first,
                                     .mnc = plmn.second,
                                     .mnc_digits = agg.mnc_digits,
                                     .towers = agg.towers,
                                     .sites = agg.sites.size()});
  }
  std::sort(out.operators.begin(), out.operators.end(), [](const Operator& a, const Operator& b) {
    if (a.towers != b.towers) return a.towers > b.towers;  // richest operator first
    if (a.mcc != b.mcc) return a.mcc < b.mcc;
    return a.mnc < b.mnc;
  });

  return out;
}

}  // namespace QCom::Engine
