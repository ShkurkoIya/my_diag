/// @file AtCmgrmi.h
/// @brief Parse SIMCOM AT+CMGRMI=4 (LTE) into tracker envelopes.
///
/// Sample (SIM8300):
///   +CMGRMI: Serving_Cell,375,250,01,17806,2,51830553,1,4,4,17,156,-112,-914,-599,0
///            fields: earfcn,mcc,mnc,tac,TA,cid,…,pci,rsrq10,rsrp10,rssi10
///   +CMGRMI: LTE_Intra_Cell1,156,-112,-914,-599,0
///   +CMGRMI: LTE_Inter,3,Freq1,38100,2,...
///   +CMGRMI: LTE_InterFreq1_Cell1,455,-120,-991,-781,0
///   +CMGRMI: CA_Scell,38100,455,38,5,0,455,-120,-991,-781,0,0,0
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <observer/model/Events.h>
#include <observer/model/Utils.h>

namespace QCom::AtCmgrmi {

struct LteServing {
  uint32_t earfcn{0};
  uint16_t pci{0};
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint32_t tac{0};
  uint32_t timing_advance{0};  ///< LTE TA index after TAC (0 = unset / not present)
  uint32_t cell_id{0};
  uint8_t dl_bw_mhz{0};
  uint8_t ul_bw_mhz{0};
  float rsrp{-999.0f};
  float rsrq{-999.0f};
  float rssi{-999.0f};
  bool ok{false};
};

struct LteNeighbor {
  uint32_t earfcn{0};
  uint16_t pci{0};
  float rsrp{-999.0f};
  float rsrq{-999.0f};
  float rssi{-999.0f};
  bool has_rsrp{false};
  bool has_rsrq{false};
  bool ca{false};
};

struct LteSnapshot {
  LteServing serving;
  std::vector<LteNeighbor> neighbors;
  std::vector<InterFreqCarrier> inter_carriers;
};

namespace detail {

[[nodiscard]] inline std::vector<std::string> split_csv(std::string_view line) {
  std::vector<std::string> toks;
  std::string cur;
  for (char c : line) {
    if (c == ',') {
      toks.push_back(cur);
      cur.clear();
    } else if (c != ' ' && c != '"' && c != '\r') {
      cur.push_back(c);
    }
  }
  toks.push_back(cur);
  return toks;
}

[[nodiscard]] inline uint8_t bw_index_to_mhz(int idx) noexcept {
  switch (idx) {
    case 0: return 1;
    case 1: return 3;
    case 2: return 5;
    case 3: return 10;
    case 4: return 15;
    case 5: return 20;
    default: return 0;
  }
}

[[nodiscard]] inline bool parse_u32(std::string_view s, uint32_t& out) {
  if (s.empty()) return false;
  try {
    out = static_cast<uint32_t>(std::stoul(std::string(s)));
    return true;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] inline bool parse_i32(std::string_view s, int32_t& out) {
  if (s.empty()) return false;
  try {
    out = static_cast<int32_t>(std::stol(std::string(s)));
    return true;
  } catch (...) {
    return false;
  }
}

/// CMGRMI stores RSRP/RSRQ/RSSI as tenths (e.g. -914 → -91.4 dBm).
[[nodiscard]] inline float tenths_to_float(int32_t v) noexcept {
  return static_cast<float>(v) / 10.0f;
}

/// Modem transitional sentinels: EARFCN=0xFFFFFFFF, MCC/MNC=0xFFFF, etc.
[[nodiscard]] inline bool sane_plmn(uint16_t mcc, uint16_t mnc) noexcept {
  return mcc >= 100 && mcc <= 999 && mnc < 1000;
}

[[nodiscard]] inline bool sane_neighbor(uint32_t earfcn, uint16_t pci) noexcept {
  return Utils::valid_lte_earfcn(earfcn) && pci > 0 && Utils::valid_lte_pci(pci);
}

inline void add_inter_pci(LteSnapshot& snap, uint32_t earfcn, uint16_t pci) {
  if (!sane_neighbor(earfcn, pci)) return;
  for (auto& c : snap.inter_carriers) {
    if (c.earfcn != earfcn) continue;
    if (std::find(c.neigh_pcis.begin(), c.neigh_pcis.end(), pci) == c.neigh_pcis.end())
      c.neigh_pcis.push_back(pci);
    return;
  }
  InterFreqCarrier c;
  c.earfcn = earfcn;
  c.neigh_pcis.push_back(pci);
  snap.inter_carriers.push_back(c);
}

inline void clamp_neigh_signal(LteNeighbor& n) noexcept {
  if (n.has_rsrp && !Utils::valid_lte_rsrp(n.rsrp)) n.has_rsrp = false;
  if (n.has_rsrq && !(n.rsrq >= -30.0f && n.rsrq <= -1.0f)) n.has_rsrq = false;
}

}  // namespace detail

/// Parse full AT+CMGRMI=4 response body.
[[nodiscard]] inline LteSnapshot parse_lte(std::string_view resp) {
  LteSnapshot snap;
  uint32_t serving_earfcn = 0;
  std::map<int, uint32_t> inter_earfcn_by_idx;

  for (std::size_t pos = 0; pos < resp.size();) {
    const auto at = resp.find("+CMGRMI:", pos);
    if (at == std::string_view::npos) break;
    pos = at + 8;
    std::string line(resp.substr(pos));
    if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);
    auto toks = detail::split_csv(line);
    if (toks.empty()) continue;
    const std::string& tag = toks[0];

    if (tag == "Serving_Cell" && toks.size() >= 14) {
      // earfcn,mcc,mnc,tac,TA,cid,?,dlbw,ulbw,?,pci,rsrq10,rsrp10,rssi10
      LteServing s;
      uint32_t tmp = 0;
      int32_t si = 0;
      if (!detail::parse_u32(toks[1], s.earfcn)) continue;
      if (!detail::parse_u32(toks[2], tmp)) continue;
      s.mcc = static_cast<uint16_t>(tmp);
      if (!detail::parse_u32(toks[3], tmp)) continue;
      s.mnc = static_cast<uint16_t>(tmp);
      if (!detail::parse_u32(toks[4], s.tac)) continue;
      // toks[5] = Starting UL Timing Advance (matches QXDM 0xB114 / QMI NAS TA).
      if (detail::parse_u32(toks[5], tmp) && tmp > 0 && tmp <= 1282) s.timing_advance = tmp;
      if (!detail::parse_u32(toks[6], s.cell_id)) continue;
      if (detail::parse_u32(toks[8], tmp))
        s.dl_bw_mhz = detail::bw_index_to_mhz(static_cast<int>(tmp));
      if (detail::parse_u32(toks[9], tmp))
        s.ul_bw_mhz = detail::bw_index_to_mhz(static_cast<int>(tmp));
      if (!detail::parse_u32(toks[11], tmp) || tmp == 0 || tmp > 503) continue;
      s.pci = static_cast<uint16_t>(tmp);
      if (detail::parse_i32(toks[12], si)) s.rsrq = detail::tenths_to_float(si);
      if (detail::parse_i32(toks[13], si)) s.rsrp = detail::tenths_to_float(si);
      if (toks.size() > 14 && detail::parse_i32(toks[14], si))
        s.rssi = detail::tenths_to_float(si);
      // Reject 0xFFFFFFFF / 0xFFFF transitional rows (GUI "65535" blobs).
      s.ok = Utils::valid_lte_earfcn(s.earfcn) && Utils::valid_lte_pci(s.pci) && s.pci != 0 &&
             detail::sane_plmn(s.mcc, s.mnc) && Utils::valid_lte_eci(s.cell_id) &&
             Utils::valid_lte_tac(s.tac);
      if (s.ok) {
        snap.serving = s;
        serving_earfcn = s.earfcn;
      }
      continue;
    }

    if (tag == "CA_Scell" && toks.size() >= 9) {
      LteNeighbor n;
      uint32_t tmp = 0;
      int32_t si = 0;
      if (!detail::parse_u32(toks[1], n.earfcn)) continue;
      if (!detail::parse_u32(toks[2], tmp) || tmp == 0 || tmp > 503) continue;
      n.pci = static_cast<uint16_t>(tmp);
      if (detail::parse_i32(toks[7], si)) {
        n.rsrq = detail::tenths_to_float(si);
        n.has_rsrq = true;
      }
      if (detail::parse_i32(toks[8], si)) {
        n.rsrp = detail::tenths_to_float(si);
        n.has_rsrp = true;
      }
      if (toks.size() > 9 && detail::parse_i32(toks[9], si)) n.rssi = detail::tenths_to_float(si);
      n.ca = true;
      detail::clamp_neigh_signal(n);
      if (detail::sane_neighbor(n.earfcn, n.pci)) {
        snap.neighbors.push_back(n);
        // CA PCI belongs on the parent carrier (SIB5-shaped), not stamped onto serving EARFCN.
        if (!(snap.serving.ok && n.earfcn == snap.serving.earfcn))
          detail::add_inter_pci(snap, n.earfcn, n.pci);
      }
      continue;
    }

    if (tag.rfind("LTE_Intra_Cell", 0) == 0 && toks.size() >= 4) {
      LteNeighbor n;
      n.earfcn = serving_earfcn;
      uint32_t tmp = 0;
      int32_t si = 0;
      if (!detail::parse_u32(toks[1], tmp) || tmp == 0 || tmp > 503) continue;
      n.pci = static_cast<uint16_t>(tmp);
      // Serving often repeats as Intra_Cell — skip duplicate of serving key.
      if (snap.serving.ok && n.earfcn == snap.serving.earfcn && n.pci == snap.serving.pci) continue;
      if (detail::parse_i32(toks[2], si)) {
        n.rsrq = detail::tenths_to_float(si);
        n.has_rsrq = true;
      }
      if (detail::parse_i32(toks[3], si)) {
        n.rsrp = detail::tenths_to_float(si);
        n.has_rsrp = true;
      }
      if (toks.size() > 4 && detail::parse_i32(toks[4], si)) n.rssi = detail::tenths_to_float(si);
      detail::clamp_neigh_signal(n);
      if (detail::sane_neighbor(n.earfcn, n.pci)) snap.neighbors.push_back(n);
      continue;
    }

    if (tag == "LTE_Inter" && toks.size() >= 4) {
      for (std::size_t i = 2; i + 1 < toks.size(); ++i) {
        if (toks[i].rfind("Freq", 0) != 0) continue;
        int idx = 0;
        try {
          idx = std::stoi(toks[i].substr(4));
        } catch (...) {
          continue;
        }
        uint32_t earfcn = 0;
        if (!detail::parse_u32(toks[i + 1], earfcn) || !Utils::valid_lte_earfcn(earfcn)) continue;
        inter_earfcn_by_idx[idx] = earfcn;
        bool found = false;
        for (const auto& existing : snap.inter_carriers) {
          if (existing.earfcn == earfcn) {
            found = true;
            break;
          }
        }
        if (!found) {
          InterFreqCarrier c;
          c.earfcn = earfcn;
          snap.inter_carriers.push_back(c);
        }
      }
      continue;
    }

    if (tag.rfind("LTE_InterFreq", 0) == 0 && tag.find("_Cell") != std::string::npos &&
        toks.size() >= 4) {
      int freq_idx = 0;
      {
        auto p = tag.find("Freq");
        auto c = tag.find("_Cell");
        if (p == std::string::npos || c == std::string::npos || c <= p + 4) continue;
        try {
          freq_idx = std::stoi(tag.substr(p + 4, c - (p + 4)));
        } catch (...) {
          continue;
        }
      }
      auto it = inter_earfcn_by_idx.find(freq_idx);
      if (it == inter_earfcn_by_idx.end()) continue;
      LteNeighbor n;
      n.earfcn = it->second;
      uint32_t tmp = 0;
      int32_t si = 0;
      if (!detail::parse_u32(toks[1], tmp) || tmp == 0 || tmp > 503) continue;
      n.pci = static_cast<uint16_t>(tmp);
      if (detail::parse_i32(toks[2], si)) {
        n.rsrq = detail::tenths_to_float(si);
        n.has_rsrq = true;
      }
      if (detail::parse_i32(toks[3], si)) {
        n.rsrp = detail::tenths_to_float(si);
        n.has_rsrp = true;
      }
      if (toks.size() > 4 && detail::parse_i32(toks[4], si)) n.rssi = detail::tenths_to_float(si);
      detail::clamp_neigh_signal(n);
      if (!detail::sane_neighbor(n.earfcn, n.pci)) continue;
      snap.neighbors.push_back(n);
      detail::add_inter_pci(snap, n.earfcn, n.pci);
      continue;
    }
  }

  {
    std::map<std::pair<uint32_t, uint16_t>, LteNeighbor> best;
    for (const auto& n : snap.neighbors) {
      if (!detail::sane_neighbor(n.earfcn, n.pci)) continue;
      auto key = std::pair{n.earfcn, n.pci};
      auto it = best.find(key);
      if (it == best.end()) {
        best.emplace(key, n);
      } else if (n.has_rsrp && (!it->second.has_rsrp || n.rsrp > it->second.rsrp)) {
        it->second = n;
      }
    }
    snap.neighbors.clear();
    for (auto& [_, n] : best) snap.neighbors.push_back(std::move(n));
  }
  return snap;
}

/// Build RRC envelopes for CellTracker injection.
/// Serving must pass sentinel checks; neighbors alone still mint RF RADIO rows.
[[nodiscard]] inline std::vector<Events::RrcEventEnvelope> to_envelopes(const LteSnapshot& snap) {
  std::vector<Events::RrcEventEnvelope> envs;
  const bool have_srv = snap.serving.ok;
  if (!have_srv && snap.neighbors.empty() && snap.inter_carriers.empty()) return envs;

  LocalCellKey skey{};
  if (have_srv) {
    skey = LocalCellKey{.freq = snap.serving.earfcn, .pci_bsic = snap.serving.pci};

    {
      Events::RadioParamsEvent<LteRadioParams> radio;
      radio.data.earfcn = snap.serving.earfcn;
      radio.data.pci = snap.serving.pci;
      radio.data.dl_bw = snap.serving.dl_bw_mhz;
      radio.data.ul_bw = snap.serving.ul_bw_mhz;
      if (snap.serving.timing_advance) radio.data.timing_advance = snap.serving.timing_advance;
      envs.push_back(Events::RrcEventEnvelope{
          .key = skey, .rat = RatType::LTE, .event_data = Events::RrcEvent{std::move(radio)}});
    }
    {
      CellPassport pass;
      pass.mcc = snap.serving.mcc;
      pass.mnc = snap.serving.mnc;
      pass.tac = snap.serving.tac;
      pass.cell_id = snap.serving.cell_id;
      envs.push_back(Events::RrcEventEnvelope{
          .key = skey, .rat = RatType::LTE, .event_data = Events::PassportEvent{.passport = pass}});
    }
    if (Utils::valid_lte_rsrp(snap.serving.rsrp)) {
      CellSignal sig;
      LteSignalParams lp;
      lp.rsrp = snap.serving.rsrp;
      if (snap.serving.rsrq > -30.0f && snap.serving.rsrq <= -1.0f) lp.rsrq = snap.serving.rsrq;
      if (snap.serving.rssi > -140.0f && snap.serving.rssi < 0.0f) {
        lp.rssi = snap.serving.rssi;
        lp.has_rssi = true;
      }
      sig.signal_data = lp;
      envs.push_back(Events::RrcEventEnvelope{
          .key = skey,
          .rat = RatType::LTE,
          .event_data = Events::SignalUpdateEvent{.signal = std::move(sig)}});
    }
    envs.push_back(Events::RrcEventEnvelope{
        .key = skey,
        .rat = RatType::LTE,
        .event_data = Events::ServingChangedEvent{.is_serving = true}});
  }

  std::vector<NeighborMeasResult> meas;
  std::vector<IntraFreqNeighbor> intra;
  for (const auto& n : snap.neighbors) {
    if (!detail::sane_neighbor(n.earfcn, n.pci)) continue;
    if (have_srv && n.earfcn == snap.serving.earfcn && n.pci != snap.serving.pci) {
      IntraFreqNeighbor in;
      in.pci = n.pci;
      intra.push_back(in);
      NeighborMeasResult m;
      m.pci = n.pci;
      if (n.has_rsrp) {
        m.rsrp_dbm = n.rsrp;
        m.has_rsrp = true;
      }
      if (n.has_rsrq) {
        m.rsrq_db = n.rsrq;
        m.has_rsrq = true;
      }
      meas.push_back(m);
    }
    if (have_srv && n.earfcn == snap.serving.earfcn && n.pci == snap.serving.pci) continue;
    LocalCellKey nkey{.freq = n.earfcn, .pci_bsic = n.pci};
    Events::RadioParamsEvent<LteRadioParams> radio;
    radio.data.earfcn = n.earfcn;
    radio.data.pci = n.pci;
    envs.push_back(Events::RrcEventEnvelope{
        .key = nkey, .rat = RatType::LTE, .event_data = Events::RrcEvent{std::move(radio)}});
    if (n.has_rsrp && Utils::valid_lte_rsrp(n.rsrp)) {
      CellSignal sig;
      LteSignalParams lp;
      lp.rsrp = n.rsrp;
      if (n.has_rsrq) lp.rsrq = n.rsrq;
      if (n.rssi > -140.0f && n.rssi < 0.0f) {
        lp.rssi = n.rssi;
        lp.has_rssi = true;
      }
      sig.signal_data = lp;
      envs.push_back(Events::RrcEventEnvelope{
          .key = nkey,
          .rat = RatType::LTE,
          .event_data = Events::SignalUpdateEvent{.signal = std::move(sig)}});
    }
  }
  if (have_srv && !intra.empty()) {
    envs.push_back(Events::RrcEventEnvelope{
        .key = skey,
        .rat = RatType::LTE,
        .event_data = Events::IntraNeighborsEvent{.neighbors = std::move(intra)}});
  }
  if (have_srv && !meas.empty()) {
    envs.push_back(Events::RrcEventEnvelope{
        .key = skey,
        .rat = RatType::LTE,
        .event_data = Events::NeighborMeasEvent{.neighbors = std::move(meas)}});
  }
  if (have_srv && !snap.inter_carriers.empty()) {
    envs.push_back(Events::RrcEventEnvelope{
        .key = skey,
        .rat = RatType::LTE,
        .event_data = Events::InterFreqCarriersEvent{.carriers = snap.inter_carriers}});
  } else if (!have_srv) {
    // Orphan inter-freq PCIs still mint as RADIO rows (no SIB5 attach without serving key).
    for (const auto& c : snap.inter_carriers) {
      if (!Utils::valid_lte_earfcn(c.earfcn)) continue;
      for (uint16_t pci : c.neigh_pcis) {
        if (!detail::sane_neighbor(c.earfcn, pci)) continue;
        LocalCellKey nkey{.freq = c.earfcn, .pci_bsic = pci};
        Events::RadioParamsEvent<LteRadioParams> radio;
        radio.data.earfcn = c.earfcn;
        radio.data.pci = pci;
        envs.push_back(Events::RrcEventEnvelope{
            .key = nkey, .rat = RatType::LTE, .event_data = Events::RrcEvent{std::move(radio)}});
      }
    }
  }
  return envs;
}

/// Intra + interfreq PCI keys from a CMGRMI=4 snapshot (serving itself excluded).
/// Hop grind whitelist — not SSS / tracker RF.
[[nodiscard]] inline std::set<std::pair<uint32_t, uint16_t>> neighbor_hop_keys(
    const LteSnapshot& snap) {
  std::set<std::pair<uint32_t, uint16_t>> keys;
  auto take = [&](uint32_t earfcn, uint16_t pci) {
    if (!detail::sane_neighbor(earfcn, pci)) return;
    if (snap.serving.ok && earfcn == snap.serving.earfcn && pci == snap.serving.pci) return;
    keys.emplace(earfcn, pci);
  };
  for (const auto& n : snap.neighbors) take(n.earfcn, n.pci);
  for (const auto& c : snap.inter_carriers) {
    for (uint16_t pci : c.neigh_pcis) take(c.earfcn, pci);
  }
  return keys;
}

}  // namespace QCom::AtCmgrmi
