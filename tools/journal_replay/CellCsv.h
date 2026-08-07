/// @file CellCsv.h
/// @brief Vlad-compatible cells CSV read/write for offline journal_replay.
#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/BandInfo.h"
#include "core/CellIdentity.h"

namespace QCom::Tools {

struct CellRow {
  std::string rat;
  uint32_t arfcn{0};
  std::string band;
  uint16_t pci{0};   // PCI / PSC / unused for GSM
  uint16_t bsic{0};  // GSM only
  uint8_t ncc{0};
  uint8_t bcc{0};
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint32_t lac{0};
  uint64_t cid{0};
  float signal_dbm{0};
  bool has_signal{false};
  float rsrq{0};
  bool has_rsrq{false};
  float snr{0};
  bool has_snr{false};
  int16_t c1{0};
  int16_t c2{0};
  bool has_c1c2{false};
  bool serving{false};

  // SDR-aligned extras
  std::string duplex;
  double dl_freq_mhz{0};
  double ul_freq_mhz{0};
  uint8_t bandwidth_mhz{0};  // DL BW
  uint8_t ul_bw_mhz{0};
  uint32_t ul_arfcn{0};  // ul_earfcn / ul_uarfcn
  uint32_t enb_or_rnc{0};
  uint32_t ncell_id{0};
  uint32_t nb_lte{0};
  uint32_t nb_gsm{0};
  uint32_t nb_umts{0};
  std::string nb_lte_detail;
  std::string nb_gsm_detail;
  std::string nb_umts_detail;

  [[nodiscard]] std::string key() const {
    // Match Vlad merge key: RAT + ARFCN + PCI/BSIC
    std::ostringstream oss;
    oss << rat << '|' << arfcn << '|';
    if (rat == "GSM")
      oss << bsic;
    else
      oss << pci;
    return oss.str();
  }
};

inline std::string csv_escape(std::string_view s) {
  if (s.find_first_of(",\"\n\r") == std::string_view::npos) return std::string(s);
  std::string out = "\"";
  for (char c : s) {
    if (c == '"') out += "\"\"";
    else out += c;
  }
  out += '"';
  return out;
}

inline std::string format_nb_lte(const CellIdentity& c) {
  std::ostringstream oss;
  bool first = true;
  auto emit = [&](uint16_t pci, float rsrp, bool has) {
    if (!first) oss << ';';
    first = false;
    oss << pci;
    if (has) oss << ':' << static_cast<int>(rsrp);
  };
  for (const auto& n : c.radio.meas_neighbors) emit(n.pci, n.rsrp_dbm, n.has_rsrp);
  for (const auto& n : c.radio.intra_freq_neighbors) emit(n.pci, 0, false);
  return oss.str();
}

inline std::string format_nb_gsm(const CellIdentity& c) {
  std::ostringstream oss;
  bool first = true;
  for (const auto& n : c.radio.gsm_neighbors) {
    if (!first) oss << ';';
    first = false;
    oss << n.arfcn;
    if (n.bsic_valid) oss << '/' << static_cast<unsigned>(n.bsic);
    if (n.rxlev) oss << ':' << n.rxlev;
  }
  for (const auto& n : c.radio.geran_neighbors) {
    if (!first) oss << ';';
    first = false;
    oss << n.arfcn_start;
  }
  return oss.str();
}

inline std::string format_nb_umts(const CellIdentity& c) {
  std::ostringstream oss;
  bool first = true;
  for (const auto& n : c.radio.wcdma_neighbors) {
    if (!first) oss << ';';
    first = false;
    oss << n.uarfcn << '/' << n.psc;
    if (n.rscp) oss << ':' << n.rscp;
  }
  for (const auto& n : c.radio.utra_neighbors) {
    if (!first) oss << ';';
    first = false;
    oss << n.uarfcn;
  }
  return oss.str();
}

inline CellRow from_identity(const CellIdentity& c) {
  CellRow r;
  r.rat = to_string(c.rat);
  r.serving = c.is_serving;
  r.mcc = c.passport.mcc;
  r.mnc = c.passport.mnc;
  r.lac = c.passport.tac;
  r.cid = c.passport.cell_id;

  if (auto* lte = c.radio_as_if<LteRadioParams>()) {
    r.arfcn = lte->earfcn;
    r.pci = lte->pci;
    r.bandwidth_mhz = lte->dl_bw;
    r.ul_bw_mhz = lte->ul_bw;
    r.ul_arfcn = lte->ul_earfcn ? lte->ul_earfcn : lte->earfcn;
    auto bi = BandInfo::lte_from_earfcn(lte->earfcn, lte->freq_band_ind);
    if (!bi.band && lte->freq_band_ind) {
      bi.band = lte->freq_band_ind;
      bi.duplex = BandInfo::lte_duplex(lte->freq_band_ind);
      bi.name = "";
    }
    if (bi.name && bi.name[0]) r.band = bi.name;
    else if (bi.band) r.band = "B" + std::to_string(bi.band);
    r.duplex = BandInfo::to_string(bi.duplex != BandInfo::Duplex::Unknown ? bi.duplex
                                          : BandInfo::lte_duplex(lte->freq_band_ind));
    r.dl_freq_mhz = bi.dl_mhz;
    r.ul_freq_mhz = bi.ul_mhz;
    if (r.cid) {
      r.enb_or_rnc = c.passport.enb_id();
      r.ncell_id = c.passport.local_cell_id();
    }
  } else if (auto* nr = c.radio_as_if<NrRadioParams>()) {
    r.arfcn = nr->nrarfcn;
    r.pci = nr->pci;
    r.bandwidth_mhz = nr->dl_bw;
    r.ul_bw_mhz = nr->ul_bw;
  } else if (auto* w = c.radio_as_if<WcdmaRadioParams>()) {
    r.arfcn = w->dl_uarfcn;
    r.pci = w->psc;
    r.ul_arfcn = w->ul_uarfcn;
    auto bi = BandInfo::umts_from_uarfcn(w->dl_uarfcn);
    if (bi.name && bi.name[0]) r.band = bi.name;
    r.duplex = BandInfo::to_string(bi.duplex);
    r.dl_freq_mhz = bi.dl_mhz;
    r.ul_freq_mhz = bi.ul_mhz;
    if (r.cid) {
      r.enb_or_rnc = c.passport.rnc_id();
      r.ncell_id = c.passport.umts_cid16();
    }
  } else if (auto* g = c.radio_as_if<GsmRadioParams>()) {
    r.arfcn = g->arfcn;
    r.bsic = g->bsic;
    r.ncc = g->ncc;
    r.bcc = g->bcc;
    auto bi = BandInfo::gsm_from_arfcn(static_cast<uint16_t>(g->arfcn),
                                       g->band_class == 0xFF ? -1 : g->band_class);
    if (bi.name && bi.name[0]) r.band = bi.name;
  } else {
    r.arfcn = c.radio.freq();
    r.pci = c.radio.pci_bsic();
  }

  float lvl = c.signal.main_level();
  if (lvl != 0.0f || c.signal.signal_data.index() != 0) {
    r.signal_dbm = lvl;
    r.has_signal = c.signal.signal_data.index() != 0;
  }
  if (auto* ls = c.signal_as_if<LteSignalParams>()) {
    r.rsrq = ls->rsrq;
    r.has_rsrq = true;
    if (ls->has_sinr) {
      r.snr = ls->sinr;
      r.has_snr = true;
    }
  } else if (auto* ns = c.signal_as_if<NrSignalParams>()) {
    r.rsrq = ns->ss_rsrq;
    r.has_rsrq = true;
    r.snr = ns->ss_sinr;
    r.has_snr = true;
  } else if (auto* ws = c.signal_as_if<WcdmaSignalParams>()) {
    if (ws->has_ecio) {
      r.rsrq = ws->ecio;
      r.has_rsrq = true;
      r.snr = ws->ecio;
      r.has_snr = true;
    }
  } else if (auto* gs = c.signal_as_if<GsmSignalParams>()) {
    if (gs->has_snr) {
      r.snr = static_cast<float>(gs->snr);
      r.has_snr = true;
    }
    if (gs->has_c1c2) {
      r.c1 = gs->c1;
      r.c2 = gs->c2;
      r.has_c1c2 = true;
    }
  }

  r.nb_lte_detail = format_nb_lte(c);
  r.nb_gsm_detail = format_nb_gsm(c);
  r.nb_umts_detail = format_nb_umts(c);
  r.nb_lte = static_cast<uint32_t>(c.radio.meas_neighbors.size() + c.radio.intra_freq_neighbors.size() +
                                  c.radio.inter_freq_carriers.size());
  r.nb_gsm = static_cast<uint32_t>(c.radio.gsm_neighbors.size() + c.radio.geran_neighbors.size());
  r.nb_umts = static_cast<uint32_t>(c.radio.wcdma_neighbors.size() + c.radio.utra_neighbors.size());
  return r;
}

inline bool write_cells_csv(const std::string& path, const std::vector<CellIdentity>& cells) {
  std::ofstream out(path);
  if (!out) return false;

  out << "sep=,\n";
  // Vlad-compatible prefix + SDR / protocol extras
  out << "rat,arfcn,band,pci,bsic,ncc,bcc,mcc,mnc,lac,cid,signal_dbm,rsrq,c1,c2,serving,"
         "seen,first_seen,last_seen,lat,lon,"
         "duplex,dl_freq_mhz,ul_freq_mhz,bandwidth,ul_bw,ul_arfcn,snr,enb_rnc_id,ncell_id,"
         "nb_lte,nb_gsm,nb_umts,nb_lte_detail,nb_gsm_detail,nb_umts_detail\n";

  for (const auto& c : cells) {
    CellRow r = from_identity(c);
    auto empty_if_zero = [](auto v) -> std::string {
      if (v == 0) return {};
      return std::to_string(v);
    };

    out << r.rat << ',' << r.arfcn << ',' << r.band << ',';
    if (r.rat != "GSM") out << empty_if_zero(r.pci);
    out << ',';
    if (r.rat == "GSM") out << empty_if_zero(r.bsic);
    out << ',';
    if (r.rat == "GSM" && (r.ncc || r.bcc || r.bsic)) out << static_cast<unsigned>(r.ncc);
    out << ',';
    if (r.rat == "GSM" && (r.ncc || r.bcc || r.bsic)) out << static_cast<unsigned>(r.bcc);
    out << ',' << empty_if_zero(r.mcc) << ',' << empty_if_zero(r.mnc) << ',' << empty_if_zero(r.lac)
        << ',' << empty_if_zero(r.cid) << ',';
    if (r.has_signal) out << static_cast<int>(r.signal_dbm);
    out << ',';
    if (r.has_rsrq) out << r.rsrq;
    out << ',';
    if (r.has_c1c2) out << r.c1;
    out << ',';
    if (r.has_c1c2) out << r.c2;
    out << ',' << (r.serving ? 1 : 0) << ",,,,,";  // seen,first,last,lat,lon

    out << ',' << r.duplex << ',';
    if (r.dl_freq_mhz > 0) out << r.dl_freq_mhz;
    out << ',';
    if (r.ul_freq_mhz > 0) out << r.ul_freq_mhz;
    out << ',' << empty_if_zero(r.bandwidth_mhz) << ',' << empty_if_zero(r.ul_bw_mhz) << ','
        << empty_if_zero(r.ul_arfcn) << ',';
    if (r.has_snr) out << r.snr;
    out << ',' << empty_if_zero(r.enb_or_rnc) << ',' << empty_if_zero(r.ncell_id) << ','
        << empty_if_zero(r.nb_lte) << ',' << empty_if_zero(r.nb_gsm) << ',' << empty_if_zero(r.nb_umts)
        << ',' << csv_escape(r.nb_lte_detail) << ',' << csv_escape(r.nb_gsm_detail) << ','
        << csv_escape(r.nb_umts_detail) << '\n';
  }
  return true;
}

struct CidMismatch {
  std::string key;
  std::string rat;
  uint32_t arfcn{0};
  uint64_t ours_cid{0};
  uint64_t expect_cid{0};
  uint16_t ours_mcc{0};
  uint16_t ours_mnc{0};
  uint16_t expect_mcc{0};
  uint16_t expect_mnc{0};
};

struct ExpectDiff {
  size_t ours{0};
  size_t expect{0};
  size_t only_ours{0};
  size_t only_expect{0};
  size_t shared{0};
  std::map<std::string, size_t> ours_by_rat;
  std::map<std::string, size_t> expect_by_rat;
  std::map<std::string, size_t> shared_by_rat;
  std::map<std::string, size_t> only_ours_by_rat;
  std::map<std::string, size_t> only_expect_by_rat;
  size_t ours_serving{0};
  size_t expect_serving{0};
  size_t ours_with_cid{0};
  size_t expect_with_cid{0};
  size_t shared_cid_match{0};
  size_t shared_cid_mismatch{0};
  size_t shared_both_signal{0};
  std::vector<std::string> sample_only_ours;
  std::vector<std::string> sample_only_expect;
  std::vector<CidMismatch> cid_mismatches;
};

inline std::vector<std::string> split_csv_line(const std::string& line) {
  std::vector<std::string> fields;
  std::string cur;
  bool in_q = false;
  for (size_t i = 0; i < line.size(); ++i) {
    char c = line[i];
    if (in_q) {
      if (c == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          cur += '"';
          ++i;
        } else {
          in_q = false;
        }
      } else {
        cur += c;
      }
    } else if (c == '"') {
      in_q = true;
    } else if (c == ',') {
      fields.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  fields.push_back(cur);
  return fields;
}

inline std::vector<CellRow> read_cells_csv(const std::string& path) {
  std::ifstream in(path);
  std::vector<CellRow> rows;
  if (!in) return rows;

  std::string line;
  std::map<std::string, size_t> col;
  bool header_done = false;

  auto to_u64 = [](std::string_view s) -> uint64_t {
    if (s.empty()) return 0;
    try {
      return std::stoull(std::string(s));
    } catch (...) {
      return 0;
    }
  };
  auto to_f = [](std::string_view s, bool& ok) -> float {
    ok = false;
    if (s.empty()) return 0;
    try {
      ok = true;
      return std::stof(std::string(s));
    } catch (...) {
      return 0;
    }
  };

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    // BOM
    if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF) {
      if (line.size() >= 3) line = line.substr(3);
    }
    if (line.starts_with("sep=")) continue;

    auto fields = split_csv_line(line);
    if (!header_done) {
      for (size_t i = 0; i < fields.size(); ++i) col[fields[i]] = i;
      header_done = true;
      continue;
    }

    auto get = [&](std::string_view name) -> std::string {
      auto it = col.find(std::string(name));
      if (it == col.end() || it->second >= fields.size()) return {};
      return fields[it->second];
    };

    CellRow r;
    r.rat = get("rat");
    r.arfcn = static_cast<uint32_t>(to_u64(get("arfcn")));
    r.pci = static_cast<uint16_t>(to_u64(get("pci")));
    r.bsic = static_cast<uint16_t>(to_u64(get("bsic")));
    r.ncc = static_cast<uint8_t>(to_u64(get("ncc")));
    r.bcc = static_cast<uint8_t>(to_u64(get("bcc")));
    r.mcc = static_cast<uint16_t>(to_u64(get("mcc")));
    r.mnc = static_cast<uint16_t>(to_u64(get("mnc")));
    r.lac = static_cast<uint32_t>(to_u64(get("lac")));
    r.cid = to_u64(get("cid"));
    bool ok = false;
    r.signal_dbm = to_f(get("signal_dbm"), ok);
    r.has_signal = ok;
    r.rsrq = to_f(get("rsrq"), ok);
    r.has_rsrq = ok;
    r.serving = (get("serving") == "1");
    if (!r.rat.empty()) rows.push_back(std::move(r));
  }
  return rows;
}

inline ExpectDiff diff_cells(const std::vector<CellIdentity>& ours_cells,
                             const std::vector<CellRow>& expect) {
  ExpectDiff d;
  std::map<std::string, CellRow> ours_map;
  std::map<std::string, CellRow> exp_map;

  for (const auto& c : ours_cells) {
    CellRow r = from_identity(c);
    ++d.ours;
    ++d.ours_by_rat[r.rat];
    if (r.serving) ++d.ours_serving;
    if (r.cid) ++d.ours_with_cid;
    ours_map.emplace(r.key(), std::move(r));
  }
  for (const auto& r : expect) {
    ++d.expect;
    ++d.expect_by_rat[r.rat];
    if (r.serving) ++d.expect_serving;
    if (r.cid) ++d.expect_with_cid;
    exp_map.emplace(r.key(), r);
  }

  for (const auto& [k, r] : ours_map) {
    if (exp_map.contains(k)) {
      ++d.shared;
      ++d.shared_by_rat[r.rat];
      const auto& e = exp_map.at(k);
      if (r.cid && e.cid) {
        if (r.cid == e.cid)
          ++d.shared_cid_match;
        else {
          ++d.shared_cid_mismatch;
          if (d.cid_mismatches.size() < 20) {
            d.cid_mismatches.push_back(CidMismatch{
                .key = k,
                .rat = r.rat,
                .arfcn = r.arfcn,
                .ours_cid = r.cid,
                .expect_cid = e.cid,
                .ours_mcc = r.mcc,
                .ours_mnc = r.mnc,
                .expect_mcc = e.mcc,
                .expect_mnc = e.mnc,
            });
          }
        }
      }
      if (r.has_signal && e.has_signal) ++d.shared_both_signal;
    } else {
      ++d.only_ours;
      ++d.only_ours_by_rat[r.rat];
      if (d.sample_only_ours.size() < 12) d.sample_only_ours.push_back(k);
    }
  }
  for (const auto& [k, r] : exp_map) {
    if (!ours_map.contains(k)) {
      ++d.only_expect;
      ++d.only_expect_by_rat[r.rat];
      if (d.sample_only_expect.size() < 12) d.sample_only_expect.push_back(k);
    }
  }
  return d;
}

}  // namespace QCom::Tools
