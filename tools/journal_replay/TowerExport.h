/// @file TowerExport.h
/// @brief Nested tower dump for GUI (RAT-specific JSON keys + pretty console).
///
/// Per-RAT keys always present; missing values are empty strings.
/// Neighbors nest under each unique tower (not flattened).
#pragma once

#include <algorithm>
#include <cstdint>
#include <fmt/format.h>
#include <fstream>
#include <glaze/glaze.hpp>
#include <glaze/json/generic.hpp>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <observer/model/BandInfo.h>
#include <observer/model/CellIdentity.h>
#include <observer/model/Utils.h>
#include <observer/engine/SurveyProjection.h>
#include "lte/LteQcomLayouts.h"

namespace QCom::Tools {

namespace tower_detail {

inline std::string num_or_empty(bool ok, auto v) {
  if (!ok) return {};
  return fmt::format("{}", v);
}

inline std::string i_or_empty(auto v) {
  if (v == 0) return {};
  return fmt::format("{}", v);
}

inline void put_extra(std::map<std::string, std::string>& m, const char* k, std::string v) {
  if (!v.empty()) m[k] = std::move(v);
}

inline void put_bool(std::map<std::string, std::string>& m, const char* k, bool v) {
  m[k] = v ? "1" : "0";
}

inline void merge_extra_maps(std::map<std::string, std::string>& dest,
                             const std::map<std::string, std::string>& src) {
  for (const auto& [k, v] : src) {
    auto it = dest.find(k);
    if ((it == dest.end() || it->second.empty()) && !v.empty()) dest[k] = v;
  }
}

/// 3GPP MNC is 2 or 3 digits — never emit bare "1" for MTS (250-01).
inline std::string format_mnc(uint16_t mnc, uint8_t digits = 0) {
  const int width = (digits == 3 || mnc >= 100) ? 3 : 2;
  return fmt::format("{:0{}}", mnc, width);
}

inline std::string format_mcc_mnc(uint16_t mcc, uint16_t mnc, uint8_t digits = 0) {
  if (!mcc) return {};
  return fmt::format("{}-{}", mcc, format_mnc(mnc, digits));
}

/// Nested JSON object; all leaf values are strings (GUI contract qcom.towers.v5).
struct JsonObj {
  glz::generic v;

  JsonObj() { v = glz::generic::object_t{}; }

  void ensure_object() {
    // Brace-init `generic{object_t{}}` hits glaze's initializer_list ctor and
    // becomes a 1-element array; operator[] then throws bad_variant_access.
    if (!v.is_object()) v = glz::generic::object_t{};
  }

  void str(std::string_view k, std::string_view val) {
    if (k.empty()) return;
    ensure_object();
    v[std::string(k)] = std::string(val);
  }
  void obj(std::string_view k, JsonObj nested) {
    if (k.empty()) return;
    ensure_object();
    v[std::string(k)] = std::move(nested.v);
  }
  void arr(std::string_view k, std::vector<JsonObj> items) {
    if (k.empty()) return;
    ensure_object();
    glz::generic::array_t a;
    a.reserve(items.size());
    for (auto& i : items) a.push_back(std::move(i.v));
    v[std::string(k)] = std::move(a);
  }

  [[nodiscard]] std::string dump() const {
    std::string out;
    if (const auto ec = glz::write<glz::opts{.prettify = true}>(v, out); ec) return "{}";
    return out;
  }
};

inline void put_extras_into(JsonObj& o, const std::map<std::string, std::string>& extras) {
  for (const auto& [k, v] : extras) o.str(k, v);
}

}  // namespace tower_detail

/// Internal flat bag — mapped to RAT-specific JSON keys on encode.
struct TowerFields {
  std::string mcc;
  std::string mnc;
  std::string mcc_mnc;
  std::string lac_tac;
  std::string cid;
  std::string enb_rnc_id;
  std::string ncell_id;

  std::string arfcn;  // dl channel (earfcn / uarfcn / arfcn)
  std::string ul_arfcn;
  std::string pci;  // PCI / PSC
  std::string bsic;
  std::string ncc;
  std::string bcc;
  std::string band;
  std::string duplex;
  std::string dl_freq_mhz;
  std::string ul_freq_mhz;
  std::string bandwidth;
  std::string ul_bw;
  std::string dl_code;
  std::string ul_code;

  std::string rxl;
  std::string rsrq;
  std::string snr;
  std::string rssi;
  std::string rxqual;
  std::string c1;
  std::string c2;

  std::string serving;  // "0" / "1" — current serving only
  std::string camped;   // "0" / "1" — ever camped / locked this session (incl. serving)
  std::string seen;
  std::string first_seen;
  std::string last_seen;

  /// Passport / SIB / IRAT fields without a fixed RAT column.
  std::map<std::string, std::string> identity_extra;
  std::map<std::string, std::string> radio_extra;
  std::map<std::string, std::string> signal_extra;
};

inline void fill_passport_extras(TowerFields& f, const CellPassport& p) {
  using namespace tower_detail;
  put_bool(f.identity_extra, "cell_barred", p.cell_barred);
  put_bool(f.identity_extra, "csg_ind", p.csg_ind);
  put_bool(f.identity_extra, "intra_freq_reselection_allowed", p.intra_freq_reselection_allowed);
  put_bool(f.identity_extra, "cell_reserved_for_operator", p.cell_reserved_for_operator);
  if (p.plmn_soft) put_bool(f.identity_extra, "plmn_soft", true);
  put_extra(f.identity_extra, "q_rx_lev_min", i_or_empty(p.q_rx_lev_min));
  put_extra(f.identity_extra, "q_rx_lev_min_offset", i_or_empty(p.q_rx_lev_min_offset));
  put_extra(f.identity_extra, "csg_id", i_or_empty(p.csg_id));
  put_extra(f.identity_extra, "freq_band_ind", i_or_empty(p.freq_band_ind));
  put_extra(f.identity_extra, "mnc_digits", i_or_empty(p.mnc_digits));
  put_extra(f.identity_extra, "rac", i_or_empty(p.rac));
}

inline TowerFields fields_from_cell(const CellIdentity& c) {
  using namespace tower_detail;
  TowerFields f;
  f.serving = c.is_serving ? "1" : "0";
  f.camped = (c.is_serving || c.ever_serving) ? "1" : "0";
  if (c.seen) f.seen = std::to_string(c.seen);
  f.first_seen = c.first_seen;
  f.last_seen = c.last_seen;
  f.mcc = i_or_empty(c.passport.mcc);
  f.mnc = c.passport.mcc ? format_mnc(c.passport.mnc, c.passport.mnc_digits) : "";
  if (c.passport.mcc) f.mcc_mnc = format_mcc_mnc(c.passport.mcc, c.passport.mnc, c.passport.mnc_digits);
  f.lac_tac = i_or_empty(c.passport.tac);
  f.cid = i_or_empty(c.passport.cell_id);
  fill_passport_extras(f, c.passport);

  if (auto* lte = c.radio_as_if<LteRadioParams>()) {
    f.arfcn = i_or_empty(lte->earfcn);
    f.ul_arfcn = i_or_empty(lte->ul_earfcn ? lte->ul_earfcn : 0);
    if (f.ul_arfcn.empty() && lte->earfcn) f.ul_arfcn = std::to_string(lte->earfcn);
    f.pci = i_or_empty(lte->pci);
    f.dl_code = f.pci;
    f.ul_code = f.pci;
    f.bandwidth = i_or_empty(lte->dl_bw);
    f.ul_bw = i_or_empty(lte->ul_bw);
    auto bi = BandInfo::lte_from_earfcn(lte->earfcn, lte->freq_band_ind);
    if (bi.name && bi.name[0])
      f.band = bi.name;
    else if (bi.band)
      f.band = "B" + std::to_string(bi.band);
    else if (lte->freq_band_ind)
      f.band = "B" + std::to_string(lte->freq_band_ind);
    auto dup = bi.duplex != BandInfo::Duplex::Unknown ? bi.duplex
                                                      : BandInfo::lte_duplex(lte->freq_band_ind);
    f.duplex = BandInfo::to_string(dup);
    if (bi.dl_mhz > 0) f.dl_freq_mhz = num_or_empty(true, bi.dl_mhz);
    if (bi.ul_mhz > 0) f.ul_freq_mhz = num_or_empty(true, bi.ul_mhz);
    if (c.passport.cell_id) {
      f.enb_rnc_id = std::to_string(c.passport.enb_id());
      f.ncell_id = std::to_string(c.passport.local_cell_id());
    }
    put_extra(f.radio_extra, "freq_band_ind", i_or_empty(lte->freq_band_ind));
    put_extra(f.radio_extra, "phich_duration", i_or_empty(lte->phich_duration));
    put_extra(f.radio_extra, "phich_resource",
              lte->phich_resource || lte->phich_duration ? std::to_string(lte->phich_resource) : "");
    put_extra(f.radio_extra, "sfn", i_or_empty(lte->sfn));
    if (lte->has_sfn_sf) {
      put_extra(f.radio_extra, "subframe", std::to_string(lte->subframe));
      put_extra(f.radio_extra, "serving_cell_index", std::to_string(lte->serving_cell_index));
      put_bool(f.radio_extra, "is_restricted", lte->is_restricted);
    }
    if (lte->valid_rx) put_extra(f.radio_extra, "valid_rx", std::to_string(lte->valid_rx));
    if (lte->p_max_present) put_extra(f.radio_extra, "p_max", std::to_string(lte->p_max));
    put_bool(f.radio_extra, "ac_barr_emergency", lte->ac_barr_emergency);
    put_bool(f.radio_extra, "ac_barr_mo_signaling", lte->ac_barr_mo_signaling);
    put_bool(f.radio_extra, "ac_barr_mo_data", lte->ac_barr_mo_data);
    put_extra(f.radio_extra, "q_hyst", i_or_empty(lte->q_hyst));
    put_extra(f.radio_extra, "t_resel_eutra", i_or_empty(lte->t_resel_eutra));
    put_extra(f.radio_extra, "s_intra_search", i_or_empty(lte->s_intra_search));
    put_extra(f.radio_extra, "s_non_intra_search", i_or_empty(lte->s_non_intra_search));
    put_extra(f.radio_extra, "thresh_serving_low", i_or_empty(lte->thresh_serving_low));
    put_extra(f.radio_extra, "cell_resel_prio", i_or_empty(lte->cell_resel_prio));
    put_extra(f.radio_extra, "thresh_x_high", i_or_empty(lte->thresh_x_high));
    put_extra(f.radio_extra, "thresh_x_low", i_or_empty(lte->thresh_x_low));
    put_extra(f.radio_extra, "timing_advance", i_or_empty(lte->timing_advance));
    if (lte->timing_advance) {
      // One-way estimate: TA_index × 78.125 m (LTE Ts×16 round-trip / 2).
      const int meters = static_cast<int>(lte->timing_advance * 78.125 + 0.5);
      put_extra(f.radio_extra, "ta_distance_m", std::to_string(meters));
    }
    if (lte->nas_idle >= 0) put_bool(f.radio_extra, "nas_idle", lte->nas_idle != 0);
    if (lte->allowed_access >= 0) put_bool(f.radio_extra, "allowed_access", lte->allowed_access != 0);
    if (lte->emm_state >= 0) {
      put_extra(f.radio_extra, "emm_state", std::to_string(lte->emm_state));
      put_extra(f.radio_extra, "emm_substate", std::to_string(lte->emm_substate));
      if (lte->emm_mcc) {
        put_extra(f.radio_extra, "emm_mcc", std::to_string(lte->emm_mcc));
        put_extra(f.radio_extra, "emm_mnc", std::to_string(lte->emm_mnc));
        if (lte->emm_mnc_digits)
          put_extra(f.radio_extra, "emm_mnc_digits", std::to_string(lte->emm_mnc_digits));
      }
      if (lte->mme_present) {
        put_extra(f.radio_extra, "mme_group_id", std::to_string(lte->mme_group_id));
        put_extra(f.radio_extra, "mme_code", std::to_string(lte->mme_code));
      }
    }
    if (lte->rrc_state >= 0) {
      put_extra(f.radio_extra, "rrc_state", std::to_string(lte->rrc_state));
      put_extra(f.radio_extra, "rrc_state_name",
                std::string(Lte::Wire::Evt1606::name(static_cast<uint8_t>(lte->rrc_state))));
    }
  } else if (auto* w = c.radio_as_if<WcdmaRadioParams>()) {
    f.arfcn = i_or_empty(w->dl_uarfcn);
    f.ul_arfcn = i_or_empty(w->ul_uarfcn);
    // PSC 0 is valid — do not drop via i_or_empty.
    f.pci = w->dl_uarfcn ? std::to_string(w->psc) : "";
    f.dl_code = f.pci;
    f.ul_code = f.pci;
    auto bi = BandInfo::umts_from_uarfcn(w->dl_uarfcn);
    if (bi.name && bi.name[0]) f.band = bi.name;
    f.duplex = BandInfo::to_string(bi.duplex);
    if (bi.dl_mhz > 0) f.dl_freq_mhz = num_or_empty(true, bi.dl_mhz);
    if (bi.ul_mhz > 0) f.ul_freq_mhz = num_or_empty(true, bi.ul_mhz);
    if (c.passport.cell_id) {
      f.enb_rnc_id = std::to_string(c.passport.rnc_id());
      f.ncell_id = std::to_string(c.passport.umts_cid16());
    }
    put_extra(f.radio_extra, "ura_id", i_or_empty(w->ura_id));
    put_extra(f.radio_extra, "access", i_or_empty(w->access));
    put_extra(f.radio_extra, "flags", i_or_empty(w->flags));
    put_extra(f.radio_extra, "q_rx_lev_min_rscp", i_or_empty(w->q_rx_lev_min_rscp));
    put_extra(f.radio_extra, "q_qual_min_ecno", i_or_empty(w->q_qual_min_ecno));
    if (w->last_rrc_channel != 0xFF) {
      put_extra(f.radio_extra, "rrc_channel", std::to_string(w->last_rrc_channel));
      put_extra(f.radio_extra, "rrc_len", i_or_empty(w->last_rrc_len));
    }
    if (bi.band) put_extra(f.radio_extra, "umts_band", std::to_string(bi.band));
  } else if (auto* g = c.radio_as_if<GsmRadioParams>()) {
    f.arfcn = i_or_empty(g->arfcn);
    f.bsic = i_or_empty(g->bsic);
    f.ncc = (g->bsic || g->ncc || g->bcc) ? std::to_string(g->ncc) : "";
    f.bcc = (g->bsic || g->ncc || g->bcc) ? std::to_string(g->bcc) : "";
    f.dl_code = f.bsic;
    f.ul_code = f.bsic;
    auto bi = BandInfo::gsm_from_arfcn(static_cast<uint16_t>(g->arfcn),
                                       g->band_class == 0xFF ? -1 : g->band_class);
    if (bi.name && bi.name[0]) f.band = bi.name;
    if (g->band_class != 0xFF) put_extra(f.radio_extra, "band_class", std::to_string(g->band_class));
    put_extra(f.radio_extra, "rxlev_access_min", i_or_empty(g->rxlev_access_min));
    put_extra(f.radio_extra, "ms_txpwr_max_cch", i_or_empty(g->ms_txpwr_max_cch));
    if (g->ncc_permitted != 0xFF)
      put_extra(f.radio_extra, "ncc_permitted", std::to_string(g->ncc_permitted));
    put_bool(f.radio_extra, "reselect_params_present", g->reselect_params_present);
    if (g->reselect_params_present) {
      put_extra(f.radio_extra, "cell_reselect_offset", std::to_string(g->cell_reselect_offset));
      put_extra(f.radio_extra, "temporary_offset", std::to_string(g->temporary_offset));
      put_extra(f.radio_extra, "penalty_time", std::to_string(g->penalty_time));
    }
    put_extra(f.radio_extra, "timing_advance", i_or_empty(g->timing_advance));
  } else if (auto* nr = c.radio_as_if<NrRadioParams>()) {
    f.arfcn = i_or_empty(nr->nrarfcn);
    f.ul_arfcn = i_or_empty(nr->ul_nrarfcn);
    f.pci = i_or_empty(nr->pci);
    f.dl_code = f.pci;
    f.ul_code = f.pci;
    f.bandwidth = i_or_empty(nr->dl_bw);
    f.ul_bw = i_or_empty(nr->ul_bw);
    if (nr->band) f.band = "n" + std::to_string(nr->band);
    put_extra(f.radio_extra, "q_rx_lev_min", i_or_empty(nr->q_rx_lev_min));
    put_extra(f.radio_extra, "q_qual_min", i_or_empty(nr->q_qual_min));
    put_extra(f.radio_extra, "q_hyst", i_or_empty(nr->q_hyst));
    put_extra(f.radio_extra, "ranac", i_or_empty(nr->ranac));
    put_extra(f.radio_extra, "sfn", i_or_empty(nr->sfn));
    put_extra(f.radio_extra, "scs_khz", i_or_empty(nr->scs_khz));
    put_extra(f.radio_extra, "band", i_or_empty(nr->band));
    if (nr->allowed_access >= 0) put_bool(f.radio_extra, "allowed_access", nr->allowed_access != 0);
  }

  if (c.signal.signal_data.index() != 0) {
    float lvl = c.signal.main_level();
    if (lvl != 0.0f) f.rxl = num_or_empty(true, lvl);
  }
  if (auto* ls = c.signal_as_if<LteSignalParams>()) {
    if (Utils::valid_lte_rsrp(ls->rsrp)) f.rxl = num_or_empty(true, ls->rsrp);
    if (ls->rsrq != 0.0f) f.rsrq = num_or_empty(true, ls->rsrq);
    if (ls->has_sinr) f.snr = num_or_empty(true, ls->sinr);
    if (ls->has_rssi) f.rssi = num_or_empty(true, ls->rssi);
    if (ls->has_rsrp_filt) put_extra(f.signal_extra, "rsrp_filt", num_or_empty(true, ls->rsrp_filt));
    if (ls->has_rsrq_filt) put_extra(f.signal_extra, "rsrq_filt", num_or_empty(true, ls->rsrq_filt));
    if (ls->has_sinr_per_rx) {
      put_extra(f.signal_extra, "sinr_rx0", num_or_empty(true, ls->sinr_rx0));
      put_extra(f.signal_extra, "sinr_rx1", num_or_empty(true, ls->sinr_rx1));
    }
  } else if (auto* ws = c.signal_as_if<WcdmaSignalParams>()) {
    if (ws->has_ecio) {
      f.rsrq = num_or_empty(true, ws->ecio);
      f.snr = num_or_empty(true, ws->ecio);
    }
  } else if (auto* gs = c.signal_as_if<GsmSignalParams>()) {
    if (gs->rxqual) f.rxqual = std::to_string(gs->rxqual);
    if (gs->has_snr) f.snr = std::to_string(gs->snr);
    if (gs->has_c1c2) {
      f.c1 = std::to_string(gs->c1);
      f.c2 = std::to_string(gs->c2);
    }
  } else if (auto* ns = c.signal_as_if<NrSignalParams>()) {
    if (ns->ss_rsrq != 0.0f) f.rsrq = num_or_empty(true, ns->ss_rsrq);
    if (ns->ss_sinr != 0.0f) f.snr = num_or_empty(true, ns->ss_sinr);
    put_extra(f.signal_extra, "ss_rsrp", num_or_empty(ns->ss_rsrp != 0.0f, ns->ss_rsrp));
    put_extra(f.signal_extra, "ss_rsrq", num_or_empty(ns->ss_rsrq != 0.0f, ns->ss_rsrq));
    put_extra(f.signal_extra, "ss_sinr", num_or_empty(ns->ss_sinr != 0.0f, ns->ss_sinr));
  }

  return f;
}

inline LocalCellKey cell_key_of(const CellIdentity& c) {
  return {.freq = c.radio.freq(), .pci_bsic = c.radio.pci_bsic()};
}

inline std::string rat_key_str(RatType rat, LocalCellKey k) {
  std::ostringstream oss;
  oss << to_string(rat) << '|' << k.freq << '|' << k.pci_bsic;
  return oss.str();
}

struct NeighborRef {
  RatType rat{RatType::UNKNOWN};
  TowerFields fields;
  bool resolved{false};
};

inline std::string neighbor_key(const NeighborRef& n) {
  uint32_t freq = 0;
  uint16_t code = 0;
  if (!n.fields.arfcn.empty()) freq = static_cast<uint32_t>(std::stoul(n.fields.arfcn));
  if (!n.fields.pci.empty())
    code = static_cast<uint16_t>(std::stoul(n.fields.pci));
  else if (!n.fields.bsic.empty())
    code = static_cast<uint16_t>(std::stoul(n.fields.bsic));
  return rat_key_str(n.rat, {.freq = freq, .pci_bsic = code});
}

inline void fill_empty_field(std::string& dest, const std::string& src) {
  if (dest.empty() && !src.empty()) dest = src;
}

inline void apply_meas_signal(TowerFields& f, const NeighborMeasResult& n) {
  using namespace tower_detail;
  if (n.has_rsrp) f.rxl = num_or_empty(true, n.rsrp_dbm);
  if (n.has_rsrq) f.rsrq = num_or_empty(true, n.rsrq_db);
  if (n.has_sinr) f.snr = num_or_empty(true, n.sinr_db);
  if (n.has_rssi) f.rssi = num_or_empty(true, n.rssi_dbm);
  if (n.has_rsrp_filt) put_extra(f.signal_extra, "rsrp_filt", num_or_empty(true, n.rsrp_filt));
  if (n.has_rsrq_filt) put_extra(f.signal_extra, "rsrq_filt", num_or_empty(true, n.rsrq_filt));
  if (n.has_sinr_per_rx) {
    put_extra(f.signal_extra, "sinr_rx0", num_or_empty(true, n.sinr_rx0));
    put_extra(f.signal_extra, "sinr_rx1", num_or_empty(true, n.sinr_rx1));
  }
  if (n.has_cgi) {
    fill_empty_field(f.mcc, i_or_empty(n.cgi.mcc));
    if (n.cgi.mcc) {
      if (f.mnc.empty()) f.mnc = format_mnc(n.cgi.mnc);
      if (f.mcc_mnc.empty()) f.mcc_mnc = format_mcc_mnc(n.cgi.mcc, n.cgi.mnc);
    }
    fill_empty_field(f.lac_tac, i_or_empty(n.cgi.tac));
    fill_empty_field(f.cid, i_or_empty(n.cgi.cell_id));
    if (n.cgi.cell_id && f.enb_rnc_id.empty()) {
      f.enb_rnc_id = std::to_string(n.cgi.enb_id());
      f.ncell_id = std::to_string(n.cgi.local_cell_id());
    }
    fill_passport_extras(f, n.cgi);
  }
}

inline NeighborRef neighbor_from_lte_meas(uint32_t earfcn, const NeighborMeasResult& n,
                                         const std::map<std::string, const CellIdentity*>& idx) {
  NeighborRef r;
  r.rat = RatType::LTE;
  std::string key = "LTE|" + std::to_string(earfcn) + "|" + std::to_string(n.pci);
  if (auto it = idx.find(key); it != idx.end()) {
    r.fields = fields_from_cell(*it->second);
    r.resolved = true;
    apply_meas_signal(r.fields, n);
  } else {
    r.fields.arfcn = earfcn ? std::to_string(earfcn) : "";
    r.fields.pci = std::to_string(n.pci);
    r.fields.dl_code = r.fields.pci;
    r.fields.ul_code = r.fields.pci;
    apply_meas_signal(r.fields, n);
    r.fields.serving = "0";
    r.fields.camped = "0";
  }
  return r;
}

inline NeighborRef neighbor_from_nr_meas(uint32_t nrarfcn, const NeighborMeasResult& n,
                                         const std::map<std::string, const CellIdentity*>& idx) {
  NeighborRef r;
  r.rat = RatType::NR;
  std::string key = "NR|" + std::to_string(nrarfcn) + "|" + std::to_string(n.pci);
  if (auto it = idx.find(key); it != idx.end()) {
    r.fields = fields_from_cell(*it->second);
    r.resolved = true;
    apply_meas_signal(r.fields, n);
  } else {
    r.fields.arfcn = nrarfcn ? std::to_string(nrarfcn) : "";
    r.fields.pci = std::to_string(n.pci);
    r.fields.dl_code = r.fields.pci;
    r.fields.ul_code = r.fields.pci;
    apply_meas_signal(r.fields, n);
    r.fields.serving = "0";
    r.fields.camped = "0";
  }
  return r;
}

inline NeighborRef neighbor_from_gsm(const GsmNeighborCell& n,
                                     const std::map<std::string, const CellIdentity*>& idx) {
  NeighborRef r;
  r.rat = RatType::GSM;
  uint16_t bsic = n.bsic_valid ? n.bsic : 0;
  std::string key = "GSM|" + std::to_string(n.arfcn) + "|" + std::to_string(bsic);
  if (auto it = idx.find(key); it != idx.end()) {
    r.fields = fields_from_cell(*it->second);
    r.resolved = true;
  } else {
    r.fields.arfcn = std::to_string(n.arfcn);
    if (n.bsic_valid) {
      r.fields.bsic = std::to_string(n.bsic);
      r.fields.ncc = std::to_string((n.bsic >> 3) & 7);
      r.fields.bcc = std::to_string(n.bsic & 7);
      r.fields.dl_code = r.fields.bsic;
    }
    if (n.rxlev) r.fields.rxl = std::to_string(n.rxlev);
    r.fields.serving = "0";
    r.fields.camped = "0";
  }
  return r;
}

inline NeighborRef neighbor_from_wcdma(const WcdmaNeighborCell& n,
                                       const std::map<std::string, const CellIdentity*>& idx) {
  NeighborRef r;
  r.rat = RatType::WCDMA;
  std::string key = "WCDMA|" + std::to_string(n.uarfcn) + "|" + std::to_string(n.psc);
  if (auto it = idx.find(key); it != idx.end()) {
    r.fields = fields_from_cell(*it->second);
    r.resolved = true;
  } else {
    r.fields.arfcn = std::to_string(n.uarfcn);
    r.fields.pci = std::to_string(n.psc);
    r.fields.dl_code = r.fields.pci;
    if (n.rscp) r.fields.rxl = std::to_string(n.rscp);
    if (n.ecio) {
      r.fields.snr = std::to_string(n.ecio);
      r.fields.rsrq = r.fields.snr;
    }
    r.fields.serving = "0";
    r.fields.camped = "0";
  }
  return r;
}

inline NeighborRef neighbor_from_intra(uint32_t earfcn, const IntraFreqNeighbor& n,
                                       const std::map<std::string, const CellIdentity*>& idx) {
  using namespace tower_detail;
  NeighborMeasResult m;
  m.pci = n.pci;
  NeighborRef r = neighbor_from_lte_meas(earfcn, m, idx);
  put_extra(r.fields.radio_extra, "q_offset", i_or_empty(n.q_offset));
  return r;
}

inline NeighborRef neighbor_stub_geran(const GeranNeighborFreq& n) {
  using namespace tower_detail;
  NeighborRef r;
  r.rat = RatType::GSM;
  r.fields.arfcn = std::to_string(n.arfcn_start);
  r.fields.serving = "0";
  r.fields.camped = "0";
  if (n.ncc_permitted != 0xFF)
    put_extra(r.fields.radio_extra, "ncc_permitted", std::to_string(n.ncc_permitted));
  put_extra(r.fields.radio_extra, "q_rx_lev_min", i_or_empty(n.q_rx_lev_min));
  put_extra(r.fields.radio_extra, "thresh_x_high", i_or_empty(n.thresh_x_high));
  put_extra(r.fields.radio_extra, "thresh_x_low", i_or_empty(n.thresh_x_low));
  put_extra(r.fields.radio_extra, "sib_kind", "geran_carrier");
  return r;
}

inline NeighborRef neighbor_stub_utra(const UtraNeighborFreq& n) {
  using namespace tower_detail;
  NeighborRef r;
  r.rat = RatType::WCDMA;
  r.fields.arfcn = std::to_string(n.uarfcn);
  r.fields.serving = "0";
  r.fields.camped = "0";
  put_extra(r.fields.radio_extra, "q_rx_lev_min", i_or_empty(n.q_rx_lev_min));
  put_extra(r.fields.radio_extra, "p_max_utra", i_or_empty(n.p_max_utra));
  put_extra(r.fields.radio_extra, "q_qual_min", i_or_empty(n.q_qual_min));
  put_extra(r.fields.radio_extra, "thresh_x_high", i_or_empty(n.thresh_x_high));
  put_extra(r.fields.radio_extra, "thresh_x_low", i_or_empty(n.thresh_x_low));
  put_extra(r.fields.radio_extra, "sib_kind", "utra_carrier");
  return r;
}

inline void fill_inter_carrier_extras(TowerFields& f, const InterFreqCarrier& n) {
  using namespace tower_detail;
  put_extra(f.radio_extra, "q_rx_lev_min", i_or_empty(n.q_rx_lev_min));
  put_extra(f.radio_extra, "thresh_x_high", i_or_empty(n.thresh_x_high));
  put_extra(f.radio_extra, "thresh_x_low", i_or_empty(n.thresh_x_low));
  put_extra(f.radio_extra, "cell_resel_prio", i_or_empty(n.cell_resel_prio));
  put_extra(f.radio_extra, "allowed_meas_bw", i_or_empty(n.allowed_meas_bw));
  put_extra(f.radio_extra, "sib_kind", "inter_freq_carrier");
}

inline NeighborRef neighbor_stub_inter(const InterFreqCarrier& n) {
  NeighborRef r;
  r.rat = RatType::LTE;
  r.fields.arfcn = std::to_string(n.earfcn);
  r.fields.serving = "0";
  r.fields.camped = "0";
  fill_inter_carrier_extras(r.fields, n);
  return r;
}

inline NeighborRef neighbor_from_inter_pci(const InterFreqCarrier& car, uint16_t pci,
                                           const std::map<std::string, const CellIdentity*>& idx) {
  using namespace tower_detail;
  NeighborMeasResult m;
  m.pci = pci;
  NeighborRef r = neighbor_from_lte_meas(car.earfcn, m, idx);
  fill_inter_carrier_extras(r.fields, car);
  put_extra(r.fields.radio_extra, "sib_kind", "inter_freq_neigh");
  return r;
}

struct TowerNode {
  RatType rat{RatType::UNKNOWN};
  std::string key;
  TowerFields fields;
  std::vector<NeighborRef> nb_lte;
  std::vector<NeighborRef> nb_gsm;
  std::vector<NeighborRef> nb_umts;
  std::vector<NeighborRef> nb_nr;
};

inline void push_nb_unique(std::vector<NeighborRef>& dst, std::set<std::string>& seen,
                           const std::string& self_key, NeighborRef&& n) {
  const std::string k = neighbor_key(n);
  if (k == self_key) return;
  if (!seen.insert(k).second) return;
  dst.push_back(std::move(n));
}

inline std::vector<TowerNode> build_towers(const std::vector<CellIdentity>& cells) {
  std::map<std::string, const CellIdentity*> idx;
  for (const auto& c : cells) idx[rat_key_str(c.rat, cell_key_of(c))] = &c;

  std::vector<TowerNode> out;
  out.reserve(cells.size());
  for (const auto& c : cells) {
    TowerNode t;
    t.rat = c.rat;
    t.key = rat_key_str(c.rat, cell_key_of(c));
    t.fields = fields_from_cell(c);

    std::set<std::string> seen_lte, seen_gsm, seen_umts, seen_nr;
    const uint32_t freq = c.radio.freq();

    if (c.rat == RatType::NR) {
      for (const auto& n : c.radio.meas_neighbors)
        push_nb_unique(t.nb_nr, seen_nr, t.key, neighbor_from_nr_meas(freq, n, idx));
    } else {
      for (const auto& n : c.radio.meas_neighbors)
        push_nb_unique(t.nb_lte, seen_lte, t.key, neighbor_from_lte_meas(freq, n, idx));
    }
    for (const auto& n : c.radio.intra_freq_neighbors)
      push_nb_unique(t.nb_lte, seen_lte, t.key, neighbor_from_intra(freq, n, idx));
    for (const auto& n : c.radio.inter_freq_carriers) {
      push_nb_unique(t.nb_lte, seen_lte, t.key, neighbor_stub_inter(n));
      for (uint16_t pci : n.neigh_pcis)
        push_nb_unique(t.nb_lte, seen_lte, t.key, neighbor_from_inter_pci(n, pci, idx));
    }
    for (const auto& n : c.radio.geran_neighbors)
      push_nb_unique(t.nb_gsm, seen_gsm, t.key, neighbor_stub_geran(n));
    for (const auto& n : c.radio.utra_neighbors)
      push_nb_unique(t.nb_umts, seen_umts, t.key, neighbor_stub_utra(n));
    for (const auto& n : c.radio.gsm_neighbors)
      push_nb_unique(t.nb_gsm, seen_gsm, t.key, neighbor_from_gsm(n, idx));
    for (const auto& n : c.radio.wcdma_neighbors)
      push_nb_unique(t.nb_umts, seen_umts, t.key, neighbor_from_wcdma(n, idx));

    out.push_back(std::move(t));
  }
  return out;
}

/// Encode identity/radio/signal with RAT-specific keys (SDR + Vlad + ours).
inline void encode_rat_sections(tower_detail::JsonObj& o, RatType rat, const TowerFields& f) {
  using namespace tower_detail;
  JsonObj id;
  JsonObj radio;
  JsonObj sig;
  JsonObj meta;

  meta.str("serving", f.serving);
  meta.str("camped", f.camped.empty() ? "0" : f.camped);
  meta.str("seen", f.seen);
  meta.str("first_seen", f.first_seen);
  meta.str("last_seen", f.last_seen);

  if (rat == RatType::LTE) {
    id.str("mcc", f.mcc);
    id.str("mnc", f.mnc);
    id.str("mcc_mnc", f.mcc_mnc);
    id.str("tac", f.lac_tac);
    id.str("cid", f.cid);
    id.str("enb_id", f.enb_rnc_id);
    id.str("ncell_id", f.ncell_id);

    radio.str("earfcn", f.arfcn);
    radio.str("ul_earfcn", f.ul_arfcn);
    radio.str("pci", f.pci);
    radio.str("dl_code", f.dl_code);
    radio.str("ul_code", f.ul_code);
    radio.str("band", f.band);
    radio.str("duplex_type", f.duplex);
    radio.str("dl_freq", f.dl_freq_mhz);
    radio.str("ul_freq", f.ul_freq_mhz);
    radio.str("bandwidth", f.bandwidth);
    radio.str("ul_bw", f.ul_bw);

    sig.str("rxl", f.rxl);
    sig.str("rsrq", f.rsrq);
    sig.str("snr", f.snr);
    sig.str("rssi", f.rssi);
    put_extras_into(id, f.identity_extra);
    put_extras_into(radio, f.radio_extra);
    put_extras_into(sig, f.signal_extra);
  } else if (rat == RatType::WCDMA) {
    id.str("mcc", f.mcc);
    id.str("mnc", f.mnc);
    id.str("mcc_mnc", f.mcc_mnc);
    id.str("lac", f.lac_tac);
    id.str("cid", f.cid);
    id.str("rnc_id", f.enb_rnc_id);
    id.str("cid16", f.ncell_id);
    if (auto it = f.identity_extra.find("rac"); it != f.identity_extra.end())
      id.str("rac", it->second);

    radio.str("uarfcn", f.arfcn);
    radio.str("ul_uarfcn", f.ul_arfcn);
    radio.str("psc", f.pci);
    radio.str("dl_code", f.dl_code);
    radio.str("ul_code", f.ul_code);
    radio.str("band", f.band);
    radio.str("duplex_type", f.duplex);
    radio.str("dl_freq", f.dl_freq_mhz);
    radio.str("ul_freq", f.ul_freq_mhz);

    sig.str("rxl", f.rxl);
    sig.str("snr", f.snr);
    sig.str("ecio", f.rsrq);
    put_extras_into(id, f.identity_extra);
    put_extras_into(radio, f.radio_extra);
    put_extras_into(sig, f.signal_extra);
  } else if (rat == RatType::GSM) {
    id.str("mcc", f.mcc);
    id.str("mnc", f.mnc);
    id.str("mcc_mnc", f.mcc_mnc);
    id.str("lac", f.lac_tac);
    id.str("cid", f.cid);

    radio.str("arfcn", f.arfcn);
    radio.str("bsic", f.bsic);
    radio.str("ncc", f.ncc);
    radio.str("bcc", f.bcc);
    radio.str("band", f.band);
    radio.str("dl_code", f.dl_code);

    sig.str("rxl", f.rxl);
    sig.str("snr", f.snr);
    sig.str("rxqual", f.rxqual);
    sig.str("c1", f.c1);
    sig.str("c2", f.c2);
    put_extras_into(id, f.identity_extra);
    put_extras_into(radio, f.radio_extra);
    put_extras_into(sig, f.signal_extra);
  } else {
    id.str("mcc", f.mcc);
    id.str("mnc", f.mnc);
    id.str("mcc_mnc", f.mcc_mnc);
    id.str("tac", f.lac_tac);
    id.str("cid", f.cid);
    id.str("ncell_id", f.ncell_id);
    radio.str("nrarfcn", f.arfcn);
    radio.str("pci", f.pci);
    radio.str("dl_code", f.dl_code);
    radio.str("band", f.band);
    radio.str("duplex_type", f.duplex);
    radio.str("bandwidth", f.bandwidth);
    radio.str("ul_bw", f.ul_bw);
    sig.str("rxl", f.rxl);
    sig.str("rsrq", f.rsrq);
    sig.str("snr", f.snr);
    put_extras_into(id, f.identity_extra);
    put_extras_into(radio, f.radio_extra);
    put_extras_into(sig, f.signal_extra);
  }

  o.obj("meta", std::move(meta));
  o.obj("identity", std::move(id));
  o.obj("radio", std::move(radio));
  o.obj("signal", std::move(sig));
}

inline tower_detail::JsonObj encode_neighbor(const NeighborRef& n) {
  using namespace tower_detail;
  JsonObj o;
  o.str("rat", to_string(n.rat));
  o.str("resolved", n.resolved ? "1" : "0");
  encode_rat_sections(o, n.rat, n.fields);
  return o;
}

inline std::vector<tower_detail::JsonObj> encode_nb_list(const std::vector<NeighborRef>& xs) {
  std::vector<tower_detail::JsonObj> items;
  items.reserve(xs.size());
  for (const auto& n : xs) items.push_back(encode_neighbor(n));
  return items;
}

inline tower_detail::JsonObj encode_neighbors_for_rat(RatType parent, const std::vector<NeighborRef>& lte,
                                                      const std::vector<NeighborRef>& gsm,
                                                      const std::vector<NeighborRef>& umts,
                                                      const std::vector<NeighborRef>& nr) {
  using namespace tower_detail;
  JsonObj o;
  if (parent == RatType::LTE) {
    o.arr("nb_lte", encode_nb_list(lte));
    o.arr("nb_gsm", encode_nb_list(gsm));
    o.arr("nb_umts", encode_nb_list(umts));
    o.arr("nb_nr", encode_nb_list(nr));
  } else if (parent == RatType::WCDMA) {
    o.arr("nb_gsm", encode_nb_list(gsm));
    o.arr("nb_umts", encode_nb_list(umts));
  } else if (parent == RatType::GSM) {
    o.arr("nb_gsm", encode_nb_list(gsm));
  } else {
    o.arr("nb_lte", encode_nb_list(lte));
    o.arr("nb_gsm", encode_nb_list(gsm));
    o.arr("nb_umts", encode_nb_list(umts));
    o.arr("nb_nr", encode_nb_list(nr));
  }
  return o;
}

inline tower_detail::JsonObj encode_tower(const TowerNode& t) {
  using namespace tower_detail;
  JsonObj o;
  o.str("key", t.key);
  encode_rat_sections(o, t.rat, t.fields);
  o.obj("neighbors", encode_neighbors_for_rat(t.rat, t.nb_lte, t.nb_gsm, t.nb_umts, t.nb_nr));
  return o;
}

inline bool tower_newer(const TowerNode& a, const TowerNode& b) {
  if (a.fields.last_seen != b.fields.last_seen) return a.fields.last_seen > b.fields.last_seen;
  const bool a_rf = !a.fields.arfcn.empty();
  const bool b_rf = !b.fields.arfcn.empty();
  if (a_rf != b_rf) return a_rf;
  const bool a_code = !a.fields.pci.empty() || !a.fields.bsic.empty();
  const bool b_code = !b.fields.pci.empty() || !b.fields.bsic.empty();
  if (a_code != b_code) return a_code;
  return a.fields.seen > b.fields.seen;
}

inline void sort_towers_by_last_seen(std::vector<TowerNode>& xs) {
  std::sort(xs.begin(), xs.end(),
            [](const TowerNode& a, const TowerNode& b) { return tower_newer(a, b); });
}

/// Fully-formed tower: passport (MCC + LAC/TAC + CID) + RF channel;
/// LTE/UMTS/NR also require PCI/PSC; GSM prefers BSIC but ARFCN+CID is enough.
inline bool is_complete_tower(const TowerNode& t) {
  const auto& f = t.fields;
  if (f.mcc.empty() || f.cid.empty() || f.lac_tac.empty()) return false;
  if (f.arfcn.empty()) return false;
  if (t.rat == RatType::GSM) return true;
  if (t.rat == RatType::LTE || t.rat == RatType::WCDMA || t.rat == RatType::NR)
    return !f.pci.empty();
  return false;
}

/// OR serving/camped; copy missing signal/radio/identity enrichments from `src` into `dest`.
inline void merge_tower_fields(TowerFields& dest, const TowerFields& src) {
  using namespace tower_detail;
  if (src.serving == "1") dest.serving = "1";
  if (src.camped == "1") dest.camped = "1";
  fill_empty_field(dest.mnc, src.mnc);
  fill_empty_field(dest.mcc_mnc, src.mcc_mnc);
  fill_empty_field(dest.enb_rnc_id, src.enb_rnc_id);
  fill_empty_field(dest.ncell_id, src.ncell_id);
  fill_empty_field(dest.ul_arfcn, src.ul_arfcn);
  fill_empty_field(dest.bsic, src.bsic);
  fill_empty_field(dest.ncc, src.ncc);
  fill_empty_field(dest.bcc, src.bcc);
  fill_empty_field(dest.band, src.band);
  fill_empty_field(dest.duplex, src.duplex);
  fill_empty_field(dest.dl_freq_mhz, src.dl_freq_mhz);
  fill_empty_field(dest.ul_freq_mhz, src.ul_freq_mhz);
  fill_empty_field(dest.bandwidth, src.bandwidth);
  fill_empty_field(dest.ul_bw, src.ul_bw);
  fill_empty_field(dest.dl_code, src.dl_code);
  fill_empty_field(dest.ul_code, src.ul_code);
  fill_empty_field(dest.rxl, src.rxl);
  fill_empty_field(dest.rsrq, src.rsrq);
  fill_empty_field(dest.snr, src.snr);
  fill_empty_field(dest.rssi, src.rssi);
  fill_empty_field(dest.rxqual, src.rxqual);
  fill_empty_field(dest.c1, src.c1);
  fill_empty_field(dest.c2, src.c2);
  fill_empty_field(dest.first_seen, src.first_seen);
  merge_extra_maps(dest.identity_extra, src.identity_extra);
  merge_extra_maps(dest.radio_extra, src.radio_extra);
  merge_extra_maps(dest.signal_extra, src.signal_extra);
  if (dest.seen.empty() || (!src.seen.empty() && src.seen > dest.seen)) {
    // Prefer a non-empty seen count; if both set, keep the larger lexical/numeric string.
    if (!src.seen.empty()) {
      if (dest.seen.empty())
        dest.seen = src.seen;
      else {
        try {
          if (std::stoull(src.seen) > std::stoull(dest.seen)) dest.seen = src.seen;
        } catch (...) {
        }
      }
    }
  }
}

inline void merge_neighbor_lists(TowerNode& dest, const TowerNode& src) {
  auto merge_vec = [](std::vector<NeighborRef>& d, const std::vector<NeighborRef>& s) {
    std::set<std::string> seen;
    for (const auto& n : d) seen.insert(neighbor_key(n));
    for (const auto& n : s) {
      if (seen.insert(neighbor_key(n)).second) d.push_back(n);
    }
  };
  merge_vec(dest.nb_lte, src.nb_lte);
  merge_vec(dest.nb_gsm, src.nb_gsm);
  merge_vec(dest.nb_umts, src.nb_umts);
  merge_vec(dest.nb_nr, src.nb_nr);
}

inline bool tower_has_neighbors(const TowerNode& t) {
  return !t.nb_lte.empty() || !t.nb_gsm.empty() || !t.nb_umts.empty() || !t.nb_nr.empty();
}

inline std::string tower_rf_key(const TowerNode& t) {
  const std::string& code = !t.fields.pci.empty() ? t.fields.pci : t.fields.bsic;
  return std::string(to_string(t.rat)) + '|' + t.fields.arfcn + '|' + code;
}

/// Keep only complete towers; one row per (RAT, CID).
/// Latest last_seen wins as the base row; OR serving and fill empty signal/radio
/// fields from duplicate CID rows and from RF siblings (same rat|arfcn|pci/bsic)
/// that may be incomplete but carry ML1 signal.
inline std::vector<TowerNode> unique_complete_towers(const std::vector<TowerNode>& in) {
  std::map<std::string, std::vector<const TowerNode*>> by_rf;
  for (const auto& t : in) {
    if (t.fields.arfcn.empty()) continue;
    by_rf[tower_rf_key(t)].push_back(&t);
  }

  std::map<std::string, TowerNode> by_id;
  for (const auto& t : in) {
    if (!is_complete_tower(t)) continue;
    const std::string id = std::string(to_string(t.rat)) + "|cid|" + t.fields.cid;
    auto it = by_id.find(id);
    if (it == by_id.end()) {
      by_id.emplace(id, t);
    } else if (tower_newer(t, it->second)) {
      TowerNode newer = t;
      merge_tower_fields(newer.fields, it->second.fields);
      merge_neighbor_lists(newer, it->second);
      it->second = std::move(newer);
    } else {
      merge_tower_fields(it->second.fields, t.fields);
      merge_neighbor_lists(it->second, t);
    }
  }

  for (auto& [_, t] : by_id) {
    auto rit = by_rf.find(tower_rf_key(t));
    if (rit == by_rf.end()) continue;
    for (const TowerNode* sib : rit->second) {
      merge_tower_fields(t.fields, sib->fields);
      merge_neighbor_lists(t, *sib);
    }
  }

  std::vector<TowerNode> out;
  out.reserve(by_id.size());
  for (auto& [_, t] : by_id) out.push_back(std::move(t));
  sort_towers_by_last_seen(out);
  return out;
}

struct TowersByRat {
  std::vector<TowerNode> gsm;
  std::vector<TowerNode> lte;
  std::vector<TowerNode> wcdma;
  std::vector<TowerNode> nr;
};

inline TowersByRat group_towers_by_rat(const std::vector<TowerNode>& towers) {
  TowersByRat g;
  for (const auto& t : towers) {
    if (t.rat == RatType::GSM)
      g.gsm.push_back(t);
    else if (t.rat == RatType::LTE)
      g.lte.push_back(t);
    else if (t.rat == RatType::WCDMA)
      g.wcdma.push_back(t);
    else if (t.rat == RatType::NR)
      g.nr.push_back(t);
  }
  sort_towers_by_last_seen(g.gsm);
  sort_towers_by_last_seen(g.lte);
  sort_towers_by_last_seen(g.wcdma);
  sort_towers_by_last_seen(g.nr);
  return g;
}

inline std::vector<tower_detail::JsonObj> encode_tower_list(const std::vector<TowerNode>& xs) {
  std::vector<tower_detail::JsonObj> items;
  items.reserve(xs.size());
  for (const auto& t : xs) items.push_back(encode_tower(t));
  return items;
}

inline std::string encode_document(const std::vector<TowerNode>& towers, std::string_view source) {
  using namespace tower_detail;
  auto unique = unique_complete_towers(towers);
  auto by_rat = group_towers_by_rat(unique);

  size_t serving = 0, with_nb = 0;
  std::string situation_as_of;
  for (const auto& t : unique) {
    if (t.fields.serving == "1") ++serving;
    if (tower_has_neighbors(t)) ++with_nb;
    if (situation_as_of.empty() || t.fields.last_seen > situation_as_of)
      situation_as_of = t.fields.last_seen;
  }

  JsonObj meta;
  meta.str("source", std::string(source));
  meta.str("situation_as_of", situation_as_of);
  meta.str("tower_count", std::to_string(unique.size()));
  meta.str("raw_tower_count", std::to_string(towers.size()));
  meta.str("filter", "complete_passport_rf_unique_cid_latest");
  meta.str("serving_count", std::to_string(serving));
  meta.str("towers_with_neighbors", std::to_string(with_nb));
  meta.str("gsm", std::to_string(by_rat.gsm.size()));
  meta.str("lte", std::to_string(by_rat.lte.size()));
  meta.str("wcdma", std::to_string(by_rat.wcdma.size()));
  meta.str("nr", std::to_string(by_rat.nr.size()));
  meta.str("schema", "qcom.towers.v5");

  JsonObj towers_obj;
  towers_obj.arr("gsm", encode_tower_list(by_rat.gsm));
  towers_obj.arr("lte", encode_tower_list(by_rat.lte));
  towers_obj.arr("wcdma", encode_tower_list(by_rat.wcdma));
  towers_obj.arr("nr", encode_tower_list(by_rat.nr));

  JsonObj root;
  root.obj("meta", std::move(meta));
  root.obj("towers", std::move(towers_obj));
  return root.dump() + '\n';
}

inline bool write_towers_json(const std::string& path, const std::vector<CellIdentity>& cells,
                              std::string_view source) {
  auto towers = build_towers(cells);
  std::ofstream out(path);
  if (!out) return false;
  out << encode_document(towers, source);
  return true;
}

/// All RF keys (EARFCN|PCI etc.), including PLMN/RADIO incompletes — for live GUI tree.
inline std::vector<TowerNode> unique_survey_towers(const std::vector<TowerNode>& in) {
  std::map<std::string, TowerNode> by_rf;
  for (const auto& t : in) {
    if (t.fields.arfcn.empty()) continue;
    if (t.rat != RatType::GSM && t.fields.pci.empty()) continue;
    const std::string k = tower_rf_key(t);
    auto it = by_rf.find(k);
    if (it == by_rf.end()) {
      by_rf.emplace(k, t);
    } else if (tower_newer(t, it->second)) {
      TowerNode newer = t;
      merge_tower_fields(newer.fields, it->second.fields);
      merge_neighbor_lists(newer, it->second);
      it->second = std::move(newer);
    } else {
      merge_tower_fields(it->second.fields, t.fields);
      merge_neighbor_lists(it->second, t);
    }
  }
  std::vector<TowerNode> out;
  out.reserve(by_rf.size());
  for (auto& [_, t] : by_rf) out.push_back(std::move(t));
  sort_towers_by_last_seen(out);
  return out;
}

inline std::string encode_document_survey(const std::vector<TowerNode>& towers,
                                          std::string_view source,
                                          const std::map<std::string, std::string>& extra_meta = {}) {
  using namespace tower_detail;
  auto survey = unique_survey_towers(towers);
  auto by_rat = group_towers_by_rat(survey);

  size_t serving = 0, with_nb = 0, complete = 0;
  std::string situation_as_of;
  for (const auto& t : survey) {
    if (t.fields.serving == "1") ++serving;
    if (tower_has_neighbors(t)) ++with_nb;
    if (is_complete_tower(t)) ++complete;
    if (situation_as_of.empty() || t.fields.last_seen > situation_as_of)
      situation_as_of = t.fields.last_seen;
  }

  JsonObj meta;
  meta.str("source", std::string(source));
  meta.str("situation_as_of", situation_as_of);
  meta.str("tower_count", std::to_string(survey.size()));
  meta.str("raw_tower_count", std::to_string(towers.size()));
  meta.str("complete_count", std::to_string(complete));
  meta.str("filter", "live_survey_rf_all");
  meta.str("serving_count", std::to_string(serving));
  meta.str("towers_with_neighbors", std::to_string(with_nb));
  meta.str("gsm", std::to_string(by_rat.gsm.size()));
  meta.str("lte", std::to_string(by_rat.lte.size()));
  meta.str("wcdma", std::to_string(by_rat.wcdma.size()));
  meta.str("nr", std::to_string(by_rat.nr.size()));
  meta.str("schema", "qcom.towers.v5");
  meta.str("origin", "live_scanner");
  for (const auto& [k, v] : extra_meta) meta.str(k, v);

  JsonObj towers_obj;
  towers_obj.arr("gsm", encode_tower_list(by_rat.gsm));
  towers_obj.arr("lte", encode_tower_list(by_rat.lte));
  towers_obj.arr("wcdma", encode_tower_list(by_rat.wcdma));
  towers_obj.arr("nr", encode_tower_list(by_rat.nr));

  JsonObj root;
  root.obj("meta", std::move(meta));
  root.obj("towers", std::move(towers_obj));
  return root.dump() + '\n';
}

/// Atomic survey write (tmp + rename) for live GUI polling.
inline bool write_towers_json_survey(const std::string& path, const std::vector<CellIdentity>& cells,
                                     std::string_view source,
                                     const std::map<std::string, std::string>& extra_meta = {}) {
  auto towers = build_towers(cells);
  auto extra = extra_meta;
  {
    const auto r = QCom::Engine::project_lte(cells);
    extra.try_emplace("rf_unique", std::to_string(r.stats.lte_rf_unique));
    extra.try_emplace("full_passport", std::to_string(r.stats.lte_full));
    extra.try_emplace("lte_sites", std::to_string(r.stats.lte_sites));
    extra.try_emplace("lte_serving", std::to_string(r.stats.lte_serving));
  }
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp);
    if (!out) return false;
    out << encode_document_survey(towers, source, extra);
    if (!out.flush()) return false;
  }
  return std::rename(tmp.c_str(), path.c_str()) == 0;
}

/// Human-readable nested console dump — complete unique towers by last_seen, grouped by RAT.
inline void print_towers_pretty(std::ostream& os, const std::vector<CellIdentity>& cells,
                                std::string_view source, size_t max_towers = 0) {
  auto raw = build_towers(cells);
  auto unique = unique_complete_towers(raw);
  auto by_rat = group_towers_by_rat(unique);

  std::string as_of;
  size_t with_nb = 0, serving = 0;
  for (const auto& t : unique) {
    if (t.fields.serving == "1") ++serving;
    if (tower_has_neighbors(t)) ++with_nb;
    if (as_of.empty() || t.fields.last_seen > as_of) as_of = t.fields.last_seen;
  }

  os << "\n=== situation (complete unique towers, latest by CID, qcom.towers.v5) ===\n";
  os << "source: " << source << '\n';
  os << "situation_as_of: " << (as_of.empty() ? "-" : as_of) << '\n';
  os << "complete_unique: " << unique.size() << "  raw: " << raw.size()
     << "  serving: " << serving << "  with_neighbors: " << with_nb << '\n';
  os << "by_rat: gsm=" << by_rat.gsm.size() << " lte=" << by_rat.lte.size()
     << " wcdma=" << by_rat.wcdma.size() << " nr=" << by_rat.nr.size() << '\n';
  os << "filter: passport(MCC+LAC/TAC+CID) + RF(channel[+pci/psc]) ; unique by RAT|CID ; latest last_seen\n";

  auto line = [&](int ind, std::string_view k, std::string_view v) {
    os << std::string(static_cast<size_t>(ind), ' ') << k << ": ";
    if (v.empty())
      os << "-\n";
    else
      os << v << '\n';
  };

  auto dump_fields = [&](int ind, RatType rat, const TowerFields& f) {
    line(ind, "serving", f.serving);
    line(ind, "seen", f.seen);
    line(ind, "first_seen", f.first_seen);
    line(ind, "last_seen", f.last_seen);
    os << std::string(static_cast<size_t>(ind), ' ') << "identity:\n";
    line(ind + 2, "mcc", f.mcc);
    line(ind + 2, "mnc", f.mnc);
    line(ind + 2, "mcc_mnc", f.mcc_mnc);
    if (rat == RatType::LTE) {
      line(ind + 2, "tac", f.lac_tac);
      line(ind + 2, "cid", f.cid);
      line(ind + 2, "enb_id", f.enb_rnc_id);
      line(ind + 2, "ncell_id", f.ncell_id);
    } else if (rat == RatType::WCDMA) {
      line(ind + 2, "lac", f.lac_tac);
      line(ind + 2, "cid", f.cid);
      line(ind + 2, "rnc_id", f.enb_rnc_id);
      line(ind + 2, "cid16", f.ncell_id);
    } else {
      line(ind + 2, "lac", f.lac_tac);
      line(ind + 2, "cid", f.cid);
    }
    os << std::string(static_cast<size_t>(ind), ' ') << "radio:\n";
    if (rat == RatType::LTE) {
      line(ind + 2, "earfcn", f.arfcn);
      line(ind + 2, "ul_earfcn", f.ul_arfcn);
      line(ind + 2, "pci", f.pci);
      line(ind + 2, "dl_code", f.dl_code);
      line(ind + 2, "ul_code", f.ul_code);
      line(ind + 2, "band", f.band);
      line(ind + 2, "duplex_type", f.duplex);
      line(ind + 2, "dl_freq", f.dl_freq_mhz);
      line(ind + 2, "ul_freq", f.ul_freq_mhz);
      line(ind + 2, "bandwidth", f.bandwidth);
      line(ind + 2, "ul_bw", f.ul_bw);
    } else if (rat == RatType::WCDMA) {
      line(ind + 2, "uarfcn", f.arfcn);
      line(ind + 2, "ul_uarfcn", f.ul_arfcn);
      line(ind + 2, "psc", f.pci);
      line(ind + 2, "dl_code", f.dl_code);
      line(ind + 2, "ul_code", f.ul_code);
      line(ind + 2, "band", f.band);
      line(ind + 2, "duplex_type", f.duplex);
      line(ind + 2, "dl_freq", f.dl_freq_mhz);
      line(ind + 2, "ul_freq", f.ul_freq_mhz);
    } else if (rat == RatType::GSM) {
      line(ind + 2, "arfcn", f.arfcn);
      line(ind + 2, "bsic", f.bsic);
      line(ind + 2, "ncc", f.ncc);
      line(ind + 2, "bcc", f.bcc);
      line(ind + 2, "band", f.band);
      line(ind + 2, "dl_code", f.dl_code);
    } else {
      line(ind + 2, "nrarfcn", f.arfcn);
      line(ind + 2, "pci", f.pci);
      line(ind + 2, "band", f.band);
      line(ind + 2, "bandwidth", f.bandwidth);
      line(ind + 2, "ul_bw", f.ul_bw);
    }
    for (const auto& [k, v] : f.radio_extra) line(ind + 2, k, v);
    for (const auto& [k, v] : f.identity_extra) line(ind + 2, k, v);
    os << std::string(static_cast<size_t>(ind), ' ') << "signal:\n";
    if (rat == RatType::LTE) {
      line(ind + 2, "rxl", f.rxl);
      line(ind + 2, "rsrq", f.rsrq);
      line(ind + 2, "snr", f.snr);
      line(ind + 2, "rssi", f.rssi);
    } else if (rat == RatType::WCDMA) {
      line(ind + 2, "rxl", f.rxl);
      line(ind + 2, "snr", f.snr);
      line(ind + 2, "ecio", f.rsrq);
    } else {
      line(ind + 2, "rxl", f.rxl);
      line(ind + 2, "snr", f.snr);
      line(ind + 2, "rxqual", f.rxqual);
      line(ind + 2, "c1", f.c1);
      line(ind + 2, "c2", f.c2);
    }
    for (const auto& [k, v] : f.signal_extra) line(ind + 2, k, v);
  };

  auto dump_nb_list = [&](int ind, std::string_view title, const std::vector<NeighborRef>& xs) {
    os << std::string(static_cast<size_t>(ind), ' ') << title << ": [" << xs.size() << "]\n";
    for (size_t i = 0; i < xs.size(); ++i) {
      os << std::string(static_cast<size_t>(ind + 2), ' ') << "- [" << i << "] rat="
         << to_string(xs[i].rat) << " resolved=" << (xs[i].resolved ? "1" : "0") << '\n';
      dump_fields(ind + 4, xs[i].rat, xs[i].fields);
    }
  };

  auto dump_group = [&](std::string_view name, RatType rat, const std::vector<TowerNode>& xs) {
    os << "\n--- " << name << " (" << xs.size() << ", by last_seen) ---\n";
    size_t shown = 0;
    for (const auto& t : xs) {
      if (max_towers && shown >= max_towers) {
        os << "... (" << (xs.size() - shown) << " more; use --towers-json for full dump)\n";
        break;
      }
      os << "\n- tower: " << t.key << '\n';
      dump_fields(2, rat, t.fields);
      os << "  neighbors:\n";
      if (rat == RatType::LTE) {
        dump_nb_list(4, "nb_lte", t.nb_lte);
        dump_nb_list(4, "nb_gsm", t.nb_gsm);
        dump_nb_list(4, "nb_umts", t.nb_umts);
        dump_nb_list(4, "nb_nr", t.nb_nr);
      } else if (rat == RatType::WCDMA) {
        dump_nb_list(4, "nb_gsm", t.nb_gsm);
        dump_nb_list(4, "nb_umts", t.nb_umts);
      } else if (rat == RatType::GSM) {
        dump_nb_list(4, "nb_gsm", t.nb_gsm);
      } else {
        dump_nb_list(4, "nb_lte", t.nb_lte);
        dump_nb_list(4, "nb_gsm", t.nb_gsm);
        dump_nb_list(4, "nb_umts", t.nb_umts);
        dump_nb_list(4, "nb_nr", t.nb_nr);
      }
      ++shown;
    }
  };

  dump_group("gsm", RatType::GSM, by_rat.gsm);
  dump_group("lte", RatType::LTE, by_rat.lte);
  dump_group("wcdma", RatType::WCDMA, by_rat.wcdma);
  dump_group("nr", RatType::NR, by_rat.nr);
}

}  // namespace QCom::Tools
