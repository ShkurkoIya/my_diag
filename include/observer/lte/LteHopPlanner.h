/// @file LteHopPlanner.h
/// @brief Pure LTE hop target-selection policy (no I/O, unit-testable).
///
/// This is the logic that decides *which* EARFCN|PCI the survey walk should
/// camp on next — the single biggest lever on how full the tower map gets.
/// It was previously buried in an anonymous namespace inside the scanner app
/// and therefore untestable; extracted here so the ranking rules are covered
/// by unit tests (see tests/test_lte_hop_planner.cpp).
///
/// All functions are pure: `snapshot in → ranked targets out`. No locks, no AT,
/// no DIAG. The orchestrator (live_scanner) owns the actual lock/camp I/O.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

#include <observer/model/BandInfo.h>
#include <observer/model/CellIdentity.h>
#include <observer/model/Utils.h>

namespace QCom::Lte {

/// One candidate cell for the survey walk to lock/camp on.
struct HopTarget {
  uint32_t earfcn{0};
  uint16_t pci{0};  ///< 0 = no PCI yet (band search / CFUN fallback)
  float rsrp{-160.0f};
  uint16_t mcc{0};
  uint16_t mnc{0};
  bool has_full{false};  ///< passport FULL (CID+TAC+PLMN)
  bool camped{false};    ///< ever_serving / real camp on this RF key
  int plmn_camp_n{0};    ///< how many camped LTE cells share this PLMN (0 = new operator)
};

/// Hop only commercial EARFCNs we can map to a band (drops DIAG 0x8000 ghosts).
[[nodiscard]] inline bool hop_earfcn_ok(uint32_t earfcn) noexcept {
  return Utils::valid_lte_earfcn(earfcn) && BandInfo::is_known_lte_earfcn(earfcn);
}

[[nodiscard]] inline bool hop_earfcn_is_fdd(uint32_t earfcn) noexcept {
  return BandInfo::lte_from_earfcn(earfcn).duplex == BandInfo::Duplex::FDD;
}

[[nodiscard]] inline bool hop_earfcn_is_tdd(uint32_t earfcn) noexcept {
  return BandInfo::lte_from_earfcn(earfcn).duplex == BandInfo::Duplex::TDD;
}

/// FULL identity on a physical LTE key = PLMN + TAC + ECI + EARFCN|PCI.
[[nodiscard]] inline bool cell_is_full_lte(const CellIdentity& c) noexcept {
  return c.rat == RatType::LTE && Utils::valid_lte_eci(c.passport.cell_id) &&
         Utils::valid_lte_tac(c.passport.tac) && c.passport.mcc != 0 && c.radio.pci_bsic() != 0 &&
         Utils::valid_lte_earfcn(c.radio.freq());
}

/// Rank: FDD before TDD → (full-walk) new carrier then new PLMN → measured → fewer FULLs on
/// EARFCN → incomplete → RSRP.
/// Full-walk treats extra SIB1 PLMNs on an already-FULL EARFCN as the same grid: those
/// targets lose to a carrier with 0 FULL (other operators) via `a_n == 0`.
[[nodiscard]] inline bool hop_target_better(const HopTarget& a, const HopTarget& b,
                                            const std::map<uint32_t, int>& full_on_earfcn,
                                            bool full_walk) {
  const bool a_fdd = hop_earfcn_is_fdd(a.earfcn);
  const bool b_fdd = hop_earfcn_is_fdd(b.earfcn);
  if (a_fdd != b_fdd) return a_fdd && !b_fdd;
  const int a_n = full_on_earfcn.contains(a.earfcn) ? full_on_earfcn.at(a.earfcn) : 0;
  const int b_n = full_on_earfcn.contains(b.earfcn) ? full_on_earfcn.at(b.earfcn) : 0;
  if (full_walk) {
    const bool a_new_car = a_n == 0;
    const bool b_new_car = b_n == 0;
    if (a_new_car != b_new_car) return a_new_car;
    if (a.plmn_camp_n != b.plmn_camp_n) return a.plmn_camp_n < b.plmn_camp_n;
  }
  const bool a_meas = a.rsrp > -154.0f;
  const bool b_meas = b.rsrp > -154.0f;
  if (a_meas != b_meas) return a_meas && !b_meas;
  if (a_n != b_n) return a_n < b_n;
  if (full_walk && a.has_full != b.has_full) return !a.has_full && b.has_full;
  if (a.rsrp != b.rsrp) return a.rsrp > b.rsrp;
  if (a.earfcn != b.earfcn) return a.earfcn < b.earfcn;
  return a.pci < b.pci;
}

[[nodiscard]] inline std::map<uint32_t, int> full_count_by_earfcn(
    const std::vector<CellIdentity>& cells) {
  std::map<uint32_t, int> n;
  for (const auto& c : cells) {
    if (cell_is_full_lte(c)) ++n[c.radio.freq()];
  }
  return n;
}

/// How many times we already camped each PLMN (ever_serving).
[[nodiscard]] inline std::map<std::pair<uint16_t, uint16_t>, int> camped_count_by_plmn(
    const std::vector<CellIdentity>& cells) {
  std::map<std::pair<uint16_t, uint16_t>, int> n;
  for (const auto& c : cells) {
    if (c.rat != RatType::LTE || !c.ever_serving || c.passport.mcc == 0) continue;
    ++n[{c.passport.mcc, c.passport.mnc}];
  }
  return n;
}

/// Max camp-count of any PLMN already serving on this EARFCN.
/// Extra SIB1 PLMNs (250-02 vs 250-21 on the same 1596) inherit that count so
/// full-walk does not treat them as a new operator.
[[nodiscard]] inline std::map<uint32_t, int> camped_plmn_n_by_earfcn(
    const std::vector<CellIdentity>& cells,
    const std::map<std::pair<uint16_t, uint16_t>, int>& camped_per_plmn) {
  std::map<uint32_t, int> n;
  for (const auto& c : cells) {
    if (c.rat != RatType::LTE || !c.ever_serving || c.passport.mcc == 0) continue;
    const int pn = camped_per_plmn.contains({c.passport.mcc, c.passport.mnc})
                       ? camped_per_plmn.at({c.passport.mcc, c.passport.mnc})
                       : 0;
    auto& slot = n[c.radio.freq()];
    if (pn > slot) slot = pn;
  }
  return n;
}

[[nodiscard]] inline int full_walk_plmn_camp_n(
    uint32_t earfcn, uint16_t mcc, uint16_t mnc,
    const std::map<std::pair<uint16_t, uint16_t>, int>& camped_per_plmn,
    const std::map<uint32_t, int>& earfcn_plmn_camps) {
  int n = (mcc != 0 && camped_per_plmn.contains({mcc, mnc})) ? camped_per_plmn.at({mcc, mnc}) : 0;
  if (auto it = earfcn_plmn_camps.find(earfcn); it != earfcn_plmn_camps.end())
    n = std::max(n, it->second);
  return n;
}

/// Classic hop: only incomplete RF rows (skip anything already FULL).
/// Ranking: FDD before TDD, measured-RSRP before ghost seeds, new carrier, then RSRP.
[[nodiscard]] inline std::vector<HopTarget> pick_hop_targets(const std::vector<CellIdentity>& cells,
                                                             std::size_t max_n) {
  std::set<std::pair<uint32_t, uint16_t>> full_keys;
  for (const auto& c : cells) {
    if (cell_is_full_lte(c)) full_keys.insert({c.radio.freq(), c.radio.pci_bsic()});
  }
  const auto full_on_earfcn = full_count_by_earfcn(cells);

  std::map<std::pair<uint32_t, uint16_t>, HopTarget> best;
  auto consider = [&](uint32_t earfcn, uint16_t pci, float rsrp, uint16_t mcc, uint16_t mnc) {
    if (!hop_earfcn_ok(earfcn) || pci == 0 || pci > 503) return;
    if (full_keys.contains({earfcn, pci})) return;
    const auto key = std::pair{earfcn, pci};
    auto it = best.find(key);
    if (it == best.end() || rsrp > it->second.rsrp)
      best[key] = HopTarget{.earfcn = earfcn,
                            .pci = pci,
                            .rsrp = rsrp,
                            .mcc = mcc,
                            .mnc = mnc,
                            .has_full = false,
                            .camped = false};
  };

  for (const auto& c : cells) {
    if (c.rat != RatType::LTE) continue;
    const uint32_t earfcn = c.radio.freq();
    if (!hop_earfcn_ok(earfcn)) continue;
    if (!cell_is_full_lte(c)) {
      float rsrp = -160.0f;
      if (auto* s = c.signal.get_if<LteSignalParams>()) {
        if (Utils::valid_lte_rsrp(s->rsrp)) rsrp = s->rsrp;
      }
      consider(earfcn, c.radio.pci_bsic(), rsrp, c.passport.mcc, c.passport.mnc);
    }
    for (const auto& cf : c.radio.inter_freq_carriers) {
      if (!hop_earfcn_ok(cf.earfcn)) continue;
      for (uint16_t pci : cf.neigh_pcis)
        consider(cf.earfcn, pci, -155.0f, c.passport.mcc, c.passport.mnc);
    }
  }

  std::vector<HopTarget> ranked;
  ranked.reserve(best.size());
  for (const auto& [_, t] : best) ranked.push_back(t);
  std::sort(ranked.begin(), ranked.end(), [&](const HopTarget& a, const HopTarget& b) {
    return hop_target_better(a, b, full_on_earfcn, /*full_walk=*/false);
  });
  if (ranked.size() > max_n) ranked.resize(max_n);
  return ranked;
}

/// Full walk: every visible EARFCN|PCI that is not yet camped (incl. uncamped
/// FULL). Priority: FDD → new carrier (0 FULL) → new PLMN → measured → RSRP.
/// Extra SIB1 PLMNs on a camped EARFCN inherit that grid's camp count.
[[nodiscard]] inline std::vector<HopTarget> pick_full_walk_targets(
    const std::vector<CellIdentity>& cells, std::size_t max_n) {
  const auto camped_per_plmn = camped_count_by_plmn(cells);
  const auto earfcn_plmn_camps = camped_plmn_n_by_earfcn(cells, camped_per_plmn);
  const auto full_on_earfcn = full_count_by_earfcn(cells);

  std::map<std::pair<uint32_t, uint16_t>, HopTarget> best;
  auto upsert = [&](HopTarget t) {
    if (!hop_earfcn_ok(t.earfcn) || t.pci == 0 || t.pci > 503) return;
    if (t.camped) return;  // already walked / really camped
    t.plmn_camp_n =
        full_walk_plmn_camp_n(t.earfcn, t.mcc, t.mnc, camped_per_plmn, earfcn_plmn_camps);
    const auto key = std::pair{t.earfcn, t.pci};
    auto it = best.find(key);
    if (it == best.end()) {
      best.emplace(key, t);
      return;
    }
    // Prefer richer identity / stronger signal.
    if (t.has_full && !it->second.has_full) {
      t.rsrp = std::max(t.rsrp, it->second.rsrp);
      it->second = t;
    } else if (t.mcc != 0 && it->second.mcc == 0) {
      t.rsrp = std::max(t.rsrp, it->second.rsrp);
      it->second = t;
    } else if (t.rsrp > it->second.rsrp) {
      t.has_full = t.has_full || it->second.has_full;
      t.camped = false;
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
    if (c.rat != RatType::LTE) continue;
    const uint32_t earfcn = c.radio.freq();
    const uint16_t pci = c.radio.pci_bsic();
    if (!hop_earfcn_ok(earfcn) || pci == 0) continue;
    float rsrp = -160.0f;
    if (auto* s = c.signal.get_if<LteSignalParams>()) {
      if (Utils::valid_lte_rsrp(s->rsrp)) rsrp = s->rsrp;
    }
    HopTarget t{.earfcn = earfcn,
                .pci = pci,
                .rsrp = rsrp,
                .mcc = c.passport.mcc,
                .mnc = c.passport.mnc,
                .has_full = cell_is_full_lte(c),
                .camped = c.ever_serving};
    upsert(t);
    for (const auto& cf : c.radio.inter_freq_carriers) {
      if (!hop_earfcn_ok(cf.earfcn)) continue;
      for (uint16_t npci : cf.neigh_pcis) {
        upsert(HopTarget{.earfcn = cf.earfcn,
                         .pci = npci,
                         .rsrp = -155.0f,
                         .mcc = c.passport.mcc,
                         .mnc = c.passport.mnc,
                         .has_full = false,
                         .camped = false});
      }
    }
    for (const auto& n : c.radio.meas_neighbors) {
      if (n.pci == 0 || n.pci > 503) continue;
      float nr = n.has_rsrp ? n.rsrp_dbm : -160.0f;
      upsert(HopTarget{.earfcn = earfcn,
                       .pci = n.pci,
                       .rsrp = nr,
                       .mcc = c.passport.mcc,
                       .mnc = c.passport.mnc,
                       .has_full = false,
                       .camped = false});
    }
  }

  std::vector<HopTarget> ranked;
  ranked.reserve(best.size());
  for (const auto& [_, t] : best) ranked.push_back(t);
  std::sort(ranked.begin(), ranked.end(), [&](const HopTarget& a, const HopTarget& b) {
    return hop_target_better(a, b, full_on_earfcn, /*full_walk=*/true);
  });
  if (ranked.size() > max_n) ranked.resize(max_n);
  return ranked;
}

/// Measured ML1/CMGRMI RSRP. SIB5 hop seeds are −155; SSS ghosts typically −160.
[[nodiscard]] inline bool hop_rsrp_measured(float rsrp) noexcept { return rsrp > -154.0f; }

using HopKey = std::pair<uint32_t, uint16_t>;

[[nodiscard]] inline HopTarget hop_target_from_snapshot(uint32_t earfcn, uint16_t pci,
                                                        const std::vector<CellIdentity>& cells) {
  HopTarget t{.earfcn = earfcn, .pci = pci, .rsrp = -160.0f};
  for (const auto& c : cells) {
    if (c.rat != RatType::LTE) continue;
    if (c.radio.freq() != earfcn || c.radio.pci_bsic() != pci) continue;
    t.has_full = cell_is_full_lte(c);
    t.camped = c.ever_serving;
    t.mcc = c.passport.mcc;
    t.mnc = c.passport.mnc;
    if (auto* s = c.signal.get_if<LteSignalParams>()) {
      if (Utils::valid_lte_rsrp(s->rsrp)) t.rsrp = s->rsrp;
    }
    break;
  }
  return t;
}

/// After COPS=?: new FDD carriers first. Uncamped FULL is next (camp → neigh list),
/// but QMI paper-FULL on a busy EARFCN must not beat COPS=? RF on an empty one.
[[nodiscard]] inline bool hop_seed_better(const HopTarget& a, const HopTarget& b,
                                          const std::map<uint32_t, int>& full_on_earfcn,
                                          bool full_walk) {
  const bool a_fdd = hop_earfcn_is_fdd(a.earfcn);
  const bool b_fdd = hop_earfcn_is_fdd(b.earfcn);
  if (a_fdd != b_fdd) return a_fdd && !b_fdd;
  if (full_walk) {
    const int a_n = full_on_earfcn.contains(a.earfcn) ? full_on_earfcn.at(a.earfcn) : 0;
    const int b_n = full_on_earfcn.contains(b.earfcn) ? full_on_earfcn.at(b.earfcn) : 0;
    if ((a_n == 0) != (b_n == 0)) return a_n == 0;
  }
  if (a.has_full != b.has_full) return a.has_full && !b.has_full;
  return hop_target_better(a, b, full_on_earfcn, full_walk);
}

/// After COPS=?: RF rows from the snapshot, including unmeasured (empty rxl).
/// SSS ghosts (−160) on an already-FULL EARFCN stay out (mill). Measured siblings
/// (QMI/ML1 RSRP) are seeds so intra PCI get a kick when CMGRMI is empty.
/// Uncamped FULL is a seed so we can camp and pull the neighbor list.
[[nodiscard]] inline std::vector<HopTarget> pick_seed_targets(const std::vector<CellIdentity>& cells,
                                                              std::size_t max_n, bool full_walk) {
  const auto full_on_earfcn = full_count_by_earfcn(cells);
  const auto camped_per_plmn = camped_count_by_plmn(cells);
  const auto earfcn_plmn_camps = camped_plmn_n_by_earfcn(cells, camped_per_plmn);

  std::map<HopKey, HopTarget> best;
  for (const auto& c : cells) {
    if (c.rat != RatType::LTE) continue;
    const uint32_t earfcn = c.radio.freq();
    const uint16_t pci = c.radio.pci_bsic();
    if (!hop_earfcn_ok(earfcn) || pci == 0 || pci > 503) continue;
    const bool full = cell_is_full_lte(c);
    if (full_walk) {
      if (c.ever_serving) continue;
    } else if (full) {
      continue;
    }
    float rsrp = -160.0f;
    if (auto* s = c.signal.get_if<LteSignalParams>()) {
      if (Utils::valid_lte_rsrp(s->rsrp)) rsrp = s->rsrp;
    }
    const int n_full = full_on_earfcn.contains(earfcn) ? full_on_earfcn.at(earfcn) : 0;
    if (n_full > 0 && !full && !hop_rsrp_measured(rsrp)) continue;  // SSS mill
    HopTarget t{.earfcn = earfcn,
                .pci = pci,
                .rsrp = rsrp,
                .mcc = c.passport.mcc,
                .mnc = c.passport.mnc,
                .has_full = full,
                .camped = false};
    if (full_walk)
      t.plmn_camp_n =
          full_walk_plmn_camp_n(earfcn, t.mcc, t.mnc, camped_per_plmn, earfcn_plmn_camps);
    const auto key = HopKey{earfcn, pci};
    auto it = best.find(key);
    if (it == best.end() || t.rsrp > it->second.rsrp) best[key] = t;
  }

  std::vector<HopTarget> ranked;
  ranked.reserve(best.size());
  for (const auto& [_, t] : best) ranked.push_back(t);
  std::sort(ranked.begin(), ranked.end(), [&](const HopTarget& a, const HopTarget& b) {
    return hop_seed_better(a, b, full_on_earfcn, full_walk);
  });
  if (ranked.size() > max_n) ranked.resize(max_n);
  return ranked;
}

/// PCI neighbors advertised by a camped/serving cell (inspector: meas + SIB4 + SIB5 PCI).
/// Used when CMGRMI=4 is ERROR (EMM DEREGISTERED) so the whitelist is still the live list.
[[nodiscard]] inline std::set<HopKey> serving_neigh_hop_keys(const std::vector<CellIdentity>& cells) {
  std::set<HopKey> keys;
  auto take = [&](uint32_t earfcn, uint16_t pci) {
    if (!hop_earfcn_ok(earfcn) || pci == 0 || pci > 503) return;
    keys.emplace(earfcn, pci);
  };
  for (const auto& c : cells) {
    if (c.rat != RatType::LTE) continue;
    if (!c.ever_serving && !c.is_serving) continue;
    const uint32_t earfcn = c.radio.freq();
    const uint16_t spci = c.radio.pci_bsic();
    for (const auto& n : c.radio.meas_neighbors) take(earfcn, n.pci);
    for (const auto& n : c.radio.intra_freq_neighbors) take(earfcn, n.pci);
    for (const auto& car : c.radio.inter_freq_carriers) {
      for (uint16_t pci : car.neigh_pcis) take(car.earfcn, pci);
    }
    keys.erase({earfcn, spci});
  }
  return keys;
}

/// Incomplete PCI with measured RSRP on a carrier that already has FULL (QMI/ML1 intra).
/// SSS ghosts stay out — those wait for serving-neigh / CMGRMI.
[[nodiscard]] inline std::set<HopKey> measured_intra_hop_keys(const std::vector<CellIdentity>& cells) {
  const auto full_on_earfcn = full_count_by_earfcn(cells);
  std::set<HopKey> keys;
  for (const auto& c : cells) {
    if (c.rat != RatType::LTE) continue;
    const uint32_t earfcn = c.radio.freq();
    const uint16_t pci = c.radio.pci_bsic();
    if (!hop_earfcn_ok(earfcn) || pci == 0 || pci > 503) continue;
    if (c.ever_serving || cell_is_full_lte(c)) continue;
    const int n_full = full_on_earfcn.contains(earfcn) ? full_on_earfcn.at(earfcn) : 0;
    if (n_full == 0) continue;
    float rsrp = -160.0f;
    if (auto* s = c.signal.get_if<LteSignalParams>()) {
      if (Utils::valid_lte_rsrp(s->rsrp)) rsrp = s->rsrp;
    }
    if (!hop_rsrp_measured(rsrp)) continue;
    keys.emplace(earfcn, pci);
  }
  return keys;
}

/// Advertised + measured neighbors that are not camped yet — stay on these before band-wave.
[[nodiscard]] inline std::set<HopKey> pending_neigh_hop_keys(const std::vector<CellIdentity>& cells) {
  auto keys = serving_neigh_hop_keys(cells);
  auto meas = measured_intra_hop_keys(cells);
  keys.insert(meas.begin(), meas.end());
  for (const auto& c : cells) {
    if (c.rat != RatType::LTE) continue;
    if (!c.ever_serving) continue;
    keys.erase({c.radio.freq(), c.radio.pci_bsic()});
  }
  return keys;
}

/// SIB5 EARFCNs with no PCI list and no RF row yet — CLEARFCN listen, then grind minted PCI.
[[nodiscard]] inline std::vector<uint32_t> sib5_bare_earfcns(const std::vector<CellIdentity>& cells) {
  std::set<uint32_t> advertised;
  std::set<uint32_t> have_pci;
  for (const auto& c : cells) {
    if (c.rat != RatType::LTE) continue;
    const uint32_t earfcn = c.radio.freq();
    if (hop_earfcn_ok(earfcn) && c.radio.pci_bsic() > 0 && c.radio.pci_bsic() <= 503)
      have_pci.insert(earfcn);
    if (!c.ever_serving && !c.is_serving) continue;
    for (const auto& car : c.radio.inter_freq_carriers) {
      if (!hop_earfcn_ok(car.earfcn)) continue;
      if (!car.neigh_pcis.empty()) continue;
      advertised.insert(car.earfcn);
    }
  }
  std::vector<uint32_t> out;
  for (uint32_t e : advertised) {
    if (!have_pci.contains(e)) out.push_back(e);
  }
  std::sort(out.begin(), out.end(), [](uint32_t a, uint32_t b) {
    const bool a_fdd = hop_earfcn_is_fdd(a);
    const bool b_fdd = hop_earfcn_is_fdd(b);
    if (a_fdd != b_fdd) return a_fdd && !b_fdd;
    return a < b;
  });
  return out;
}

/// After CMGRMI=4: grind only these intra/interfreq PCI keys.
[[nodiscard]] inline std::vector<HopTarget> pick_neigh_targets(
    const std::vector<CellIdentity>& cells, const std::set<HopKey>& allow, std::size_t max_n,
    bool full_walk) {
  const auto full_on_earfcn = full_count_by_earfcn(cells);
  const auto camped_per_plmn = camped_count_by_plmn(cells);
  const auto earfcn_plmn_camps = camped_plmn_n_by_earfcn(cells, camped_per_plmn);

  std::vector<HopTarget> ranked;
  ranked.reserve(allow.size());
  for (const auto& [earfcn, pci] : allow) {
    if (!hop_earfcn_ok(earfcn) || pci == 0 || pci > 503) continue;
    HopTarget t = hop_target_from_snapshot(earfcn, pci, cells);
    if (full_walk) {
      if (t.camped) continue;
      t.plmn_camp_n =
          full_walk_plmn_camp_n(earfcn, t.mcc, t.mnc, camped_per_plmn, earfcn_plmn_camps);
    } else if (t.has_full) {
      continue;
    }
    ranked.push_back(t);
  }
  std::sort(ranked.begin(), ranked.end(), [&](const HopTarget& a, const HopTarget& b) {
    return hop_target_better(a, b, full_on_earfcn, full_walk);
  });
  if (ranked.size() > max_n) ranked.resize(max_n);
  return ranked;
}

}  // namespace QCom::Lte
