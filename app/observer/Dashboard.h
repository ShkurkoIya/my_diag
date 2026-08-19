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
#include <observer/model/BandInfo.h>
#include <observer/model/CellIdentity.h>
#include <observer/model/Utils.h>
#include <poll.h>
#include <qcom/parser/QualcomParser.h>
#include <sstream>
#include <string>
#include <string_view>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace Observer {

[[nodiscard]] inline const char* cell_fill_status(const QCom::CellIdentity& c) noexcept {
  const bool has_plmn = c.passport.mcc != 0;
  if (c.rat == QCom::RatType::WCDMA) {
    const bool has_radio =
        c.radio.freq() > 0 && c.radio.freq() <= 16383 && c.radio.pci_bsic() <= 511;
    const bool has_id = c.passport.cell_id != 0 && c.passport.tac != 0;
    if (has_radio && has_plmn && has_id) return "FULL";
    if (has_radio && has_plmn) return "PLMN";
    if (has_radio) return "RADIO";
    if (has_plmn) return "WEAK";
    return "EMPTY";
  }
  const bool has_radio = c.radio.freq() != 0 && c.radio.pci_bsic() != 0;
  const bool has_id =
      QCom::Utils::valid_lte_eci(c.passport.cell_id) && QCom::Utils::valid_lte_tac(c.passport.tac);
  if (has_radio && has_plmn && has_id) return "FULL";
  if (has_radio && has_plmn) return "PLMN";
  if (has_radio) return "RADIO";
  if (has_plmn) return "WEAK";
  return "EMPTY";
}

[[nodiscard]] inline std::string fmt_plmn(const QCom::CellPassport& p) {
  if (p.mcc == 0) return "-";
  std::ostringstream os;
  os << p.mcc << '-' << std::setfill('0') << std::setw(2) << p.mnc;
  return os.str();
}

[[nodiscard]] inline std::string fmt_band(const QCom::CellIdentity& c) {
  if (c.rat == QCom::RatType::LTE) {
    auto bi = QCom::BandInfo::lte_from_earfcn(c.radio.freq());
    if (bi.band) return bi.name;
  } else if (c.rat == QCom::RatType::WCDMA) {
    auto bi = QCom::BandInfo::umts_from_uarfcn(c.radio.freq());
    if (bi.band) return bi.name;
  }
  return "-";
}

[[nodiscard]] inline std::string fmt_mhz(const QCom::CellIdentity& c) {
  double mhz = 0;
  if (c.rat == QCom::RatType::LTE)
    mhz = QCom::BandInfo::lte_from_earfcn(c.radio.freq()).dl_mhz;
  else if (c.rat == QCom::RatType::WCDMA)
    mhz = QCom::BandInfo::umts_from_uarfcn(c.radio.freq()).dl_mhz;
  if (mhz <= 0) return "-";
  std::ostringstream os;
  os << std::fixed << std::setprecision(1) << mhz;
  return os.str();
}

[[nodiscard]] inline std::string fmt_signal(const QCom::CellIdentity& c) {
  std::ostringstream os;
  os << std::fixed << std::setprecision(1);
  if (auto* s = c.signal.get_if<QCom::LteSignalParams>()) {
    if (!QCom::Utils::valid_lte_rsrp(s->rsrp)) return "-";
    os << s->rsrp;
    return os.str();
  }
  if (auto* s = c.signal.get_if<QCom::NrSignalParams>()) {
    os << s->ss_rsrp;
    return os.str();
  }
  if (auto* s = c.signal.get_if<QCom::WcdmaSignalParams>()) {
    os << s->rscp;
    return os.str();
  }
  if (auto* s = c.signal.get_if<QCom::GsmSignalParams>()) {
    os << static_cast<int>(s->rxlev);
    return os.str();
  }
  return "-";
}

[[nodiscard]] inline std::string fmt_rsrq(const QCom::CellIdentity& c) {
  if (auto* s = c.signal.get_if<QCom::LteSignalParams>()) {
    if (!QCom::Utils::valid_lte_rsrp(s->rsrp)) return "-";
    // -30.0 is the MI "raw=0" sentinel, not a real RSRQ.
    if (s->rsrq <= -30.0f || s->rsrq > -1.0f) return "-";
    std::ostringstream os;
    os << std::fixed << std::setprecision(1) << s->rsrq;
    return os.str();
  }
  return "-";
}

[[nodiscard]] inline std::string fmt_bw(const QCom::CellIdentity& c) {
  if (auto* r = c.radio.get_if<QCom::LteRadioParams>()) {
    if (r->dl_bw == 0) return "-";
    auto one = [](uint8_t mhz) -> std::string {
      // ASN.1 6 RB → stored as 1 (=1.4 MHz).
      if (mhz == 1) return "1.4";
      return std::to_string(static_cast<int>(mhz));
    };
    if (r->ul_bw != 0 && r->ul_bw != r->dl_bw) return one(r->dl_bw) + "/" + one(r->ul_bw);
    return one(r->dl_bw);
  }
  return "-";
}

[[nodiscard]] inline std::string fmt_bar(const QCom::CellIdentity& c) {
  return c.passport.cell_barred ? "Y" : "-";
}

[[nodiscard]] inline std::string fmt_nb(const QCom::CellIdentity& c) {
  const size_t n = c.radio.meas_neighbors.size() + c.radio.inter_freq_carriers.size() +
                   c.radio.intra_freq_neighbors.size();
  if (n == 0) return "-";
  return std::to_string(n);
}

[[nodiscard]] inline double signal_sort_key(const QCom::CellIdentity& c) {
  if (auto* s = c.signal.get_if<QCom::LteSignalParams>()) return s->rsrp;
  if (auto* s = c.signal.get_if<QCom::NrSignalParams>()) return s->ss_rsrp;
  if (auto* s = c.signal.get_if<QCom::WcdmaSignalParams>()) return static_cast<double>(s->rscp);
  if (auto* s = c.signal.get_if<QCom::GsmSignalParams>()) return static_cast<double>(s->rxlev);
  return -999.0;
}

[[nodiscard]] inline std::string clip_field(std::string s, std::size_t width, bool right = false) {
  if (s.size() > width) s.resize(width);
  if (s.size() < width) {
    if (right)
      s.insert(0, width - s.size(), ' ');
    else
      s.append(width - s.size(), ' ');
  }
  return s;
}

[[nodiscard]] inline bool is_lte_full_row(const QCom::CellIdentity& c) noexcept {
  return c.rat == QCom::RatType::LTE && QCom::Utils::valid_lte_earfcn(c.radio.freq()) &&
         c.radio.pci_bsic() != 0 && c.passport.mcc != 0 &&
         QCom::Utils::valid_lte_eci(c.passport.cell_id) &&
         QCom::Utils::valid_lte_tac(c.passport.tac);
}

[[nodiscard]] inline std::vector<QCom::CellIdentity> filter_display_cells(
    const std::vector<QCom::CellIdentity>& cells) {
  std::vector<QCom::CellIdentity> rows;
  rows.reserve(cells.size());
  for (const auto& c : cells) {
    // Hide LTE EARFCN|0 / PCI=0 ghosts (SSS headers, B193 without cell rows).
    if (c.rat == QCom::RatType::LTE && c.radio.pci_bsic() == 0) continue;
    if (c.rat == QCom::RatType::LTE && !QCom::Utils::valid_lte_earfcn(c.radio.freq())) continue;
    if (c.rat == QCom::RatType::LTE && c.radio.pci_bsic() > 503) continue;
    // CMGRMI/CPSI transitional PLMN sentinels (0xFFFF → GUI "65535").
    if (c.rat == QCom::RatType::LTE &&
        (c.passport.mcc == 0xFFFF || c.passport.mnc == 0xFFFF || c.passport.mcc > 999))
      continue;
    // Hide non-serving RADIO rows with garbage-strong RSRP (decode ghosts).
    if (c.rat == QCom::RatType::LTE && !c.is_serving && !c.passport.has_identity()) {
      if (auto* s = c.signal.get_if<QCom::LteSignalParams>()) {
        if (s->rsrp > -45.0f && s->rsrp != 0.0f) continue;
      }
    }
    rows.push_back(c);
  }
  return rows;
}

/// Tree: eNB site → FULL carriers → same-EARFCN neighbors; orphans by EARFCN; other RATs.
[[nodiscard]] inline std::string render_cells_table(const std::vector<QCom::CellIdentity>& cells,
                                                    const char* title) {
  auto rows = filter_display_cells(cells);
  auto by_rsrp = [](const QCom::CellIdentity& a, const QCom::CellIdentity& b) {
    if (a.is_serving != b.is_serving) return a.is_serving > b.is_serving;
    return signal_sort_key(a) > signal_sort_key(b);
  };

  struct Site {
    uint32_t enb{0};
    uint16_t mcc{0};
    uint16_t mnc{0};
    uint32_t tac{0};
    std::vector<std::size_t> fulls;                                // indices into rows
    std::map<uint32_t, std::vector<std::size_t>> neigh_by_earfcn;  // incomplete
    std::vector<std::size_t> site_orphans;                         // incomplete, other EARFCN
  };

  std::map<uint32_t, Site> sites;
  std::vector<std::size_t> lte_incomplete;
  std::vector<std::size_t> other_rat;

  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& c = rows[i];
    if (c.rat != QCom::RatType::LTE) {
      other_rat.push_back(i);
      continue;
    }
    if (is_lte_full_row(c)) {
      const uint32_t enb = c.passport.enb_id();
      auto& site = sites[enb];
      site.enb = enb;
      if (site.mcc == 0) {
        site.mcc = c.passport.mcc;
        site.mnc = c.passport.mnc;
        site.tac = c.passport.tac;
      }
      site.fulls.push_back(i);
    } else {
      lte_incomplete.push_back(i);
    }
  }

  // Nest incomplete under site that owns same EARFCN (else same TAC site, else free).
  std::map<uint32_t, std::vector<std::size_t>> free_by_earfcn;  // no parent site
  for (std::size_t idx : lte_incomplete) {
    const auto& c = rows[idx];
    const uint32_t earfcn = c.radio.freq();
    uint32_t parent_enb = 0;
    for (auto& [enb, site] : sites) {
      for (std::size_t fi : site.fulls) {
        if (rows[fi].radio.freq() == earfcn) {
          parent_enb = enb;
          break;
        }
      }
      if (parent_enb) break;
    }
    if (!parent_enb && c.passport.tac != 0) {
      for (auto& [enb, site] : sites) {
        if (site.tac == c.passport.tac) {
          parent_enb = enb;
          break;
        }
      }
      if (parent_enb) {
        sites[parent_enb].site_orphans.push_back(idx);
        continue;
      }
    }
    if (parent_enb) {
      sites[parent_enb].neigh_by_earfcn[earfcn].push_back(idx);
    } else {
      free_by_earfcn[earfcn].push_back(idx);
    }
  }

  std::vector<uint32_t> site_order;
  site_order.reserve(sites.size());
  for (const auto& [enb, _] : sites) site_order.push_back(enb);
  std::sort(site_order.begin(), site_order.end(), [&](uint32_t a, uint32_t b) {
    double sa = -999, sb = -999;
    for (std::size_t i : sites[a].fulls) sa = std::max(sa, signal_sort_key(rows[i]));
    for (std::size_t i : sites[b].fulls) sb = std::max(sb, signal_sort_key(rows[i]));
    if (sa != sb) return sa > sb;
    return a < b;
  });

  auto fmt_cell_line = [&](const QCom::CellIdentity& c, bool as_neigh) {
    std::ostringstream os;
    const char* fill = cell_fill_status(c);
    if (c.is_serving)
      os << "* ";
    else if (as_neigh)
      os << "n ";
    else
      os << "  ";
    os << clip_field(fmt_band(c), 4) << ' ' << std::setw(5) << c.radio.freq() << '/' << std::setw(3)
       << c.radio.pci_bsic();
    const std::string mhz = fmt_mhz(c);
    if (mhz != "-") os << "  " << std::setw(6) << mhz << "MHz";
    const std::string bw = fmt_bw(c);
    if (bw != "-") os << "  bw" << bw;
    os << "  " << clip_field(fmt_plmn(c.passport), 7);
    if (is_lte_full_row(c)) {
      os << "  CID " << c.passport.cell_id << " (loc "
         << static_cast<unsigned>(c.passport.local_cell_id()) << ")";
    } else if (c.passport.tac != 0 && !(QCom::Utils::valid_lte_eci(c.passport.cell_id))) {
      os << "  TAC " << c.passport.tac;
    }
    os << "  " << std::setw(6) << fmt_signal(c);
    const std::string rq = fmt_rsrq(c);
    if (rq != "-") os << "/" << rq;
    os << "  " << fill;
    if (c.passport.cell_barred) os << "  BAR";
    return os.str();
  };

  auto fmt_other_line = [&](const QCom::CellIdentity& c) {
    std::ostringstream os;
    if (c.is_serving)
      os << "* ";
    else
      os << "  ";
    os << clip_field(QCom::to_string(c.rat), 5) << ' ' << clip_field(fmt_band(c), 4) << ' '
       << std::setw(5) << c.radio.freq() << '/' << std::setw(3) << c.radio.pci_bsic() << "  "
       << clip_field(fmt_plmn(c.passport), 7);
    if (c.passport.has_identity()) os << "  CID " << c.passport.cell_id;
    if (c.passport.tac != 0) os << "  LAC/TAC " << c.passport.tac;
    os << "  " << std::setw(6) << fmt_signal(c) << "  " << cell_fill_status(c);
    return os.str();
  };

  const std::size_t n_full = [&] {
    std::size_t n = 0;
    for (const auto& [_, s] : sites) n += s.fulls.size();
    return n;
  }();
  const std::size_t n_neigh = lte_incomplete.size();

  std::ostringstream out;
  out << title << " — " << sites.size() << " site(s), " << n_full << " cell(s), " << n_neigh
      << " neigh, " << other_rat.size() << " other RAT  (rows " << rows.size() << ")\n";
  out << "  site = eNB  |  cell = FULL carrier  |  n = neighbor without CID\n";

  if (sites.empty() && free_by_earfcn.empty() && other_rat.empty()) {
    out << "  (empty)\n";
    return out.str();
  }

  for (std::size_t si = 0; si < site_order.size(); ++si) {
    Site& site = sites[site_order[si]];
    std::sort(site.fulls.begin(), site.fulls.end(),
              [&](std::size_t a, std::size_t b) { return by_rsrp(rows[a], rows[b]); });
    for (auto& [_, vec] : site.neigh_by_earfcn)
      std::sort(vec.begin(), vec.end(),
                [&](std::size_t a, std::size_t b) { return by_rsrp(rows[a], rows[b]); });
    std::sort(site.site_orphans.begin(), site.site_orphans.end(),
              [&](std::size_t a, std::size_t b) { return by_rsrp(rows[a], rows[b]); });

    std::size_t neigh_n = site.site_orphans.size();
    for (const auto& [_, v] : site.neigh_by_earfcn) neigh_n += v.size();

    double best = -999;
    for (std::size_t i : site.fulls) best = std::max(best, signal_sort_key(rows[i]));

    out << "■ eNB " << site.enb << "  " << site.mcc << '-' << std::setfill('0') << std::setw(2)
        << site.mnc << std::setfill(' ') << "  TAC " << site.tac << "  cells " << site.fulls.size()
        << "  neigh " << neigh_n;
    if (best > -200) out << "  best " << std::fixed << std::setprecision(1) << best;
    out << '\n';

    for (std::size_t ci = 0; ci < site.fulls.size(); ++ci) {
      const auto& cell = rows[site.fulls[ci]];
      const uint32_t earfcn = cell.radio.freq();
      auto nit = site.neigh_by_earfcn.find(earfcn);
      std::vector<std::size_t> neighs =
          (nit != site.neigh_by_earfcn.end()) ? std::move(nit->second) : std::vector<std::size_t>{};
      if (nit != site.neigh_by_earfcn.end()) site.neigh_by_earfcn.erase(nit);

      const bool more_top = (ci + 1 < site.fulls.size()) || !site.site_orphans.empty() ||
                            !site.neigh_by_earfcn.empty();
      out << (more_top ? "├─ " : "└─ ") << "cell " << fmt_cell_line(cell, false) << '\n';
      const char* pad = more_top ? "│  " : "   ";
      for (std::size_t ni = 0; ni < neighs.size(); ++ni) {
        out << pad << (ni + 1 == neighs.size() ? "└─ " : "├─ ")
            << fmt_cell_line(rows[neighs[ni]], true) << '\n';
      }
    }

    // Rare: EARFCN assigned to site but no FULL carrier row (should be empty).
    {
      std::vector<uint32_t> leftover;
      for (const auto& [e, v] : site.neigh_by_earfcn)
        if (!v.empty()) leftover.push_back(e);
      std::sort(leftover.begin(), leftover.end());
      for (std::size_t ei = 0; ei < leftover.size(); ++ei) {
        auto& vec = site.neigh_by_earfcn[leftover[ei]];
        const bool more_top = (ei + 1 < leftover.size()) || !site.site_orphans.empty();
        out << (more_top ? "├─ " : "└─ ") << "earfcn " << leftover[ei] << " (no FULL yet)\n";
        const char* pad = more_top ? "│  " : "   ";
        for (std::size_t ni = 0; ni < vec.size(); ++ni) {
          out << pad << (ni + 1 == vec.size() ? "└─ " : "├─ ") << fmt_cell_line(rows[vec[ni]], true)
              << '\n';
        }
      }
    }
    for (std::size_t oi = 0; oi < site.site_orphans.size(); ++oi) {
      const bool last = oi + 1 == site.site_orphans.size();
      out << (last ? "└─ " : "├─ ") << "other " << fmt_cell_line(rows[site.site_orphans[oi]], true)
          << '\n';
    }
    if (si + 1 < site_order.size() || !free_by_earfcn.empty() || !other_rat.empty()) out << '\n';
  }

  if (!free_by_earfcn.empty()) {
    out << "◇ no eNB yet (incomplete)\n";
    std::vector<uint32_t> ears;
    for (const auto& [e, _] : free_by_earfcn) ears.push_back(e);
    std::sort(ears.begin(), ears.end());
    for (std::size_t ei = 0; ei < ears.size(); ++ei) {
      auto& vec = free_by_earfcn[ears[ei]];
      std::sort(vec.begin(), vec.end(),
                [&](std::size_t a, std::size_t b) { return by_rsrp(rows[a], rows[b]); });
      const bool last_e = ei + 1 == ears.size() && other_rat.empty();
      out << (last_e && vec.empty() ? "└─ " : "├─ ") << "EARFCN " << ears[ei] << '\n';
      for (std::size_t ni = 0; ni < vec.size(); ++ni) {
        const bool last_n = ni + 1 == vec.size() && last_e;
        out << "│  " << (last_n ? "└─ " : "├─ ") << fmt_cell_line(rows[vec[ni]], true) << '\n';
      }
    }
    if (!other_rat.empty()) out << '\n';
  }

  if (!other_rat.empty()) {
    std::sort(other_rat.begin(), other_rat.end(),
              [&](std::size_t a, std::size_t b) { return by_rsrp(rows[a], rows[b]); });
    out << "◇ other RAT\n";
    for (std::size_t oi = 0; oi < other_rat.size(); ++oi) {
      const bool last = oi + 1 == other_rat.size();
      out << (last ? "└─ " : "├─ ") << fmt_other_line(rows[other_rat[oi]]) << '\n';
    }
  }

  return out.str();
}

inline void print_cells_table(const std::vector<QCom::CellIdentity>& cells, const char* title) {
  std::cout << "\n" << render_cells_table(cells, title);
}

/// In-place TTY redraw so cell updates don't scroll as a sausage of duplicate tables.
[[nodiscard]] inline std::string scanner_log_timestamp() {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto tt = clock::to_time_t(now);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
  std::tm tm{};
  localtime_r(&tt, &tm);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900,
                tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<int>(ms.count()));
  return buf;
}

[[nodiscard]] inline std::string scanner_log_session_path(std::string_view primary) {
  // /tmp/qcom_live_scanner.log → /tmp/qcom_live_scanner_20260810_154312.log
  using clock = std::chrono::system_clock;
  const auto tt = clock::to_time_t(clock::now());
  std::tm tm{};
  localtime_r(&tt, &tm);
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
  std::string p(primary);
  const auto dot = p.rfind('.');
  if (dot != std::string::npos && p.find('/', dot) == std::string::npos)
    return p.substr(0, dot) + "_" + stamp + p.substr(dot);
  return p + "_" + stamp;
}

class LiveDashboard {
public:
  explicit LiveDashboard(bool enable_inplace)
      : m_tty(enable_inplace && ::isatty(STDOUT_FILENO) != 0) {}

  void set_banner(std::string banner) {
    std::lock_guard lock(m_mu);
    m_banner = std::move(banner);
  }

  /// Persist every note to primary (current run, truncated) + session (kept).
  bool open_scanner_log(const std::string& primary, std::string_view session_header) {
    std::lock_guard lock(m_mu);
    m_log_path = primary;
    m_log_session_path = scanner_log_session_path(primary);
    m_log = std::ofstream(m_log_path, std::ios::out | std::ios::trunc);
    m_log_session = std::ofstream(m_log_session_path, std::ios::out | std::ios::trunc);
    if (!m_log || !m_log_session) {
      m_log.close();
      m_log_session.close();
      return false;
    }
    const std::string hdr = std::string("# live_scanner log  started ") + scanner_log_timestamp() +
                            "  pid=" + std::to_string(::getpid()) + "\n" +
                            std::string(session_header) + "# primary=" + m_log_path +
                            "\n"
                            "# session=" +
                            m_log_session_path +
                            "\n"
                            "# ---\n";
    m_log << hdr << std::flush;
    m_log_session << hdr << std::flush;
    return true;
  }

  [[nodiscard]] const std::string& log_path() const { return m_log_path; }
  [[nodiscard]] const std::string& log_session_path() const { return m_log_session_path; }

  void note(std::string line) {
    std::lock_guard lock(m_mu);
    append_log_locked(line);
    m_notes.push_back(std::move(line));
    if (m_notes.size() > 48) m_notes.erase(m_notes.begin());
    if (!m_table.empty()) paint_locked();
  }

  /// Newline-joined recent notes for live JSON → tower_gui scanner log.
  [[nodiscard]] std::string notes_joined() const {
    std::lock_guard lock(m_mu);
    std::string out;
    out.reserve(m_notes.size() * 64);
    for (std::size_t i = 0; i < m_notes.size(); ++i) {
      if (i) out.push_back('\n');
      out += m_notes[i];
    }
    return out;
  }

  void show_cells(const std::vector<QCom::CellIdentity>& cells, int update_n) {
    std::lock_guard lock(m_mu);
    m_table = render_cells_table(cells, "Cells");
    m_update_n = update_n;
    paint_locked();
  }

  void leave_inplace() {
    std::lock_guard lock(m_mu);
    if (m_tty && m_active) {
      // Leave the last frame on screen, then a blank line before end summary.
      std::cout << "\n" << std::flush;
    }
    m_active = false;
    if (m_log.is_open()) {
      m_log << "# stopped " << scanner_log_timestamp() << "\n" << std::flush;
      m_log.close();
    }
    if (m_log_session.is_open()) {
      m_log_session << "# stopped " << scanner_log_timestamp() << "\n" << std::flush;
      m_log_session.close();
    }
  }

private:
  void append_log_locked(std::string_view line) {
    if (!m_log.is_open() && !m_log_session.is_open()) return;
    const std::string ts = scanner_log_timestamp();
    const std::string row = ts + "  " + std::string(line) + "\n";
    if (m_log.is_open()) {
      m_log << row;
      m_log.flush();
    }
    if (m_log_session.is_open()) {
      m_log_session << row;
      m_log_session.flush();
    }
  }

  void paint_locked() {
    if (m_tty) {
      // Home + clear-down: redraw one frame instead of appending tables.
      std::cout << "\033[H\033[J";
      m_active = true;
    } else if (m_active) {
      std::cout << "\n";
    }
    m_active = true;

    if (!m_banner.empty()) std::cout << m_banner;
    std::cout << m_table;
    std::cout << "updates=" << m_update_n;
    if (!m_notes.empty()) {
      std::cout << "\n---\n";
      // Terminal keeps a short tail; full buffer goes to live JSON / GUI.
      const std::size_t start = m_notes.size() > 8 ? m_notes.size() - 8 : 0;
      for (std::size_t i = start; i < m_notes.size(); ++i) std::cout << m_notes[i] << "\n";
    } else {
      std::cout << "\n";
    }
    std::cout << std::flush;
  }

  mutable std::mutex m_mu;
  bool m_tty{false};
  bool m_active{false};
  std::string m_banner;
  std::string m_table;
  std::vector<std::string> m_notes;
  int m_update_n{0};
  std::string m_log_path;
  std::string m_log_session_path;
  std::ofstream m_log;
  std::ofstream m_log_session;
};

/// Fallback names for codes that flew but have no registered parser (or for table hints).
[[nodiscard]] inline std::string_view known_code_hint(QCom::LogCode code) noexcept {
  switch (code) {
    case 0xB061: return "LTE MAC RACH trigger";
    case 0xB062: return "LTE MAC RACH response";
    case 0xB063: return "LTE MAC DL TB";
    case 0xB064: return "LTE MAC UL TB";
    case 0xB0C0: return "LTE RRC OTA / SIB ASN.1";
    case 0xB0C1: return "LTE RRC MIB";
    case 0xB0C2: return "LTE RRC serving cell";
    case 0xB0C3: return "LTE RRC PLMN search req";
    case 0xB0C4: return "LTE RRC PLMN search rsp";
    case 0xB0CA: return "LTE RRC log meas";
    case 0xB0CB: return "LTE RRC paging";
    case 0xB0CD: return "LTE RRC CA combos";
    case 0xB0E0: return "LTE NAS ESM sec in";
    case 0xB0E2: return "LTE NAS ESM plain in";
    case 0xB0E3: return "LTE NAS ESM plain out";
    case 0xB0EA: return "LTE NAS EMM sec in";
    case 0xB0EB: return "LTE NAS EMM sec out";
    case 0xB0EC: return "LTE NAS EMM plain in";
    case 0xB0ED: return "LTE NAS EMM plain out";
    case 0xB0EE: return "LTE NAS EMM state";
    case 0xB113: return "LTE LL1 PSS results";
    case 0xB114: return "LTE LL1 serving frame timing (TA)";
    case 0xB115: return "LTE LL1 SSS results";
    case 0xB123: return "LTE LL1 ncell CER";
    case 0xB167: return "LTE MAC RAR msg1";
    case 0xB168: return "LTE MAC RAR msg2";
    case 0xB169: return "LTE MAC RAR msg3";
    case 0xB16A: return "LTE MAC RAR msg4";
    case 0xB175: return "LTE LL1/ML1 cell metrics (no identity)";
    case 0xB176: return "LTE initial acquisition";
    case 0xB179: return "LTE ML1 conn intra meas";
    case 0xB17F: return "LTE ML1 serving meas";
    case 0xB180: return "LTE ML1 neighbor meas";
    case 0xB181: return "LTE ML1 intra resel";
    case 0xB192: return "LTE PHY idle neighbor meas";
    case 0xB193: return "LTE ML1 serving meas rsp";
    case 0xB194: return "LTE ML1 search req/rsp";
    case 0xB195: return "LTE PHY conn neighbor meas";
    case 0xB197: return "LTE ML1 serving info";
    case 0xB821: return "NR RRC OTA";
    case 0xB822: return "NR RRC MIB";
    case 0xB823: return "NR RRC serving";
    case 0xB82B: return "NR RRC detected cell";
    case 0x4127: return "WCDMA cell ID";
    case 0x412B: return "WCDMA SIB";
    case 0x412F: return "WCDMA RRC OTA";
    case 0x4179: return "WCDMA PN search ed.2";
    case 0x41B0: return "WCDMA freq scan";
    case 0x512F: return "GSM RR signaling";
    case 0x5134: return "GSM RR cell information";
    default: return {};
  }
}

inline void print_log_code_table(const std::map<QCom::LogCode, uint64_t>& hist,
                                 const QCom::QualcomParser& parser) {
  std::vector<std::pair<QCom::LogCode, uint64_t>> ranked(hist.begin(), hist.end());
  std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.second > b.second; });

  const auto& stats = parser.code_stats();
  std::cout << "\nLog codes seen (all that flew):\n";
  std::cout
      << "┌──────────┬────────┬───────┬──────────┬──────────────────────────────────────────────┐\n"
      << "│   Code   │ Count  │ Supp. │  Parsed  │ Description                                  │\n"
      << "├──────────┼────────┼───────┼──────────┼──────────────────────────────────────────────┤"
         "\n";

  for (const auto& [code, count] : ranked) {
    const bool supported = parser.is_supported(code);
    QCom::LogCodeStats st{};
    if (auto it = stats.find(code); it != stats.end()) st = it->second;

    const char* parsed = "—";
    if (!supported) {
      parsed = "no parser";
    } else if (st.with_events > 0 && st.empty == 0 && st.error == 0) {
      parsed = "OK events";
    } else if (st.with_events > 0) {
      parsed = "partial";
    } else if (st.empty > 0 && st.error == 0) {
      parsed = "empty";
    } else if (st.error > 0) {
      parsed = "errors";
    } else if (st.seen > 0) {
      parsed = "seen";
    }

    std::string_view name = parser.code_name(code);
    if (name.empty()) name = known_code_hint(code);
    if (name.empty()) name = "(unknown)";

    std::ostringstream desc;
    desc << name;
    if (supported && st.seen > 0) {
      desc << "  [ev=" << st.with_events << " empty=" << st.empty << " err=" << st.error << "]";
    }

    std::cout << "│ 0x" << std::hex << std::setw(4) << std::setfill('0') << code << std::dec
              << std::setfill(' ') << " │ " << std::setw(6) << count << " │ " << std::setw(5)
              << (supported ? "YES" : "no") << " │ " << std::left << std::setw(8) << parsed
              << std::right << " │ " << std::left << std::setw(44) << desc.str().substr(0, 44)
              << std::right << " │\n";
  }
  std::cout << "└──────────┴────────┴───────┴──────────┴───────────────────────────────────────────"
               "───┘\n";
}

inline std::string snapshot_fingerprint(const std::vector<QCom::CellIdentity>& cells) {
  std::string s;
  s.reserve(cells.size() * 24);
  for (const auto& c : cells) {
    s += std::to_string(static_cast<int>(c.rat));
    s += ':';
    s += std::to_string(c.radio.freq());
    s += '/';
    s += std::to_string(c.radio.pci_bsic());
    s += c.is_serving ? 'S' : 'n';
    s += std::to_string(c.passport.mcc);
    s += '-';
    s += std::to_string(c.passport.mnc);
    s += '/';
    s += std::to_string(c.passport.cell_id);
    s += ';';
  }
  return s;
}

}  // namespace Observer
