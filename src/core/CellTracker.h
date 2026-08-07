/// @file CellTracker.h
/// @brief Single source of truth for cell state, aggregated from parser events.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "core/BandInfo.h"
#include "core/CellIdentity.h"
#include "core/Events.h"
#include "core/Utils.h"

#include <algorithm>
#include <set>

namespace QCom {

class CellTracker {
public:
  CellTracker() = default;
  ~CellTracker() = default;

  CellTracker(const CellTracker&) = delete;
  CellTracker& operator=(const CellTracker&) = delete;

  /// DIAG event 500 (GPRS_SURROUND_SEARCH_START): next GSM SI-3 binds to this ARFCN.
  void set_gsm_surround_arfcn_hint(uint16_t arfcn) {
    std::lock_guard lock(m_mutex);
    if (!arfcn || arfcn > 1023) return;
    m_surround_arfcn_hint = arfcn;
    m_surround_hint_fresh = true;
  }

  void handle_rrc_event(Events::RrcEventEnvelope&& envelope) {
    std::lock_guard lock(m_mutex);

    std::visit(
        [&](auto& e) {
          using T = std::decay_t<decltype(e)>;

          if constexpr (std::is_same_v<T, Events::GsmNeighborsEvent>) {
            upsert_gsm_neighbors(envelope.rat, e.neighbors);
            for (const auto& n : e.neighbors) {
              LocalCellKey nk{.freq = n.arfcn,
                              .pci_bsic = n.bsic_valid ? static_cast<uint16_t>(n.bsic)
                                                       : uint16_t{0}};
              if (auto it = m_registry.find(nk); it != m_registry.end())
                touch_seen(it->second, envelope.timestamp, envelope.wall_time);
            }
            return;
          } else if constexpr (std::is_same_v<T, Events::WcdmaNeighborsEvent>) {
            upsert_wcdma_neighbors(envelope.rat, e.neighbors);
            return;
          } else if constexpr (std::is_same_v<T, Events::NeighborMeasEvent>) {
            if (envelope.rat == RatType::NR)
              upsert_nr_meas_neighbors(envelope.key, envelope.rat, e.neighbors);
            else
              upsert_lte_meas_neighbors(envelope.key, envelope.rat, e.neighbors);
            return;
          }

          LocalCellKey key = envelope.key;

          if constexpr (std::is_same_v<T, Events::PassportEvent>) {
            if (key.freq == 0 && key.pci_bsic == 0) {
              key = resolve_passport_key(envelope.rat, e.passport);
              if (key.freq == 0 && key.pci_bsic == 0) {
                if (e.passport.cell_id != 0) m_pending_passports[e.passport.cell_id] = e.passport;
                return;
              }
            }
            // Promote CID-only GSM row {0,cid} onto a physical ARFCN key when known.
            if (envelope.rat == RatType::GSM && e.passport.cell_id != 0 && key.freq != 0)
              relocate_gsm_cid_row(key, e.passport.cell_id);
            // LTE PLMN-only EARFCN|0 → attach/fan-out onto EARFCN|PCI rows.
            if (envelope.rat == RatType::LTE && key.freq != 0 && key.pci_bsic == 0) {
              if (attach_or_fanout_lte_weak_passport(key, e.passport, envelope.timestamp,
                                                    envelope.wall_time))
                return;  // fan-out already applied
            }
          } else if (key.freq == 0 && key.pci_bsic == 0) {
            key = serving_key_unlocked(envelope.rat);
            if (key.freq == 0 && key.pci_bsic == 0) return;  // never create {0,0}
          }

          // LTE: absorb EARFCN|0 into EARFCN|PCI when the physical key appears.
          if (envelope.rat == RatType::LTE && key.freq != 0 && key.pci_bsic != 0)
            promote_lte_weak_row(key.freq, key.pci_bsic);

          auto& cell = m_registry[key];
          cell.rat = envelope.rat;
          if (cell.radio.radio_data.index() == 0) init_radio_params(cell, key, envelope.rat);
          touch_seen(cell, envelope.timestamp, envelope.wall_time);

          if constexpr (std::is_same_v<T, Events::PassportEvent>) {
            CellPassport pass = e.passport;
            // LTE: never bind ECI/TAC to EARFCN|0 — that minted fake FULL WEAK rows.
            if (envelope.rat == RatType::LTE && key.pci_bsic == 0) {
              pass.cell_id = 0;
              pass.tac = 0;
            }
            merge_passport(cell.passport, pass);
            // Identity may arrive after a brief RF lock — promote sticky camp then.
            if (cell.is_serving && cell.passport.has_identity()) cell.ever_serving = true;
            if (pass.cell_id != 0) {
              m_pending_passports.erase(pass.cell_id);
              // One ECI → one physical key; strip clones (CEREG mis-stamp, etc.).
              if (envelope.rat == RatType::LTE && key.freq != 0 && key.pci_bsic != 0)
                claim_lte_eci(key, pass.cell_id);
            }
            // Spread MCC/MNC to other PCI rows on this EARFCN (not CID/TAC — cell-specific).
            if (envelope.rat == RatType::LTE && key.freq != 0 && key.pci_bsic != 0 &&
                pass.mcc != 0)
              fanout_lte_plmn_same_earfcn(key.freq, pass);
          } else if constexpr (std::is_same_v<T, Events::IntraNeighborsEvent>) {
            merge_intra_neighbors(cell.radio.intra_freq_neighbors, e.neighbors);
            // SIB4 PCIs → registry RADIO rows (same EARFCN as serving), like ML1 meas.
            upsert_lte_sib_intra_neighbors(key, envelope.rat, e.neighbors);
          } else if constexpr (std::is_same_v<T, Events::InterFreqCarriersEvent>) {
            merge_inter_freq_carriers(cell.radio.inter_freq_carriers, e.carriers);
            upsert_lte_sib_inter_neighbors(key, envelope.rat, e.carriers);
          } else if constexpr (std::is_same_v<T, Events::UtraNeighborsEvent>) {
            if (!e.neighbors.empty()) cell.radio.utra_neighbors = e.neighbors;
          } else if constexpr (std::is_same_v<T, Events::GeranNeighborsEvent>) {
            if (!e.neighbors.empty()) cell.radio.geran_neighbors = e.neighbors;
          } else if constexpr (std::is_same_v<T, Events::ServingChangedEvent>) {
            if (e.is_serving) {
              for (auto& [_, c] : m_registry) {
                if (c.rat == envelope.rat) c.is_serving = false;
              }
              // Sticky "camped" only after real identity (SIB1/CPSI/CGI).
              // CCELLCFG / brief ML1 locks otherwise paint hundreds of RADIO rows as CAMPED.
              if (cell.passport.has_identity()) cell.ever_serving = true;
            }
            cell.is_serving = e.is_serving;
          } else if constexpr (std::is_same_v<T, Events::SignalUpdateEvent>) {
            update_cell_signal(cell, e.signal);
          } else if constexpr (std::is_same_v<T, Events::GenericRadioParamsEvent>) {
            apply_radio_params(cell, e);
            apply_pending_passport(cell, key);
            if (envelope.rat == RatType::GSM && cell.passport.cell_id != 0 && key.freq != 0)
              relocate_gsm_cid_row(key, cell.passport.cell_id);
          }
        },
        envelope.event_data);
  }

  void handle_l1_signal(LocalCellKey key, RatType rat, CellSignal&& signal) {
    std::lock_guard lock(m_mutex);
    if (key.freq == 0 && key.pci_bsic == 0) return;
    if (rat == RatType::LTE && key.freq != 0 && key.pci_bsic != 0)
      promote_lte_weak_row(key.freq, key.pci_bsic);
    auto& cell = m_registry[key];
    cell.rat = rat;
    update_cell_signal(cell, signal);
    if (cell.radio.radio_data.index() == 0) init_radio_params(cell, key, rat);
  }

  std::vector<CellIdentity> get_snapshot() const {
    std::lock_guard lock(m_mutex);
    std::vector<CellIdentity> snapshot;
    snapshot.reserve(m_registry.size());
    for (const auto& [_, cell] : m_registry) snapshot.push_back(cell);
    return snapshot;
  }

  size_t cell_count() const {
    std::lock_guard lock(m_mutex);
    return m_registry.size();
  }

  [[nodiscard]] LocalCellKey serving_key(RatType rat) const {
    std::lock_guard lock(m_mutex);
    return serving_key_unlocked(rat);
  }

private:
  mutable std::mutex m_mutex;
  std::map<LocalCellKey, CellIdentity> m_registry;
  /// SI-3/SIB1 identity seen before a physical key (ARFCN/PCI) was known.
  std::map<uint64_t, CellPassport> m_pending_passports;
  uint16_t m_surround_arfcn_hint{0};
  bool m_surround_hint_fresh{false};

  static void touch_seen(CellIdentity& cell, uint64_t ts, std::string_view wall) {
    ++cell.seen;
    if (!wall.empty()) {
      if (cell.first_seen.empty()) cell.first_seen.assign(wall.begin(), wall.end());
      cell.last_seen.assign(wall.begin(), wall.end());
    } else if (ts != 0) {
      auto s = std::to_string(ts);
      if (cell.first_seen.empty()) cell.first_seen = s;
      cell.last_seen = std::move(s);
    }
  }

  /// Accumulate identity: fill empty fields; never clobber a conflicting value.
  /// Incomplete → complete upgrade allowed; different FULL identities stay sticky.
  static void merge_passport(CellPassport& dst, const CellPassport& src) {
    const bool src_eci = Utils::valid_lte_eci(src.cell_id);
    const bool dst_eci = Utils::valid_lte_eci(dst.cell_id);
    const bool src_tac = Utils::valid_lte_tac(src.tac);
    const bool dst_tac = Utils::valid_lte_tac(dst.tac);
    const bool src_plmn = src.mcc != 0;
    const bool dst_plmn = dst.mcc != 0;
    const bool src_full = src_eci && src_tac && src_plmn;
    const bool dst_full = dst_eci && dst_tac && dst_plmn;

    if (src_eci) {
      if (!dst_eci)
        dst.cell_id = src.cell_id;
      else if (dst.cell_id != src.cell_id && src_full && !dst_full)
        dst.cell_id = src.cell_id;
    }
    if (src_plmn) {
      if (!dst_plmn) {
        dst.mcc = src.mcc;
        dst.mnc = src.mnc;
      } else if ((dst.mcc != src.mcc || dst.mnc != src.mnc) && src_full && !dst_full) {
        dst.mcc = src.mcc;
        dst.mnc = src.mnc;
      }
    }
    if (src_tac) {
      if (!dst_tac)
        dst.tac = src.tac;
      else if (dst.tac != src.tac && src_full && !dst_full)
        dst.tac = src.tac;
    }
    if (src.freq_band_ind != 0) dst.freq_band_ind = src.freq_band_ind;
    if (src.q_rx_lev_min != 0) dst.q_rx_lev_min = src.q_rx_lev_min;
    if (src.q_rx_lev_min_offset != 0) dst.q_rx_lev_min_offset = src.q_rx_lev_min_offset;
    // Structural SIB1 flags: only when we are filling or upgrading identity.
    if (src_full || (!dst_full && (src_eci || src_plmn))) {
      dst.cell_barred = src.cell_barred;
      dst.intra_freq_reselection_allowed = src.intra_freq_reselection_allowed;
      dst.csg_ind = src.csg_ind;
      if (src.csg_id != 0) dst.csg_id = src.csg_id;
    }
  }

  [[nodiscard]] LocalCellKey serving_key_unlocked(RatType rat) const {
    for (const auto& [key, cell] : m_registry) {
      if (cell.rat == rat && cell.is_serving) return key;
    }
    return {};
  }

  [[nodiscard]] LocalCellKey find_key_by_cid(RatType rat, uint64_t cid) const {
    if (cid == 0) return {};
    for (const auto& [key, cell] : m_registry) {
      if (cell.rat == rat && cell.passport.cell_id == cid) return key;
    }
    // GSM passport-only rows are keyed as {freq=0, pci_bsic=cid}.
    if (rat == RatType::GSM && cid <= 0xFFFF) {
      LocalCellKey pk{.freq = 0, .pci_bsic = static_cast<uint16_t>(cid)};
      if (m_registry.contains(pk)) return pk;
    }
    return {};
  }

  /// Bind a passport with no physical key to an existing cell when possible.
  [[nodiscard]] LocalCellKey resolve_passport_key(RatType rat, const CellPassport& p) {
    if (p.cell_id == 0) {
      // LAI-only (SI-4 / NAS TAI): attach to serving.
      return serving_key_unlocked(rat);
    }

    if (auto k = find_key_by_cid(rat, p.cell_id); k.freq != 0 || k.pci_bsic != 0) return k;

    auto sk = serving_key_unlocked(rat);
    if (sk.freq != 0 || sk.pci_bsic != 0) {
      auto it = m_registry.find(sk);
      if (it != m_registry.end()) {
        const auto& srv = it->second;
        if (srv.passport.cell_id == 0 || srv.passport.cell_id == p.cell_id) return sk;
      }
    }

    // Vlad: first SI-3 after GPRS_SURROUND_SEARCH_START binds to that ARFCN.
    if (rat == RatType::GSM && m_surround_hint_fresh && m_surround_arfcn_hint != 0 &&
        m_surround_arfcn_hint != sk.freq) {
      const uint16_t arfcn = m_surround_arfcn_hint;
      m_surround_hint_fresh = false;
      LocalCellKey best{};
      bool found = false;
      for (const auto& [k, c] : m_registry) {
        if (c.rat != RatType::GSM || k.freq != arfcn) continue;
        if (!found || k.pci_bsic != 0) {
          best = k;
          found = true;
          if (k.pci_bsic != 0) break;
        }
      }
      if (found) return best;
      return {.freq = arfcn, .pci_bsic = 0};
    }

    // Passport-only GSM row (no ARFCN yet) — same as Vlad CID-without-arfcn.
    if (rat == RatType::GSM && p.cell_id <= 0xFFFF)
      return {.freq = 0, .pci_bsic = static_cast<uint16_t>(p.cell_id)};

    return {};
  }

  /// Move GSM|{0}|CID into a physical ARFCN key when identity is joined.
  void relocate_gsm_cid_row(LocalCellKey dest_key, uint64_t cid) {
    if (cid == 0 || cid > 0xFFFF || dest_key.freq == 0) return;
    LocalCellKey src{.freq = 0, .pci_bsic = static_cast<uint16_t>(cid)};
    if (src.freq == dest_key.freq && src.pci_bsic == dest_key.pci_bsic) return;
    absorb_row(dest_key, src);
  }

  /// Absorb LTE EARFCN|0 into EARFCN|PCI (mirror of GSM ARFCN|0 → ARFCN|BSIC).
  void promote_lte_weak_row(uint32_t earfcn, uint16_t pci) {
    if (earfcn == 0 || pci == 0 || pci > 503) return;
    absorb_row({.freq = earfcn, .pci_bsic = pci}, {.freq = earfcn, .pci_bsic = 0});
  }

  /// Copy MCC/MNC onto same-EARFCN PCI rows that still lack PLMN (CID/TAC stay cell-local).
  void fanout_lte_plmn_same_earfcn(uint32_t earfcn, const CellPassport& p) {
    if (earfcn == 0 || p.mcc == 0) return;
    for (auto& [k, cell] : m_registry) {
      if (cell.rat != RatType::LTE || k.freq != earfcn || k.pci_bsic == 0) continue;
      if (cell.passport.mcc != 0) continue;
      cell.passport.mcc = p.mcc;
      cell.passport.mnc = p.mnc;
    }
  }

  /// LTE ECI is unique: after binding CID to `owner`, clear CID/TAC on impostor rows.
  void claim_lte_eci(LocalCellKey owner, uint64_t cid) {
    if (cid == 0) return;
    for (auto& [k, cell] : m_registry) {
      if (cell.rat != RatType::LTE) continue;
      if (k.freq == owner.freq && k.pci_bsic == owner.pci_bsic) continue;
      if (cell.passport.cell_id != cid) continue;
      cell.passport.cell_id = 0;
      cell.passport.tac = 0;
    }
  }

  /// Passport arrived on EARFCN|0. Returns true if fan-out finished (caller must return).
  /// On single PCI-row attach, updates `key` in place and returns false so normal merge runs.
  [[nodiscard]] bool attach_or_fanout_lte_weak_passport(LocalCellKey& key, const CellPassport& p,
                                                       uint64_t ts, std::string_view wall) {
    if (key.freq == 0 || key.pci_bsic != 0) return false;

    std::vector<LocalCellKey> pci_rows;
    for (const auto& [k, c] : m_registry) {
      if (c.rat != RatType::LTE || k.freq != key.freq || k.pci_bsic == 0) continue;
      if (p.cell_id != 0 && c.passport.cell_id != 0 && c.passport.cell_id != p.cell_id) continue;
      pci_rows.push_back(k);
    }

    if (pci_rows.size() == 1) {
      key = pci_rows[0];
      return false;
    }

    // PLMN-only (no CID): copy MCC/MNC onto every PCI row on this EARFCN.
    if (p.cell_id == 0 && p.mcc != 0 && pci_rows.size() > 1) {
      for (const auto& k : pci_rows) {
        auto& cell = m_registry[k];
        merge_passport(cell.passport, p);
        touch_seen(cell, ts, wall);
      }
      // Drop the weak row if it already exists — info lives on PCI keys now.
      m_registry.erase(key);
      return true;
    }

    return false;  // keep / create EARFCN|0 until a unique PCI appears
  }

  void absorb_row(LocalCellKey dest_key, LocalCellKey src_key) {
    if (src_key.freq == dest_key.freq && src_key.pci_bsic == dest_key.pci_bsic) return;
    auto it = m_registry.find(src_key);
    if (it == m_registry.end()) return;

    auto& dest = m_registry[dest_key];
    dest.rat = it->second.rat;
    merge_passport(dest.passport, it->second.passport);
    if (dest.radio.radio_data.index() == 0)
      dest.radio = std::move(it->second.radio);
    else {
      // Prefer non-empty neighbor lists / radio fields from the absorbed row.
      if (dest.radio.intra_freq_neighbors.empty())
        dest.radio.intra_freq_neighbors = std::move(it->second.radio.intra_freq_neighbors);
      if (dest.radio.inter_freq_carriers.empty())
        dest.radio.inter_freq_carriers = std::move(it->second.radio.inter_freq_carriers);
      if (dest.radio.meas_neighbors.empty())
        dest.radio.meas_neighbors = std::move(it->second.radio.meas_neighbors);
      if (auto* lr = it->second.radio.get_if<LteRadioParams>()) {
        Events::RadioParamsEvent<LteRadioParams> ev{.data = *lr};
        apply_radio_params(dest, Events::GenericRadioParamsEvent{std::move(ev)});
      } else if (auto* gr = it->second.radio.get_if<GsmRadioParams>()) {
        Events::RadioParamsEvent<GsmRadioParams> ev{.data = *gr};
        apply_radio_params(dest, Events::GenericRadioParamsEvent{std::move(ev)});
      }
    }
    if (dest.signal.signal_data.index() == 0 && it->second.signal.signal_data.index() != 0)
      dest.signal = std::move(it->second.signal);
    dest.seen += it->second.seen;
    if (dest.first_seen.empty()) dest.first_seen = std::move(it->second.first_seen);
    if (!it->second.last_seen.empty()) dest.last_seen = std::move(it->second.last_seen);
    if (it->second.is_serving) dest.is_serving = true;
    if (it->second.ever_serving) dest.ever_serving = true;
    m_registry.erase(it);
  }

  void apply_pending_passport(CellIdentity& cell, LocalCellKey key) {
    if (cell.passport.cell_id != 0) {
      m_pending_passports.erase(cell.passport.cell_id);
      return;
    }
    // Serving Cell Info with ARFCN but no CID yet: attach newest pending if unique.
    if (!cell.is_serving || key.freq == 0 || m_pending_passports.empty()) return;
    if (m_pending_passports.size() == 1) {
      merge_passport(cell.passport, m_pending_passports.begin()->second);
      m_pending_passports.erase(m_pending_passports.begin());
    }
  }

  void upsert_gsm_neighbors(RatType rat, const std::vector<GsmNeighborCell>& neighbors) {
    // Attach BA snapshot to serving GSM cell for nb_gsm export.
    auto sk = serving_key_unlocked(RatType::GSM);
    if (sk.freq != 0 || sk.pci_bsic != 0) {
      auto it = m_registry.find(sk);
      if (it != m_registry.end()) it->second.radio.gsm_neighbors = neighbors;
    }

    for (const auto& n : neighbors) {
      if (n.arfcn == 0 || n.arfcn > 1023) continue;
      uint16_t bsic = n.bsic_valid ? n.bsic : uint16_t{0};
      LocalCellKey key{.freq = n.arfcn, .pci_bsic = bsic};

      // Promote ARFCN|0 (SI-3 surround / acq without BSIC) onto ARFCN|BSIC.
      if (bsic != 0) {
        LocalCellKey weak{.freq = n.arfcn, .pci_bsic = 0};
        auto wit = m_registry.find(weak);
        if (wit != m_registry.end() && (key.freq != weak.freq || key.pci_bsic != weak.pci_bsic)) {
          auto& dest = m_registry[key];
          dest.rat = RatType::GSM;
          merge_passport(dest.passport, wit->second.passport);
          if (dest.radio.radio_data.index() == 0) dest.radio = std::move(wit->second.radio);
          if (dest.signal.signal_data.index() == 0 && wit->second.signal.signal_data.index() != 0)
            dest.signal = std::move(wit->second.signal);
          dest.seen += wit->second.seen;
          if (dest.first_seen.empty()) dest.first_seen = std::move(wit->second.first_seen);
          if (!wit->second.last_seen.empty()) dest.last_seen = std::move(wit->second.last_seen);
          if (wit->second.is_serving) dest.is_serving = true;
          if (wit->second.ever_serving) dest.ever_serving = true;
          m_registry.erase(wit);
        }
      }

      auto& cell = m_registry[key];
      cell.rat = RatType::GSM;
      if (cell.radio.radio_data.index() == 0) init_radio_params(cell, key, RatType::GSM);
      if (auto* g = cell.radio.get_if<GsmRadioParams>()) {
        g->arfcn = n.arfcn;
        if (n.bsic_valid) {
          g->bsic = n.bsic;
          g->ncc = (n.bsic >> 3) & 7;
          g->bcc = n.bsic & 7;
        }
      }
      if (n.rxlev != 0) {
        CellSignal sig;
        sig.signal_data = GsmSignalParams{.rxlev = static_cast<int8_t>(n.rxlev)};
        update_cell_signal(cell, sig);
      }
      (void)rat;
    }
  }

  void upsert_wcdma_neighbors(RatType rat, const std::vector<WcdmaNeighborCell>& neighbors) {
    auto sk = serving_key_unlocked(RatType::WCDMA);
    if (sk.freq != 0 || sk.pci_bsic != 0) {
      auto it = m_registry.find(sk);
      if (it != m_registry.end()) it->second.radio.wcdma_neighbors = neighbors;
    }

    for (const auto& n : neighbors) {
      if (n.uarfcn == 0) continue;
      LocalCellKey key{.freq = n.uarfcn, .pci_bsic = n.psc};
      auto& cell = m_registry[key];
      cell.rat = RatType::WCDMA;
      if (cell.radio.radio_data.index() == 0) init_radio_params(cell, key, RatType::WCDMA);
      CellSignal sig;
      sig.signal_data = WcdmaSignalParams{
          .rscp = static_cast<float>(n.rscp),
          .ecio = static_cast<float>(n.ecio),
          .has_ecio = true,
      };
      update_cell_signal(cell, sig);
      (void)rat;
    }
  }

  void upsert_lte_meas_neighbors(LocalCellKey serving, RatType rat,
                                const std::vector<NeighborMeasResult>& neighbors) {
    uint32_t earfcn = serving.freq;
    if (earfcn == 0) {
      auto sk = serving_key_unlocked(rat);
      earfcn = sk.freq;
      if (serving.pci_bsic == 0) serving = sk;
    }
    if (earfcn == 0) return;

    // Keep the list on the serving cell for SDR-style nb_lte expansion.
    if (serving.freq != 0 || serving.pci_bsic != 0) {
      auto it = m_registry.find(serving);
      if (it == m_registry.end()) {
        auto sk = serving_key_unlocked(rat);
        it = m_registry.find(sk);
      }
      if (it != m_registry.end()) {
        merge_meas_neighbors(it->second.radio.meas_neighbors, neighbors);
      }
    }

    for (const auto& n : neighbors) {
      if (n.pci > 503) continue;
      LocalCellKey key{.freq = earfcn, .pci_bsic = n.pci};
      promote_lte_weak_row(earfcn, n.pci);
      auto& cell = m_registry[key];
      cell.rat = RatType::LTE;
      if (cell.radio.radio_data.index() == 0) init_radio_params(cell, key, RatType::LTE);
      // Same-EARFCN only: fill PLMN if empty (never overwrite — SIB1/CPSI win later).
      if (cell.passport.mcc == 0) {
        auto sit = m_registry.find(serving);
        if (sit == m_registry.end()) sit = m_registry.find(serving_key_unlocked(rat));
        if (sit != m_registry.end() && sit->second.passport.mcc != 0 &&
            sit->second.radio.freq() == earfcn) {
          cell.passport.mcc = sit->second.passport.mcc;
          cell.passport.mnc = sit->second.passport.mnc;
        }
      }
      if (n.has_rsrp) {
        CellSignal sig;
        LteSignalParams lp;
        lp.rsrp = n.rsrp_dbm;
        if (n.has_rsrq) lp.rsrq = n.rsrq_db;
        if (n.has_sinr) {
          lp.sinr = n.sinr_db;
          lp.has_sinr = true;
        }
        sig.signal_data = lp;
        update_cell_signal(cell, sig);
      }
      if (n.has_cgi && n.cgi.has_identity()) {
        merge_passport(cell.passport, n.cgi);
        claim_lte_eci(key, n.cgi.cell_id);
      }
    }
  }

  /// SIB4 intra-freq neighbor list → EARFCN|PCI RADIO rows (no invented RSRP).
  void upsert_lte_sib_intra_neighbors(LocalCellKey serving, RatType rat,
                                     const std::vector<IntraFreqNeighbor>& neighbors) {
    uint32_t earfcn = serving.freq;
    if (earfcn == 0) earfcn = serving_key_unlocked(rat).freq;
    if (earfcn == 0) return;

    CellPassport plmn_src{};
    if (auto it = m_registry.find(serving); it != m_registry.end())
      plmn_src = it->second.passport;
    else if (auto sk = serving_key_unlocked(rat); sk.freq != 0) {
      if (auto it = m_registry.find(sk); it != m_registry.end()) plmn_src = it->second.passport;
    }

    for (const auto& n : neighbors) {
      if (n.pci > 503) continue;
      LocalCellKey key{.freq = earfcn, .pci_bsic = n.pci};
      promote_lte_weak_row(earfcn, n.pci);
      auto& cell = m_registry[key];
      cell.rat = RatType::LTE;
      if (cell.radio.radio_data.index() == 0) init_radio_params(cell, key, RatType::LTE);
      if (cell.passport.mcc == 0 && plmn_src.mcc != 0) {
        cell.passport.mcc = plmn_src.mcc;
        cell.passport.mnc = plmn_src.mnc;
      }
    }
  }

  /// SIB5 interFreqNeighCellList → RADIO rows on foreign EARFCNs (no invented RSRP/CID/PLMN).
  void upsert_lte_sib_inter_neighbors(LocalCellKey /*serving*/, RatType /*rat*/,
                                     const std::vector<InterFreqCarrier>& carriers) {
    for (const auto& car : carriers) {
      if (car.earfcn == 0 || car.earfcn > 262143) continue;
      for (uint16_t pci : car.neigh_pcis) {
        if (pci > 503) continue;
        LocalCellKey key{.freq = car.earfcn, .pci_bsic = pci};
        promote_lte_weak_row(car.earfcn, pci);
        auto& cell = m_registry[key];
        cell.rat = RatType::LTE;
        if (cell.radio.radio_data.index() == 0) init_radio_params(cell, key, RatType::LTE);
        // No serving-PLMN inheritance — foreign EARFCN is often another operator.
      }
    }
  }

  static void merge_intra_neighbors(std::vector<IntraFreqNeighbor>& dst,
                                    const std::vector<IntraFreqNeighbor>& src) {
    for (const auto& n : src) {
      if (n.pci > 503) continue;
      auto it = std::find_if(dst.begin(), dst.end(),
                             [&](const IntraFreqNeighbor& d) { return d.pci == n.pci; });
      if (it == dst.end())
        dst.push_back(n);
      else if (n.q_offset != 0)
        it->q_offset = n.q_offset;
    }
  }

  static void merge_inter_freq_carriers(std::vector<InterFreqCarrier>& dst,
                                        const std::vector<InterFreqCarrier>& src) {
    for (const auto& s : src) {
      if (s.earfcn == 0 || s.earfcn > 262143) continue;
      auto it = std::find_if(dst.begin(), dst.end(),
                             [&](const InterFreqCarrier& c) { return c.earfcn == s.earfcn; });
      if (it == dst.end()) {
        dst.push_back(s);
        continue;
      }
      if (s.thresh_x_high) it->thresh_x_high = s.thresh_x_high;
      if (s.thresh_x_low) it->thresh_x_low = s.thresh_x_low;
      if (s.q_rx_lev_min) it->q_rx_lev_min = s.q_rx_lev_min;
      if (s.cell_resel_prio) it->cell_resel_prio = s.cell_resel_prio;
      if (s.allowed_meas_bw) it->allowed_meas_bw = s.allowed_meas_bw;
      std::set<uint16_t> have(it->neigh_pcis.begin(), it->neigh_pcis.end());
      for (uint16_t pci : s.neigh_pcis) {
        if (pci > 503) continue;
        if (have.insert(pci).second) it->neigh_pcis.push_back(pci);
      }
    }
  }

  static void merge_meas_neighbors(std::vector<NeighborMeasResult>& dst,
                                   const std::vector<NeighborMeasResult>& src,
                                   uint16_t pci_max = 503) {
    auto by_pci = [](uint16_t pci, std::vector<NeighborMeasResult>& v) -> NeighborMeasResult* {
      for (auto& n : v)
        if (n.pci == pci) return &n;
      return nullptr;
    };
    for (const auto& s : src) {
      if (s.pci > pci_max) continue;
      if (auto* d = by_pci(s.pci, dst)) {
        if (s.has_rsrp) {
          d->rsrp_dbm = s.rsrp_dbm;
          d->has_rsrp = true;
        }
        if (s.has_rsrq) {
          d->rsrq_db = s.rsrq_db;
          d->has_rsrq = true;
        }
        if (s.has_sinr) {
          d->sinr_db = s.sinr_db;
          d->has_sinr = true;
        }
        if (s.has_cgi) {
          merge_passport(d->cgi, s.cgi);
          d->has_cgi = d->cgi.has_identity() || d->cgi.mcc != 0;
        }
      } else {
        dst.push_back(s);
      }
    }
  }

  /// NR ML1 / searcher neighbors — PCI 0..1007, RatType::NR registry rows.
  void upsert_nr_meas_neighbors(LocalCellKey serving, RatType rat,
                                const std::vector<NeighborMeasResult>& neighbors) {
    uint32_t nrarfcn = serving.freq;
    if (nrarfcn == 0) {
      auto sk = serving_key_unlocked(rat);
      nrarfcn = sk.freq;
      if (serving.pci_bsic == 0) serving = sk;
    }
    if (nrarfcn == 0) return;

    if (serving.freq != 0 || serving.pci_bsic != 0) {
      auto it = m_registry.find(serving);
      if (it == m_registry.end()) {
        auto sk = serving_key_unlocked(rat);
        it = m_registry.find(sk);
      }
      if (it != m_registry.end()) {
        merge_meas_neighbors(it->second.radio.meas_neighbors, neighbors, /*pci_max=*/1007);
      }
    }

    for (const auto& n : neighbors) {
      if (n.pci > 1007) continue;
      LocalCellKey key{.freq = nrarfcn, .pci_bsic = n.pci};
      auto& cell = m_registry[key];
      cell.rat = RatType::NR;
      if (cell.radio.radio_data.index() == 0) init_radio_params(cell, key, RatType::NR);
      if (n.has_rsrp) {
        CellSignal sig;
        NrSignalParams np;
        np.ss_rsrp = n.rsrp_dbm;
        if (n.has_rsrq) np.ss_rsrq = n.rsrq_db;
        if (n.has_sinr) np.ss_sinr = n.sinr_db;
        sig.signal_data = np;
        update_cell_signal(cell, sig);
      }
      if (n.has_cgi && n.cgi.has_identity()) merge_passport(cell.passport, n.cgi);
    }
  }

  template <RatType R>
  void emplace_and_fill(CellIdentity& cell, LocalCellKey key) {
    auto& params = cell.radio.radio_data.template emplace<RatRadio_t<R>>();
    if constexpr (requires { params.earfcn; })
      params.earfcn = key.freq;
    else if constexpr (requires { params.nrarfcn; })
      params.nrarfcn = key.freq;
    else if constexpr (requires { params.dl_uarfcn; })
      params.dl_uarfcn = key.freq;
    else if constexpr (requires { params.arfcn; })
      params.arfcn = key.freq;

    if constexpr (requires { params.pci; })
      params.pci = key.pci_bsic;
    else if constexpr (requires { params.psc; })
      params.psc = key.pci_bsic;
    else if constexpr (requires { params.bsic; }) {
      // GSM passport-only rows use key {0, cid} — do not treat cid as BSIC.
      if (key.freq != 0 && key.pci_bsic != 0) {
        params.bsic = key.pci_bsic;
        params.ncc = (key.pci_bsic >> 3) & 7;
        params.bcc = key.pci_bsic & 7;
      }
    }
  }

  void init_radio_params(CellIdentity& cell, LocalCellKey key, RatType rat) {
    switch (rat) {
      case RatType::LTE: emplace_and_fill<RatType::LTE>(cell, key); break;
      case RatType::NR: emplace_and_fill<RatType::NR>(cell, key); break;
      case RatType::WCDMA: emplace_and_fill<RatType::WCDMA>(cell, key); break;
      case RatType::GSM: emplace_and_fill<RatType::GSM>(cell, key); break;
      default: break;
    }
  }

  template <RatType R>
  void emplace_signal(CellIdentity& cell) {
    cell.signal.signal_data.template emplace<RatSignal_t<R>>();
  }

  void init_cell_signal(CellIdentity& cell) {
    if (cell.signal.signal_data.index() != 0) return;
    switch (cell.rat) {
      case RatType::LTE: emplace_signal<RatType::LTE>(cell); break;
      case RatType::NR: emplace_signal<RatType::NR>(cell); break;
      case RatType::WCDMA: emplace_signal<RatType::WCDMA>(cell); break;
      case RatType::GSM: emplace_signal<RatType::GSM>(cell); break;
      default: break;
    }
  }

  void update_cell_signal(CellIdentity& cell, const CellSignal& new_signal) {
    init_cell_signal(cell);
    std::visit(
        [&](const auto& incoming, auto& target) {
          using S = std::decay_t<decltype(incoming)>;
          using D = std::decay_t<decltype(target)>;
          if constexpr (!std::is_same_v<S, D>) {
            return;
          } else if constexpr (std::is_same_v<S, LteSignalParams>) {
            // Valid measured only — reject ghosts (−155) / zeros so sources accumulate.
            if (Utils::valid_lte_rsrp(incoming.rsrp)) target.rsrp = incoming.rsrp;
            if (incoming.rsrq >= -30.0f && incoming.rsrq <= -1.0f) target.rsrq = incoming.rsrq;
            if (incoming.has_rssi) {
              target.rssi = incoming.rssi;
              target.has_rssi = true;
            }
            if (incoming.has_sinr) {
              target.sinr = incoming.sinr;
              target.has_sinr = true;
            }
          } else if constexpr (std::is_same_v<S, WcdmaSignalParams>) {
            if (incoming.rscp != 0.0f) target.rscp = incoming.rscp;
            if (incoming.has_ecio) {
              target.ecio = incoming.ecio;
              target.has_ecio = true;
            }
          } else if constexpr (std::is_same_v<S, GsmSignalParams>) {
            if (incoming.rxlev != 0) target.rxlev = incoming.rxlev;
            if (incoming.rxqual) target.rxqual = incoming.rxqual;
            if (incoming.has_snr) {
              target.snr = incoming.snr;
              target.has_snr = true;
            }
            if (incoming.has_c1c2) {
              target.c1 = incoming.c1;
              target.c2 = incoming.c2;
              target.has_c1c2 = true;
            }
          } else if constexpr (std::is_same_v<S, NrSignalParams>) {
            target = incoming;
          } else {
            target = incoming;
          }
        },
        new_signal.signal_data, cell.signal.signal_data);

    refresh_gsm_c1c2(cell);
  }

  void refresh_gsm_c1c2(CellIdentity& cell) {
    if (cell.rat != RatType::GSM) return;
    auto* radio = cell.radio.get_if<GsmRadioParams>();
    auto* sig = cell.signal.get_if<GsmSignalParams>();
    if (!radio || !sig) return;
    if (radio->rxlev_access_min == 0 && radio->ms_txpwr_max_cch == 0) return;
    if (sig->rxlev == 0) return;

    auto band = BandInfo::gsm_from_arfcn(static_cast<uint16_t>(radio->arfcn),
                                         radio->band_class == 0xFF ? -1 : radio->band_class);
    int c1 = BandInfo::gsm_compute_c1(sig->rxlev, radio->rxlev_access_min, radio->ms_txpwr_max_cch,
                                      band.band);
    int c2 = BandInfo::gsm_compute_c2(c1, radio->reselect_params_present, radio->cell_reselect_offset,
                                      radio->temporary_offset, radio->penalty_time);
    sig->c1 = static_cast<int16_t>(c1);
    sig->c2 = static_cast<int16_t>(c2);
    sig->has_c1c2 = true;
  }

  void apply_radio_params(CellIdentity& cell, const Events::GenericRadioParamsEvent& generic) {
    // Ensure variant holds the right type before merge
    std::visit(
        [&](const auto& radio_event) {
          using EventData = std::decay_t<decltype(radio_event.data)>;
          if (!std::holds_alternative<EventData>(cell.radio.radio_data)) {
            cell.radio.radio_data.emplace<EventData>(radio_event.data);
            refresh_gsm_c1c2(cell);
            return;
          }
          std::visit(
              [&](auto& target) {
                using TargetType = std::decay_t<decltype(target)>;
                if constexpr (std::is_same_v<TargetType, EventData>) {
                  merge_radio_fields(target, radio_event.data);
                }
              },
              cell.radio.radio_data);
        },
        generic);
    refresh_gsm_c1c2(cell);
  }

  template <typename T>
  void merge_radio_fields(T& target, const T& src) {
    if constexpr (requires { target.earfcn; }) {
      if (src.earfcn) target.earfcn = src.earfcn;
    }
    if constexpr (requires { target.pci; }) {
      if (src.pci) target.pci = src.pci;
    }
    if constexpr (requires { target.arfcn; }) {
      if (src.arfcn) target.arfcn = src.arfcn;
    }
    if constexpr (requires { target.bsic; }) {
      if (src.bsic) target.bsic = src.bsic;
    }
    if constexpr (requires { target.ncc; }) {
      if (src.ncc) target.ncc = src.ncc;
    }
    if constexpr (requires { target.bcc; }) {
      if (src.bcc) target.bcc = src.bcc;
    }
    if constexpr (requires { target.rxlev_access_min; }) {
      if (src.rxlev_access_min) target.rxlev_access_min = src.rxlev_access_min;
    }
    if constexpr (requires { target.ms_txpwr_max_cch; }) {
      if (src.ms_txpwr_max_cch) target.ms_txpwr_max_cch = src.ms_txpwr_max_cch;
    }
    if constexpr (requires { target.ncc_permitted; }) {
      if (src.ncc_permitted) target.ncc_permitted = src.ncc_permitted;
    }
    if constexpr (requires { target.cell_reselect_offset; }) {
      if (src.cell_reselect_offset) target.cell_reselect_offset = src.cell_reselect_offset;
    }
    if constexpr (requires { target.temporary_offset; }) {
      if (src.temporary_offset) target.temporary_offset = src.temporary_offset;
    }
    if constexpr (requires { target.penalty_time; }) {
      if (src.penalty_time) target.penalty_time = src.penalty_time;
    }
    if constexpr (requires { target.reselect_params_present; }) {
      if (src.reselect_params_present) target.reselect_params_present = true;
    }
    if constexpr (requires { target.band_class; }) {
      if (src.band_class != 0xFF) target.band_class = src.band_class;
    }
    if constexpr (requires { target.dl_bw; }) {
      if (src.dl_bw) target.dl_bw = src.dl_bw;
    }
    if constexpr (requires { target.ul_bw; }) {
      if (src.ul_bw) target.ul_bw = src.ul_bw;
    }
    if constexpr (requires { target.ul_earfcn; }) {
      if (src.ul_earfcn) target.ul_earfcn = src.ul_earfcn;
    }
    if constexpr (requires { target.dl_uarfcn; }) {
      if (src.dl_uarfcn) target.dl_uarfcn = src.dl_uarfcn;
    }
    if constexpr (requires { target.ul_uarfcn; }) {
      if (src.ul_uarfcn) target.ul_uarfcn = src.ul_uarfcn;
    }
    if constexpr (requires { target.psc; }) {
      if (src.psc) target.psc = src.psc;
    }
    if constexpr (requires { target.freq_band_ind; }) {
      if (src.freq_band_ind) target.freq_band_ind = src.freq_band_ind;
    }
    if constexpr (requires { target.q_hyst; }) {
      if (src.q_hyst) target.q_hyst = src.q_hyst;
    }
    if constexpr (requires { target.t_resel_eutra; }) {
      if (src.t_resel_eutra) target.t_resel_eutra = src.t_resel_eutra;
    }
    if constexpr (requires { target.s_intra_search; }) {
      if (src.s_intra_search) target.s_intra_search = src.s_intra_search;
    }
    if constexpr (requires { target.s_non_intra_search; }) {
      if (src.s_non_intra_search) target.s_non_intra_search = src.s_non_intra_search;
    }
    if constexpr (requires { target.thresh_serving_low; }) {
      if (src.thresh_serving_low) target.thresh_serving_low = src.thresh_serving_low;
    }
    if constexpr (requires { target.sfn; }) {
      if (src.sfn) target.sfn = src.sfn;
    }
    if constexpr (requires { target.phich_duration; }) {
      if (src.phich_duration) target.phich_duration = src.phich_duration;
    }
    if constexpr (requires { target.phich_resource; }) {
      if (src.phich_resource) target.phich_resource = src.phich_resource;
    }
    if constexpr (requires { target.ac_barr_emergency; }) {
      target.ac_barr_emergency = src.ac_barr_emergency;
      target.ac_barr_mo_signaling = src.ac_barr_mo_signaling;
      target.ac_barr_mo_data = src.ac_barr_mo_data;
    }
    if constexpr (requires { target.p_max_present; }) {
      if (src.p_max_present) {
        target.p_max_present = true;
        target.p_max = src.p_max;
      }
    }
    if constexpr (requires { target.q_rx_lev_min; }) {
      if (src.q_rx_lev_min) target.q_rx_lev_min = src.q_rx_lev_min;
    }
    if constexpr (requires { target.q_qual_min; }) {
      if (src.q_qual_min) target.q_qual_min = src.q_qual_min;
    }
    if constexpr (requires { target.ranac; }) {
      if (src.ranac) target.ranac = src.ranac;
    }
    if constexpr (requires { target.dl_uarfcn; }) {
      if (src.dl_uarfcn) target.dl_uarfcn = src.dl_uarfcn;
    }
    if constexpr (requires { target.psc; }) {
      if (src.psc) target.psc = src.psc;
    }
  }
};

}  // namespace QCom
