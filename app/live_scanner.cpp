/// @file live_scanner.cpp
/// @brief DeviceCatalog → DIAG + QMI → RadioScannerEngine → stdout.
///
/// Usage:
///   live_scanner [--list] [--device PATH] [--qmi PATH] [--duration SEC]
///                [--baud N] [--no-init] [--no-qmi] [--qmi-period MS]
///                [--prefer-lte] [--plmn-search] [--search-period SEC]
///                [--at-cops] [--deep-search] [--at-cereg] [--earfcn-hop]
///                [--hop-dwell SEC] [--hop-max N] [--hop-cfun] [--hop-band-clip]
///                [--no-plmn-search]
#include <atomic>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <termios.h>
#include <poll.h>

namespace {
std::atomic<bool> g_user_stop{false};
extern "C" void live_scanner_on_signal(int) { g_user_stop.store(true, std::memory_order_relaxed); }
}  // namespace

#include "core/BandInfo.h"
#include "core/CellIdentity.h"
#include "core/QualcomParser.h"
#include "core/Utils.h"
#include "lte/LteParser.h"
#include "lte/LteRrcOta.h"
#include "qmi_observer/bridge.hpp"
#include "qmi_observer/device/catalog.hpp"
#include "qmi_observer/qmi_observer.hpp"
#include "transport/DiagSourceConfig.h"
#include "transport/LinuxSource.h"
#include "transport/ScannerEngine.h"
#include "transport/SourceFactory.h"
#include "TowerExport.h"

namespace {

struct SelectedModem {
  std::string id;
  std::string diag;
  std::optional<std::string> qmi;
  std::optional<std::string> at;
};

[[nodiscard]] const char* cell_fill_status(const QCom::CellIdentity& c) noexcept {
  const bool has_radio = c.radio.freq() != 0 && c.radio.pci_bsic() != 0;
  const bool has_plmn = c.passport.mcc != 0;
  const bool has_id = QCom::Utils::valid_lte_eci(c.passport.cell_id) &&
                      QCom::Utils::valid_lte_tac(c.passport.tac);
  if (has_radio && has_plmn && has_id) return "FULL";
  if (has_radio && has_plmn) return "PLMN";
  if (has_radio) return "RADIO";
  if (has_plmn) return "WEAK";
  return "EMPTY";
}

[[nodiscard]] std::string fmt_plmn(const QCom::CellPassport& p) {
  if (p.mcc == 0) return "-";
  std::ostringstream os;
  os << p.mcc << '-' << std::setfill('0') << std::setw(2) << p.mnc;
  return os.str();
}

[[nodiscard]] std::string fmt_band(const QCom::CellIdentity& c) {
  if (c.rat == QCom::RatType::LTE) {
    auto bi = QCom::BandInfo::lte_from_earfcn(c.radio.freq());
    if (bi.band) return bi.name;
  } else if (c.rat == QCom::RatType::WCDMA) {
    auto bi = QCom::BandInfo::umts_from_uarfcn(c.radio.freq());
    if (bi.band) return bi.name;
  }
  return "-";
}

[[nodiscard]] std::string fmt_mhz(const QCom::CellIdentity& c) {
  double mhz = 0;
  if (c.rat == QCom::RatType::LTE) mhz = QCom::BandInfo::lte_from_earfcn(c.radio.freq()).dl_mhz;
  else if (c.rat == QCom::RatType::WCDMA) mhz = QCom::BandInfo::umts_from_uarfcn(c.radio.freq()).dl_mhz;
  if (mhz <= 0) return "-";
  std::ostringstream os;
  os << std::fixed << std::setprecision(1) << mhz;
  return os.str();
}

[[nodiscard]] std::string fmt_signal(const QCom::CellIdentity& c) {
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

[[nodiscard]] std::string fmt_rsrq(const QCom::CellIdentity& c) {
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

[[nodiscard]] std::string fmt_bw(const QCom::CellIdentity& c) {
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

[[nodiscard]] std::string fmt_bar(const QCom::CellIdentity& c) {
  return c.passport.cell_barred ? "Y" : "-";
}

[[nodiscard]] std::string fmt_nb(const QCom::CellIdentity& c) {
  const size_t n = c.radio.meas_neighbors.size() + c.radio.inter_freq_carriers.size() +
                   c.radio.intra_freq_neighbors.size();
  if (n == 0) return "-";
  return std::to_string(n);
}

[[nodiscard]] double signal_sort_key(const QCom::CellIdentity& c) {
  if (auto* s = c.signal.get_if<QCom::LteSignalParams>()) return s->rsrp;
  if (auto* s = c.signal.get_if<QCom::NrSignalParams>()) return s->ss_rsrp;
  if (auto* s = c.signal.get_if<QCom::WcdmaSignalParams>()) return static_cast<double>(s->rscp);
  if (auto* s = c.signal.get_if<QCom::GsmSignalParams>()) return static_cast<double>(s->rxlev);
  return -999.0;
}

[[nodiscard]] std::string clip_field(std::string s, std::size_t width, bool right = false) {
  if (s.size() > width) s.resize(width);
  if (s.size() < width) {
    if (right) s.insert(0, width - s.size(), ' ');
    else s.append(width - s.size(), ' ');
  }
  return s;
}

[[nodiscard]] bool is_lte_full_row(const QCom::CellIdentity& c) noexcept {
  return c.rat == QCom::RatType::LTE && QCom::Utils::valid_lte_earfcn(c.radio.freq()) &&
         c.radio.pci_bsic() != 0 && c.passport.mcc != 0 &&
         QCom::Utils::valid_lte_eci(c.passport.cell_id) &&
         QCom::Utils::valid_lte_tac(c.passport.tac);
}

[[nodiscard]] std::vector<QCom::CellIdentity> filter_display_cells(
    const std::vector<QCom::CellIdentity>& cells) {
  std::vector<QCom::CellIdentity> rows;
  rows.reserve(cells.size());
  for (const auto& c : cells) {
    // Hide LTE EARFCN|0 / PCI=0 ghosts (SSS headers, B193 without cell rows).
    if (c.rat == QCom::RatType::LTE && c.radio.pci_bsic() == 0) continue;
    if (c.rat == QCom::RatType::LTE && !QCom::Utils::valid_lte_earfcn(c.radio.freq())) continue;
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
[[nodiscard]] std::string render_cells_table(const std::vector<QCom::CellIdentity>& cells,
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
    std::vector<std::size_t> fulls;                          // indices into rows
    std::map<uint32_t, std::vector<std::size_t>> neigh_by_earfcn;  // incomplete
    std::vector<std::size_t> site_orphans;                    // incomplete, other EARFCN
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
    if (c.is_serving) os << "* ";
    else if (as_neigh) os << "n ";
    else os << "  ";
    os << clip_field(fmt_band(c), 4) << ' ' << std::setw(5) << c.radio.freq() << '/'
       << std::setw(3) << c.radio.pci_bsic();
    const std::string mhz = fmt_mhz(c);
    if (mhz != "-") os << "  " << std::setw(6) << mhz << "MHz";
    const std::string bw = fmt_bw(c);
    if (bw != "-") os << "  bw" << bw;
    os << "  " << clip_field(fmt_plmn(c.passport), 7);
    if (is_lte_full_row(c)) {
      os << "  CID " << c.passport.cell_id << " (loc " << static_cast<unsigned>(c.passport.local_cell_id())
         << ")";
    } else if (c.passport.tac != 0 &&
               !(QCom::Utils::valid_lte_eci(c.passport.cell_id))) {
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
    if (c.is_serving) os << "* ";
    else os << "  ";
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

      const bool more_top =
          (ci + 1 < site.fulls.size()) || !site.site_orphans.empty() || !site.neigh_by_earfcn.empty();
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
          out << pad << (ni + 1 == vec.size() ? "└─ " : "├─ ")
              << fmt_cell_line(rows[vec[ni]], true) << '\n';
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

void print_cells_table(const std::vector<QCom::CellIdentity>& cells, const char* title) {
  std::cout << "\n" << render_cells_table(cells, title);
}

/// In-place TTY redraw so cell updates don't scroll as a sausage of duplicate tables.
class LiveDashboard {
public:
  explicit LiveDashboard(bool enable_inplace)
      : m_tty(enable_inplace && ::isatty(STDOUT_FILENO) != 0) {}

  void set_banner(std::string banner) {
    std::lock_guard lock(m_mu);
    m_banner = std::move(banner);
  }

  void note(std::string line) {
    std::lock_guard lock(m_mu);
    m_notes.push_back(std::move(line));
    if (m_notes.size() > 6) m_notes.erase(m_notes.begin());
    if (!m_table.empty()) paint_locked();
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
  }

private:
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
      for (const auto& n : m_notes) std::cout << n << "\n";
    } else {
      std::cout << "\n";
    }
    std::cout << std::flush;
  }

  std::mutex m_mu;
  bool m_tty{false};
  bool m_active{false};
  std::string m_banner;
  std::string m_table;
  std::vector<std::string> m_notes;
  int m_update_n{0};
};

/// Fallback names for codes that flew but have no registered parser (or for table hints).
[[nodiscard]] std::string_view known_code_hint(QCom::LogCode code) noexcept {
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
    case 0xB113: return "LTE LL1 PSS results";
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

void print_log_code_table(const std::map<QCom::LogCode, uint64_t>& hist,
                          const QCom::QualcomParser& parser) {
  std::vector<std::pair<QCom::LogCode, uint64_t>> ranked(hist.begin(), hist.end());
  std::sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.second > b.second; });

  const auto& stats = parser.code_stats();
  std::cout << "\nLog codes seen (all that flew):\n";
  std::cout << "┌──────────┬────────┬───────┬──────────┬──────────────────────────────────────────────┐\n"
            << "│   Code   │ Count  │ Supp. │  Parsed  │ Description                                  │\n"
            << "├──────────┼────────┼───────┼──────────┼──────────────────────────────────────────────┤\n";

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
              << std::right << " │ " << std::left << std::setw(44)
              << desc.str().substr(0, 44) << std::right << " │\n";
  }
  std::cout << "└──────────┴────────┴───────┴──────────┴──────────────────────────────────────────────┘\n";
}

std::string snapshot_fingerprint(const std::vector<QCom::CellIdentity>& cells) {
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

/// True when AT response contains a final result code on its own line.
[[nodiscard]] bool at_has_final_result(std::string_view out) noexcept {
  size_t i = 0;
  while (i < out.size()) {
    while (i < out.size() && (out[i] == '\r' || out[i] == '\n')) ++i;
    if (i >= out.size()) break;
    size_t e = i;
    while (e < out.size() && out[e] != '\r' && out[e] != '\n') ++e;
    const std::string_view line = out.substr(i, e - i);
    if (line == "OK" || line == "ERROR" || line.starts_with("+CME ERROR") ||
        line.starts_with("+CMS ERROR"))
      return true;
    i = e;
  }
  return false;
}

[[nodiscard]] std::string at_wire_cmd(const char* cmd) {
  std::string wire = cmd ? cmd : "";
  if (wire.empty() || wire.back() != '\n') wire += "\r\n";
  else if (wire.size() >= 2 && wire[wire.size() - 2] != '\r')
    wire.insert(wire.end() - 1, '\r');
  return wire;
}

/// Persistent AT port: one fd, serialized cmds, drain-before-write, line-final OK/ERROR.
/// Open/close-per-command used to drop URCs and race hop/QMI/cereg threads.
class AtSession {
public:
  using TickFn = std::function<void(int elapsed_ms, std::string_view partial)>;

  explicit AtSession(std::string path) : path_(std::move(path)) {}
  ~AtSession() { close_fd(); }
  AtSession(const AtSession&) = delete;
  AtSession& operator=(const AtSession&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  [[nodiscard]] std::optional<std::string> transact(const char* cmd, int timeout_ms = 2000,
                                                    TickFn tick = {}) {
    std::lock_guard lock(mu_);
    return transact_unlocked(cmd, timeout_ms, std::move(tick));
  }

  /// Drop and reopen tty (needed after AT+CFUN=1,1 soft reset).
  void reconnect() {
    std::lock_guard lock(mu_);
    close_fd();
    (void)ensure_open();
  }

  /// Best-effort write without waiting for OK (prefer @ref transact).
  bool write_raw(const char* cmd) {
    std::lock_guard lock(mu_);
    if (!ensure_open()) return false;
    drain_input();
    const std::string wire = at_wire_cmd(cmd);
    const ssize_t n = ::write(fd_, wire.data(), wire.size());
    if (n != static_cast<ssize_t>(wire.size())) {
      close_fd();
      return false;
    }
    ::tcdrain(fd_);
    return true;
  }

private:
  std::string path_;
  std::mutex mu_;
  int fd_{-1};
  bool echo_off_{false};

  void close_fd() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    echo_off_ = false;
  }

  bool ensure_open() {
    if (fd_ >= 0) return true;
    fd_ = ::open(path_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) return false;
    termios tio{};
    if (tcgetattr(fd_, &tio) == 0) {
      cfmakeraw(&tio);
      cfsetispeed(&tio, B115200);
      cfsetospeed(&tio, B115200);
      tio.c_cflag |= (CLOCAL | CREAD);
      tio.c_cc[VMIN] = 0;
      tio.c_cc[VTIME] = 0;
      tcsetattr(fd_, TCSANOW, &tio);
    }
    drain_input();
    if (!echo_off_) {
      // Quiet echo so parsers see clean OK lines; ignore failures on quirky firmwares.
      const std::string wire = at_wire_cmd("ATE0");
      (void)::write(fd_, wire.data(), wire.size());
      ::tcdrain(fd_);
      const auto until =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
      std::string junk;
      while (std::chrono::steady_clock::now() < until) {
        pollfd pfd{.fd = fd_, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, 50) <= 0) continue;
        char buf[256];
        const ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) junk.append(buf, static_cast<size_t>(n));
        if (at_has_final_result(junk)) break;
      }
      echo_off_ = true;
      drain_input();
    }
    return true;
  }

  void drain_input() {
    if (fd_ < 0) return;
    char junk[512];
    for (int i = 0; i < 64; ++i) {
      const ssize_t n = ::read(fd_, junk, sizeof(junk));
      if (n <= 0) break;
    }
  }

  [[nodiscard]] std::optional<std::string> transact_unlocked(const char* cmd, int timeout_ms,
                                                             TickFn tick) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!ensure_open()) return std::nullopt;
      drain_input();
      const std::string wire = at_wire_cmd(cmd);
      if (::write(fd_, wire.data(), wire.size()) != static_cast<ssize_t>(wire.size())) {
        close_fd();
        continue;
      }
      ::tcdrain(fd_);

      std::string out;
      const auto start = std::chrono::steady_clock::now();
      const auto deadline = start + std::chrono::milliseconds(std::max(200, timeout_ms));
      int last_tick_bucket = -1;
      while (std::chrono::steady_clock::now() < deadline) {
        pollfd pfd{.fd = fd_, .events = POLLIN, .revents = 0};
        const int pr = ::poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
          close_fd();
          break;
        }
        if (pr > 0 && (pfd.revents & POLLIN)) {
          char buf[1024];
          for (;;) {
            const ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n > 0) {
              out.append(buf, static_cast<size_t>(n));
              continue;
            }
            break;
          }
          if (at_has_final_result(out)) return out;
        }
        if (tick) {
          const int elapsed = static_cast<int>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count());
          const int bucket = elapsed / 1000;
          if (bucket != last_tick_bucket) {
            last_tick_bucket = bucket;
            tick(elapsed, out);
          }
        }
      }
      if (!out.empty()) return out;  // timeout with partial (COPS list mid-flight)
      close_fd();                    // retry with fresh fd
    }
    return std::nullopt;
  }
};

/// Legacy one-shot helpers (cleanup paths / when no session yet).
bool at_write(const std::string& path, const char* cmd) {
  AtSession s(path);
  return s.write_raw(cmd);
}

[[nodiscard]] std::optional<std::string> at_transact(const std::string& path, const char* cmd,
                                                     int timeout_ms = 2000) {
  AtSession s(path);
  return s.transact(cmd, timeout_ms);
}

struct CeregIdentity {
  uint32_t tac{0};
  uint32_t cell_id{0};
  bool ok{false};
};

[[nodiscard]] std::vector<std::string> split_csv_tokens(std::string_view line) {
  std::vector<std::string> toks;
  std::string cur;
  for (char c : line) {
    if (c == ',') {
      toks.push_back(cur);
      cur.clear();
    } else if (c != ' ' && c != '"') {
      cur.push_back(c);
    }
  }
  toks.push_back(cur);
  return toks;
}

[[nodiscard]] CeregIdentity parse_cereg_or_creg(std::string_view resp) {
  CeregIdentity out;
  auto pos = resp.find("+CEREG:");
  size_t prefix = 7;
  if (pos == std::string_view::npos) {
    pos = resp.find("+CREG:");
    prefix = 6;
  }
  if (pos == std::string_view::npos) return out;
  std::string line(resp.substr(pos + prefix));
  if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);

  auto toks = split_csv_tokens(line);
  if (toks.size() < 4) return out;
  try {
    out.tac = static_cast<uint32_t>(std::stoul(toks[2], nullptr, 16));
    out.cell_id = static_cast<uint32_t>(std::stoul(toks[3], nullptr, 16));
    out.ok = (out.tac != 0 && out.cell_id != 0);
  } catch (...) {
    out.ok = false;
  }
  return out;
}

/// SIMCOM AT+CPSI? LTE line — full serving passport + RF key in one shot.
/// Example: +CPSI: LTE,Online,250-20,0x4D07,200468759,468,EUTRAN-BAND7,3400,3,3,-69,-900,-661,20
struct CpsiServing {
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint32_t tac{0};
  uint32_t cell_id{0};
  uint16_t pci{0};
  uint32_t earfcn{0};
  uint8_t dl_bw_mhz{0};
  uint8_t ul_bw_mhz{0};
  float rsrp{-999.0f};
  float rsrq{-999.0f};
  bool ok{false};
};

[[nodiscard]] uint8_t cpsi_bw_index_to_mhz(int idx) noexcept {
  switch (idx) {
    case 0: return 1;   // 1.4
    case 1: return 3;
    case 2: return 5;
    case 3: return 10;
    case 4: return 15;
    case 5: return 20;
    default: return 0;
  }
}

[[nodiscard]] bool at_reply_ok(std::string_view resp) noexcept {
  return resp.find("ERROR") == std::string_view::npos &&
         (resp.find("\nOK") != std::string_view::npos || resp.find("\rOK") != std::string_view::npos ||
          resp.find("OK\n") != std::string_view::npos || resp.ends_with("OK") ||
          resp.find("\nOK\r") != std::string_view::npos);
}

/// AT+CNWINFO? LTE — EGCI (= MCC|MNC|ECI) + eNB. No PCI/EARFCN (needs serving key).
struct CnwServing {
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint32_t cell_id{0};
  bool ok{false};
};

[[nodiscard]] CnwServing parse_cnwinfo_lte(std::string_view resp) {
  CnwServing out;
  auto pos = resp.find("+CNWINFO:");
  if (pos == std::string_view::npos) return out;
  std::string line(resp.substr(pos + 9));
  if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);
  auto toks = split_csv_tokens(line);
  if (toks.size() < 2 || toks[0] != "LTE") return out;
  const std::string& egci = toks[1];
  if (egci.size() < 8) return out;
  try {
    const uint16_t mcc = static_cast<uint16_t>(std::stoul(egci.substr(0, 3)));
    // Prefer 2-digit MNC (RU/EU); fall back to 3-digit.
    for (int mnc_digits : {2, 3}) {
      if (egci.size() <= static_cast<size_t>(3 + mnc_digits)) continue;
      const uint16_t mnc =
          static_cast<uint16_t>(std::stoul(egci.substr(3, static_cast<size_t>(mnc_digits))));
      const uint64_t eci = std::stoull(egci.substr(static_cast<size_t>(3 + mnc_digits)));
      if (mcc < 100 || mcc > 999 || eci == 0 || eci > 0xFFFFFFFu) continue;
      out.mcc = mcc;
      out.mnc = mnc;
      out.cell_id = static_cast<uint32_t>(eci);
      out.ok = true;
      break;
    }
  } catch (...) {
    out.ok = false;
  }
  return out;
}

[[nodiscard]] CpsiServing parse_cpsi_lte(std::string_view resp) {
  // Modem may emit several +CPSI URC lines; pick the last valid LTE Online/Limited.
  CpsiServing best;
  for (std::size_t pos = 0; pos < resp.size();) {
    const auto at = resp.find("+CPSI:", pos);
    if (at == std::string_view::npos) break;
    pos = at + 6;
    std::string line(resp.substr(pos));
    if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);
    auto toks = split_csv_tokens(line);
    if (toks.size() < 8 || toks[0] != "LTE") continue;
    if (toks[1].find("Online") == std::string::npos &&
        toks[1].find("Limited") == std::string::npos)
      continue;
    CpsiServing out;
    try {
      std::string plmn = toks[2];
      auto dash = plmn.find('-');
      if (dash != std::string::npos) {
        out.mcc = static_cast<uint16_t>(std::stoul(plmn.substr(0, dash)));
        out.mnc = static_cast<uint16_t>(std::stoul(plmn.substr(dash + 1)));
      } else if (plmn.size() >= 5) {
        out.mcc = static_cast<uint16_t>(std::stoul(plmn.substr(0, 3)));
        out.mnc = static_cast<uint16_t>(std::stoul(plmn.substr(3)));
      } else {
        continue;
      }
      std::string tac_s = toks[3];
      if (tac_s.size() > 2 && tac_s[0] == '0' && (tac_s[1] == 'x' || tac_s[1] == 'X'))
        tac_s = tac_s.substr(2);
      out.tac = static_cast<uint32_t>(std::stoul(tac_s, nullptr, 16));
      out.cell_id = static_cast<uint32_t>(std::stoul(toks[4], nullptr, 10));
      out.pci = static_cast<uint16_t>(std::stoul(toks[5], nullptr, 10));
      out.earfcn = static_cast<uint32_t>(std::stoul(toks[7], nullptr, 10));
      if (toks.size() > 8) out.dl_bw_mhz = cpsi_bw_index_to_mhz(std::stoi(toks[8]));
      if (toks.size() > 9) out.ul_bw_mhz = cpsi_bw_index_to_mhz(std::stoi(toks[9]));
      if (toks.size() > 10) out.rsrq = static_cast<float>(std::stoi(toks[10])) / 10.0f;
      if (toks.size() > 11) out.rsrp = static_cast<float>(std::stoi(toks[11])) / 10.0f;
    } catch (...) {
      continue;
    }
    // Reject transitional garbage: PCI=0 / EARFCN=0xFFFFFFFF / BAND0 during reselection.
    out.ok = (out.earfcn > 0 && out.earfcn <= 262143 && out.pci >= 1 && out.pci <= 503 &&
              out.cell_id != 0 && out.tac != 0 && out.mcc >= 100);
    if (out.ok) best = out;
  }
  return best;
}

[[nodiscard]] bool cpsi_is_wcdma_or_noservice(std::string_view resp) noexcept {
  // Valid LTE passport present → still on LTE (ignore WCDMA URC noise).
  if (parse_cpsi_lte(resp).ok) return false;
  return resp.find("WCDMA") != std::string_view::npos ||
         resp.find("NO SERVICE") != std::string_view::npos ||
         resp.find("No service") != std::string_view::npos;
}

[[nodiscard]] std::optional<std::pair<uint16_t, uint16_t>> parse_cops_numeric_plmn(
    std::string_view resp) {
  auto pos = resp.find("+COPS:");
  if (pos == std::string_view::npos) return std::nullopt;
  auto q1 = resp.find('"', pos);
  if (q1 == std::string_view::npos) return std::nullopt;
  auto q2 = resp.find('"', q1 + 1);
  if (q2 == std::string_view::npos || q2 <= q1 + 1) return std::nullopt;
  std::string plmn(resp.substr(q1 + 1, q2 - q1 - 1));
  // Allow "250-20" or "25020"
  plmn.erase(std::remove(plmn.begin(), plmn.end(), '-'), plmn.end());
  if (plmn.size() < 5 || plmn.size() > 6) return std::nullopt;
  try {
    const uint16_t mcc = static_cast<uint16_t>(std::stoul(plmn.substr(0, 3)));
    const uint16_t mnc = static_cast<uint16_t>(std::stoul(plmn.substr(3)));
    if (mcc < 100 || mcc > 999) return std::nullopt;
    return std::make_pair(mcc, mnc);
  } catch (...) {
    return std::nullopt;
  }
}

/// All numeric PLMNs quoted in AT+COPS=? / AT+COPS? replies.
[[nodiscard]] std::vector<std::pair<uint16_t, uint16_t>> parse_cops_plmn_list(
    std::string_view resp) {
  std::set<std::pair<uint16_t, uint16_t>> uniq;
  for (std::size_t i = 0; i < resp.size(); ++i) {
    if (resp[i] != '"') continue;
    const auto j = resp.find('"', i + 1);
    if (j == std::string_view::npos) break;
    std::string tok(resp.substr(i + 1, j - i - 1));
    tok.erase(std::remove(tok.begin(), tok.end(), '-'), tok.end());
    i = j;
    if (tok.size() < 5 || tok.size() > 6) continue;
    bool digits = true;
    for (char c : tok) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        digits = false;
        break;
      }
    }
    if (!digits) continue;
    try {
      const uint16_t mcc = static_cast<uint16_t>(std::stoul(tok.substr(0, 3)));
      const uint16_t mnc = static_cast<uint16_t>(std::stoul(tok.substr(3)));
      if (mcc >= 100 && mcc <= 999) uniq.insert({mcc, mnc});
    } catch (...) {
    }
  }
  return {uniq.begin(), uniq.end()};
}

[[nodiscard]] std::string format_plmn_numeric(uint16_t mcc, uint16_t mnc) {
  char buf[16];
  if (mnc >= 100)
    std::snprintf(buf, sizeof(buf), "%03u%03u", static_cast<unsigned>(mcc),
                  static_cast<unsigned>(mnc));
  else
    std::snprintf(buf, sizeof(buf), "%03u%02u", static_cast<unsigned>(mcc),
                  static_cast<unsigned>(mnc));
  return buf;
}

/// EF_FPLMN (28539 / 0x6F7B) via AT+CRSM — empty when payload is 12×0xFF.
[[nodiscard]] bool crsm_fplmn_empty(std::string_view resp) noexcept {
  return resp.find("FFFFFFFFFFFFFFFFFFFFFFFF") != std::string_view::npos;
}

[[nodiscard]] bool crsm_sw_ok(std::string_view resp) noexcept {
  // Success SW1/SW2 usually 144,0 (0x9000) or 145,* (0x91xx).
  auto pos = resp.find("+CRSM:");
  if (pos == std::string_view::npos) return false;
  return resp.find("144,", pos) != std::string_view::npos ||
         resp.find("145,", pos) != std::string_view::npos;
}

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

[[nodiscard]] bool cell_is_full_lte(const QCom::CellIdentity& c) {
  return c.rat == QCom::RatType::LTE && QCom::Utils::valid_lte_eci(c.passport.cell_id) &&
         QCom::Utils::valid_lte_tac(c.passport.tac) && c.passport.mcc != 0 &&
         c.radio.pci_bsic() != 0 && QCom::Utils::valid_lte_earfcn(c.radio.freq());
}

/// Classic hop: incomplete RF only (skip any FULL).
[[nodiscard]] std::vector<HopTarget> pick_hop_targets(const std::vector<QCom::CellIdentity>& cells,
                                                      std::size_t max_n) {
  std::set<std::pair<uint32_t, uint16_t>> full_keys;
  for (const auto& c : cells) {
    if (cell_is_full_lte(c)) full_keys.insert({c.radio.freq(), c.radio.pci_bsic()});
  }

  std::map<std::pair<uint32_t, uint16_t>, HopTarget> best;
  auto consider = [&](uint32_t earfcn, uint16_t pci, float rsrp, uint16_t mcc, uint16_t mnc) {
    if (!QCom::Utils::valid_lte_earfcn(earfcn) || pci == 0 || pci > 503) return;
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
    if (c.rat != QCom::RatType::LTE) continue;
    const uint32_t earfcn = c.radio.freq();
    if (!QCom::Utils::valid_lte_earfcn(earfcn)) continue;
    if (!cell_is_full_lte(c)) {
      float rsrp = -160.0f;
      if (auto* s = c.signal.get_if<QCom::LteSignalParams>()) {
        if (QCom::Utils::valid_lte_rsrp(s->rsrp)) rsrp = s->rsrp;
      }
      consider(earfcn, c.radio.pci_bsic(), rsrp, c.passport.mcc, c.passport.mnc);
    }
    for (const auto& cf : c.radio.inter_freq_carriers) {
      if (!QCom::Utils::valid_lte_earfcn(cf.earfcn)) continue;
      for (uint16_t pci : cf.neigh_pcis)
        consider(cf.earfcn, pci, -155.0f, c.passport.mcc, c.passport.mnc);
    }
  }

  std::vector<HopTarget> ranked;
  ranked.reserve(best.size());
  for (const auto& [_, t] : best) ranked.push_back(t);
  std::sort(ranked.begin(), ranked.end(), [](const HopTarget& a, const HopTarget& b) {
    if (a.rsrp != b.rsrp) return a.rsrp > b.rsrp;
    if (a.earfcn != b.earfcn) return a.earfcn < b.earfcn;
    return a.pci < b.pci;
  });
  if (ranked.size() > max_n) ranked.resize(max_n);
  return ranked;
}

/// Full walk: every visible EARFCN|PCI that is not yet camped (incl. B0C2 FULL without camp).
/// Priority: new PLMN (0 camps) → incomplete → uncamped FULL → stronger RSRP.
[[nodiscard]] std::vector<HopTarget> pick_full_walk_targets(
    const std::vector<QCom::CellIdentity>& cells, std::size_t max_n) {
  std::map<std::pair<uint16_t, uint16_t>, int> camped_per_plmn;
  for (const auto& c : cells) {
    if (c.rat != QCom::RatType::LTE || !c.ever_serving || c.passport.mcc == 0) continue;
    ++camped_per_plmn[{c.passport.mcc, c.passport.mnc}];
  }

  std::map<std::pair<uint32_t, uint16_t>, HopTarget> best;
  auto upsert = [&](HopTarget t) {
    if (!QCom::Utils::valid_lte_earfcn(t.earfcn) || t.pci == 0 || t.pci > 503) return;
    if (t.camped) return;  // already walked / really camped
    t.plmn_camp_n =
        (t.mcc != 0) ? camped_per_plmn[{t.mcc, t.mnc}] : 0;
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
    if (c.rat != QCom::RatType::LTE) continue;
    const uint32_t earfcn = c.radio.freq();
    const uint16_t pci = c.radio.pci_bsic();
    if (!QCom::Utils::valid_lte_earfcn(earfcn) || pci == 0) continue;
    float rsrp = -160.0f;
    if (auto* s = c.signal.get_if<QCom::LteSignalParams>()) {
      if (QCom::Utils::valid_lte_rsrp(s->rsrp)) rsrp = s->rsrp;
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
      if (!QCom::Utils::valid_lte_earfcn(cf.earfcn)) continue;
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
  std::sort(ranked.begin(), ranked.end(), [](const HopTarget& a, const HopTarget& b) {
    // Prefer operators we have never camped.
    if (a.plmn_camp_n != b.plmn_camp_n) return a.plmn_camp_n < b.plmn_camp_n;
    // Incomplete before uncamped-FULL (need CID first).
    if (a.has_full != b.has_full) return !a.has_full && b.has_full;
    if (a.rsrp != b.rsrp) return a.rsrp > b.rsrp;
    if (a.earfcn != b.earfcn) return a.earfcn < b.earfcn;
    return a.pci < b.pci;
  });
  if (ranked.size() > max_n) ranked.resize(max_n);
  return ranked;
}

[[nodiscard]] std::string bands_csv_from_earfcns(const std::vector<HopTarget>& targets) {
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

[[nodiscard]] std::optional<std::string> parse_csyssel_lte_band_list(std::string_view resp) {
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

void print_usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0
      << " [--list] [--device PATH] [--qmi PATH] [--duration SEC] [--baud N]\n"
      << "                [--no-init] [--no-qmi] [--qmi-period MS] [--prefer-lte]\n"
      << "                [--plmn-search] [--no-plmn-search] [--search-period SEC]\n"
      << "                [--at-cops] [--deep-search] [--at-cereg]\n"
      << "                [--earfcn-hop] [--hop-dwell SEC] [--hop-max N]\n"
      << "                [--hop-lock|--hop-cfun] [--hop-band-clip]\n"
      << "                [--no-cops-between-hops] [--cops-between-hops]\n"
      << "                [--full-walk] [--no-full-walk]\n"
      << "                [--clear-fplmn] [--no-clear-fplmn]\n"
      << "  --list           enumerate modems via DeviceCatalog and exit\n"
      << "  --device         DIAG tty (default: first catalog diag path)\n"
      << "  --qmi            QMI cdc-wdm path (default: catalog)\n"
      << "  --duration       run seconds then stop (default: 30; with --earfcn-hop → 0;\n"
      << "                   0 = until Ctrl+C)\n"
      << "  --baud           DIAG serial baud (default: 921600)\n"
      << "  --no-init        skip DiagSession mask init (listen only)\n"
      << "  --no-qmi         DIAG only\n"
      << "  --qmi-period     NAS poll period ms (default: 2000)\n"
      << "  --prefer-lte      QMI SSP → LTE(+WCDMA); with --earfcn-hop → LTE-only while hopping\n"
      << "  --plmn-search    periodic QMI force-search (default OFF); use after LTE camp\n"
      << "  --no-plmn-search disable periodic QMI search\n"
      << "  --search-period  seconds between force-search / between-hop COPS=? (default: 60)\n"
      << "  --at-cops        periodic AT+COPS=? (no hop). With --earfcn-hop → between-hop mode\n"
      << "  --deep-search    once: AT+COPS=2, wait, then enable --plmn-search + --at-cops\n"
      << "  --at-cereg       poll AT+CPSI?/CNWINFO for serving FULL (EARFCN|PCI|TAC|CID)\n"
      << "  --earfcn-hop     sticky CCELLCFG grind: hold lock until target FULL (SIB1/CPSI/B0C2)\n"
      << "  --hop-lock       alias for default CCELLCFG grind\n"
      << "  --hop-cfun       CFUN 4↔1 bounce instead of CCELLCFG (less precise; USB risk)\n"
      << "  --recover-cfun   allow CFUN in LTE recover (default OFF — CFUN re-enums USB)\n"
      << "  --hop-dwell      max seconds to hold each lock (default: 30; CFUN min 8)\n"
      << "  --hop-max        max incomplete EARFCN|PCI candidates ranked per pass (default: 12)\n"
      << "  --hop-band-clip  narrow AT+CSYSSEL lte_band during hop (risky)\n"
      << "  --cops-between-hops     AT+COPS=? only between hop locks (default OFF)\n"
      << "  --no-cops-between-hops  disable between-hop COPS=?\n"
      << "  --full-walk            walk all visible EARFCN|PCI incl. uncamped FULL;\n"
      << "                         PLMN-select (COPS=1,2) for foreign operators (default ON with hop)\n"
      << "  --no-full-walk         classic hop: incomplete only, no PLMN-select\n"
      << "  --clear-fplmn          clear SIM FPLMN via AT+CRSM at hop start (default ON with full-walk)\n"
      << "  --no-clear-fplmn       do not touch EF_FPLMN\n"
      << "  --live-json [P]  survey JSON path for tower_gui (default ON:\n"
      << "                   /tmp/qcom_live_towers.json)\n"
      << "  --no-live-json   disable live JSON export\n"
      << "\n"
      << "  Neighbor FULL needs camp on that EARFCN|PCI. Grind holds CCELLCFG until match.\n"
      << "  Bn = 3GPP E-UTRA band (B8=900 MHz), not 'byte n'.\n";
}

std::optional<SelectedModem> resolve_modem(const std::optional<std::string>& diag_override,
                                           const std::optional<std::string>& qmi_override) {
  qmi_observer::device::DeviceCatalog catalog;
  auto refreshed = catalog.refresh();
  if (!refreshed) {
    std::cerr << "catalog refresh failed: " << refreshed.error().message << "\n";
    return std::nullopt;
  }

  SelectedModem out;
  for (const auto& ep : catalog.endpoints()) {
    auto diag = diag_override ? diag_override : ep.preferred_diag_path();
    if (!diag || diag->empty()) continue;
    out.id = ep.id;
    out.diag = *diag;
    out.at = ep.preferred_at_path();
    if (qmi_override && !qmi_override->empty()) {
      out.qmi = *qmi_override;
    } else {
      out.qmi = ep.qmi_path();
    }
    std::cout << "Selected modem " << ep.id << " diag=" << out.diag;
    if (out.at) std::cout << " at=" << *out.at;
    if (out.qmi) std::cout << " qmi=" << *out.qmi;
    std::cout << "\n";
    return out;
  }

  if (diag_override && !diag_override->empty()) {
    out.diag = *diag_override;
    out.qmi = qmi_override;
    out.id = "manual";
    return out;
  }

  std::cerr << "No modem with Diag port found. Pass --device /dev/ttyUSBx\n";
  return std::nullopt;
}

int list_modems() {
  qmi_observer::device::DeviceCatalog catalog;
  auto refreshed = catalog.refresh();
  if (!refreshed) {
    std::cerr << "catalog refresh failed: " << refreshed.error().message << "\n";
    return 1;
  }

  const auto& eps = catalog.endpoints();
  std::cout << "Found " << eps.size() << " modem(s):\n";
  for (const auto& ep : eps) {
    std::cout << "  " << ep.id << "  " << ep.product;
    if (!ep.matched_profile_id.empty()) std::cout << "  profile=" << ep.matched_profile_id;
    std::cout << "\n";
    for (const auto& p : ep.ports) {
      std::cout << "    " << qmi_observer::device::to_string(p.role) << "  " << p.path << "\n";
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::optional<std::string> device;
  std::optional<std::string> qmi_path;
  int duration_sec = 30;
  bool duration_explicit = false;
  int baud = 921600;
  int qmi_period_ms = 2000;
  int search_period_sec = 60;
  int hop_dwell_sec = 30;
  int hop_max = 12;
  bool list_only = false;
  bool init_masks = true;
  bool use_qmi = true;
  bool prefer_lte = false;
  bool plmn_search = false;
  bool at_cops = false;
  bool deep_search = false;
  bool at_cereg = false;
  bool earfcn_hop = false;
  bool hop_cfun = false;  // false → CCELLCFG lock (default for --earfcn-hop)
  bool hop_band_clip = false;
  bool recover_cfun = false;  // CFUN bounce in LTE recover (USB re-enum risk)
  // unset → auto ON with --earfcn-hop; explicit flags override
  std::optional<bool> cops_between_hops_flag;
  std::optional<bool> full_walk_flag;
  std::optional<bool> clear_fplmn_flag;
  // Default ON so tower_gui Live Scan works without remembering the flag.
  std::optional<std::string> live_json_path = std::string{"/tmp/qcom_live_towers.json"};

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto need = [&](const char* flag) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "Missing value for " << flag << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    auto optional_path = [&](const char* def) -> std::string {
      if (i + 1 < argc && argv[i + 1][0] != '-') return argv[++i];
      return def;
    };
    if (arg == "--list") {
      list_only = true;
    } else if (arg == "--device") {
      device = need("--device");
    } else if (arg == "--qmi") {
      qmi_path = need("--qmi");
    } else if (arg == "--duration") {
      duration_sec = std::atoi(need("--duration"));
      duration_explicit = true;
    } else if (arg == "--baud") {
      baud = std::atoi(need("--baud"));
    } else if (arg == "--qmi-period") {
      qmi_period_ms = std::atoi(need("--qmi-period"));
    } else if (arg == "--search-period") {
      search_period_sec = std::atoi(need("--search-period"));
    } else if (arg == "--hop-dwell") {
      hop_dwell_sec = std::atoi(need("--hop-dwell"));
    } else if (arg == "--hop-max") {
      hop_max = std::atoi(need("--hop-max"));
    } else if (arg == "--no-init") {
      init_masks = false;
    } else if (arg == "--no-qmi") {
      use_qmi = false;
    } else if (arg == "--prefer-lte") {
      prefer_lte = true;
    } else if (arg == "--plmn-search") {
      plmn_search = true;
    } else if (arg == "--no-plmn-search") {
      plmn_search = false;
    } else if (arg == "--at-cops") {
      at_cops = true;
    } else if (arg == "--at-cereg") {
      at_cereg = true;
    } else if (arg == "--earfcn-hop") {
      earfcn_hop = true;
      at_cereg = true;
      // Do NOT auto-enable at_cops/plmn_search — COPS=? floods AT and strands LTE→WCDMA.
    } else if (arg == "--hop-lock") {
      hop_cfun = false;
      earfcn_hop = true;
      at_cereg = true;
    } else if (arg == "--hop-cfun") {
      hop_cfun = true;
      earfcn_hop = true;
      at_cereg = true;
    } else if (arg == "--hop-band-clip") {
      hop_band_clip = true;
    } else if (arg == "--recover-cfun") {
      recover_cfun = true;
    } else if (arg == "--cops-between-hops") {
      cops_between_hops_flag = true;
    } else if (arg == "--no-cops-between-hops") {
      cops_between_hops_flag = false;
    } else if (arg == "--full-walk") {
      full_walk_flag = true;
    } else if (arg == "--no-full-walk") {
      full_walk_flag = false;
    } else if (arg == "--clear-fplmn") {
      clear_fplmn_flag = true;
    } else if (arg == "--no-clear-fplmn") {
      clear_fplmn_flag = false;
    } else if (arg == "--live-json") {
      live_json_path = optional_path("/tmp/qcom_live_towers.json");
    } else if (arg == "--no-live-json") {
      live_json_path = std::nullopt;
    } else if (arg == "--deep-search") {
      deep_search = true;
      at_cops = true;
      plmn_search = true;
    } else if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    } else {
      std::cerr << "Unknown arg: " << arg << "\n";
      print_usage(argv[0]);
      return 2;
    }
  }

  if (list_only) return list_modems();

  const bool hop_lock = earfcn_hop && !hop_cfun;
  // Periodic AT+COPS=? fights CCELLCFG/CPSI on the same AT tty — remap to between-hop.
  bool cops_between_hops = false;
  if (earfcn_hop) {
    if (cops_between_hops_flag.has_value()) {
      cops_between_hops = *cops_between_hops_flag;
    } else {
      // Default OFF — COPS=? + CFUN churn re-enumerates USB on SIM8300.
      cops_between_hops = false;
    }
    if (at_cops) {
      std::cerr << "note: --at-cops during --earfcn-hop → between-hop COPS=? "
                   "(not periodic AT flood)\n";
      at_cops = false;
      cops_between_hops = true;
    }
    // Default 30s kills LTE recover mid-flight (WCDMA→NO SERVICE). Hop needs Ctrl+C.
    if (!duration_explicit) {
      duration_sec = 0;
      std::cerr << "note: --earfcn-hop → --duration 0 (until Ctrl+C); pass --duration N to cap\n";
    } else if (duration_sec > 0 && duration_sec < 180) {
      std::cerr << "warning: --duration " << duration_sec
                << "s is short for hop/recover (often needs 60–120s to leave WCDMA)\n";
    }
  } else if (cops_between_hops_flag.value_or(false)) {
    std::cerr << "note: --cops-between-hops needs --earfcn-hop; enabling periodic --at-cops\n";
    at_cops = true;
  }

  const bool full_walk =
      earfcn_hop && (full_walk_flag.has_value() ? *full_walk_flag : true);
  const bool clear_fplmn =
      earfcn_hop &&
      (clear_fplmn_flag.has_value() ? *clear_fplmn_flag : full_walk);
  if (earfcn_hop && full_walk) {
    std::cout << "full-walk ON: hop uncamped FULL + PLMN-select for foreign operators\n";
  }
  if (earfcn_hop && clear_fplmn) {
    std::cout << "clear-fplmn ON: AT+CRSM wipe EF_FPLMN at hop start\n";
  }

  auto modem = resolve_modem(device, qmi_path);
  if (!modem) return 1;

  QCom::DiagSourceConfig cfg{
      .device_path = modem->diag,
      .baud_rate = baud,
      .init_masks = init_masks,
  };

  QCom::RadioScannerEngine engine(QCom::make_diag_source(std::move(cfg)));
  std::atomic<int> updates{0};
  std::atomic<int> cereg_ok{0};
  std::atomic<int> cpsi_ok{0};
  std::atomic<int> cnw_ok{0};
  std::atomic<int> hop_kicks{0};
  std::atomic<int> hop_locks{0};
  std::atomic<int> hop_fulls{0};
  std::atomic<int> cops_between_kicks{0};
  std::atomic<int> qmi_hop_snaps{0};
  std::mutex qmi_status_mu;
  qmi_observer::NasRadioStatus qmi_status{};
  std::mutex code_mu;
  std::map<QCom::LogCode, uint64_t> code_hist;
  std::map<int, uint64_t> b0c0_pdu_hist;
  std::map<int, uint64_t> b0c0_ver_hist;
  LiveDashboard dash(/*enable_inplace=*/true);
  std::mutex fp_mu;
  std::string last_fp;
  std::mutex live_json_mu;
  auto last_live_json_write = std::chrono::steady_clock::now() - std::chrono::seconds(10);

  auto write_live_json = [&](bool force = false) {
    if (!live_json_path) return;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(live_json_mu);
    if (!force && now - last_live_json_write < std::chrono::milliseconds(500)) return;
    last_live_json_write = now;
    std::map<std::string, std::string> extra;
    extra["updates"] = std::to_string(updates.load());
    extra["hop_kicks"] = std::to_string(hop_kicks.load());
    extra["hop_locks"] = std::to_string(hop_locks.load());
    extra["hop_fulls"] = std::to_string(hop_fulls.load());
    extra["hop_cops"] = std::to_string(cops_between_kicks.load());
    extra["cpsi_ok"] = std::to_string(cpsi_ok.load());
    extra["qmi_hop_snaps"] = std::to_string(qmi_hop_snaps.load());
    {
      std::lock_guard qlock(qmi_status_mu);
      if (!qmi_status.registration.empty()) extra["qmi_reg"] = qmi_status.registration;
      if (!qmi_status.ps_attach.empty()) extra["qmi_ps"] = qmi_status.ps_attach;
      if (!qmi_status.radio.empty()) extra["qmi_radio"] = qmi_status.radio;
      if (qmi_status.plmn) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%03u-%02u", static_cast<unsigned>(qmi_status.plmn->mcc),
                      static_cast<unsigned>(qmi_status.plmn->mnc));
        extra["qmi_plmn"] = buf;
      }
      if (!qmi_status.plmn_name.empty()) extra["qmi_plmn_name"] = qmi_status.plmn_name;
      if (qmi_status.roaming_indicator)
        extra["qmi_roam"] = std::to_string(*qmi_status.roaming_indicator);
      if (qmi_status.lte_rsrp_dbm)
        extra["qmi_rsrp"] = std::to_string(static_cast<int>(*qmi_status.lte_rsrp_dbm));
      if (qmi_status.lte_rsrq_db)
        extra["qmi_rsrq"] = std::to_string(static_cast<int>(*qmi_status.lte_rsrq_db));
      if (qmi_status.lte_snr_db) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f", *qmi_status.lte_snr_db);
        extra["qmi_snr"] = buf;
      }
    }
    (void)QCom::Tools::write_towers_json_survey(*live_json_path, engine.tracker().get_snapshot(),
                                                "live_scanner", extra);
  };

  if (live_json_path) {
    std::cout << "Live JSON → " << *live_json_path << " (tower_gui Live Scan)\n";
  }

  auto on_cells = [&](const std::vector<QCom::CellIdentity>& cells) {
    if (cells.empty()) return;
    const std::string fp = snapshot_fingerprint(cells);
    std::lock_guard lock(fp_mu);
    if (fp == last_fp) return;
    last_fp = fp;
    const int n = ++updates;
    dash.show_cells(cells, n);
    write_live_json(false);
  };

  engine.set_callback(on_cells);
  engine.set_packet_observer([&](QCom::QualcommPacketView pkt) {
    std::lock_guard lock(code_mu);
    ++code_hist[pkt.log_code];
    if (pkt.log_code == 0xB0C0) {
      if (auto ota = QCom::Lte::decode_lte_rrc_ota(pkt.payload)) {
        ++b0c0_pdu_hist[static_cast<int>(ota->pdu_num)];
        ++b0c0_ver_hist[static_cast<int>(ota->version)];
      } else {
        ++b0c0_pdu_hist[-1];
      }
    }
  });
  std::atomic<bool> diag_alive{true};
  std::atomic<bool> diag_needs_reconnect{false};
  std::atomic<int> diag_reconnects{0};
  engine.set_disconnect_callback([&] {
    // Runs on DIAG worker thread — never stop()/join here (deadlock).
    // Only fired after in-place revive failed (real unplug / prolonged death).
    diag_alive.store(false, std::memory_order_relaxed);
    diag_needs_reconnect.store(true, std::memory_order_relaxed);
    dash.note("[diag] link dead after revive attempts — full reconnect…");
  });

  std::signal(SIGINT, live_scanner_on_signal);
  std::signal(SIGTERM, live_scanner_on_signal);

  std::cout << "Starting " << (engine.source() ? engine.source()->name() : "?") << " on "
            << modem->diag << " …\n";
  if (!engine.start()) {
    std::cerr << "Failed to start DIAG source on " << modem->diag << "\n";
    if (auto* linux = dynamic_cast<QCom::LinuxSource*>(engine.source())) {
      if (!linux->last_error().empty()) std::cerr << "  reason: " << linux->last_error() << "\n";
    }
    std::cerr << "  tips: ls -l " << modem->diag
              << " ; groups | grep dialout ; fuser " << modem->diag
              << "\n  after USB re-plug wait 1–2s, or: sudo chmod a+rw " << modem->diag
              << "\n";
    if (modem->qmi) {
      std::cerr << "  QMI perms (often root:root after re-enum): sudo chmod a+rw " << *modem->qmi
                << "\n";
    }
    return 1;
  }
  if (auto* linux = dynamic_cast<QCom::LinuxSource*>(engine.source())) {
    if (!linux->init_ok()) {
      std::cerr << "Warning: mask init failed (" << linux->last_error()
                << ") — listening anyway\n";
    } else if (init_masks) {
      std::cout << "Diag masks init OK\n";
    }
  }

  std::unique_ptr<qmi_observer::Session> qmi_session;
  std::atomic<bool> qmi_stop{false};
  std::thread qmi_thread;
  std::atomic<int> qmi_polls{0};
  std::atomic<int> qmi_ok{0};
  std::atomic<int> search_kicks{0};
  std::atomic<bool> deep_dereg_done{false};
  // One persistent AT session for hop/QMI/cereg/CPSI — open/close-per-cmd raced the tty.
  std::unique_ptr<AtSession> at_sess;
  if (modem->at) at_sess = std::make_unique<AtSession>(*modem->at);
  auto at_do = [&](const char* cmd, int timeout_ms = 2000,
                   AtSession::TickFn tick = {}) -> std::optional<std::string> {
    if (!at_sess) return std::nullopt;
    return at_sess->transact(cmd, timeout_ms, std::move(tick));
  };

  if (use_qmi && modem->qmi) {
    qmi_observer::Settings qs;
    qs.device_path = *modem->qmi;
    qs.use_proxy = false;
    qs.allow_dms_offline = false;
    qs.request_timeout = std::chrono::seconds(8);
    qmi_session = std::make_unique<qmi_observer::Session>(std::move(qs));
    if (auto opened = qmi_session->open(); !opened) {
      std::cerr << "QMI open failed: " << opened.error().message << " — continuing DIAG+AT only\n";
      std::cerr << "  Serving CGI still comes from AT+CPSI?; QMI adds neighbour GCI when open.\n";
      std::cerr << "  Fix perms: sudo chmod a+rw " << *modem->qmi
                << "   (often root:root after plug)\n";
      qmi_session.reset();
    } else {
      if (prefer_lte || hop_lock) {
        // During CCELLCFG hop keep WCDMA out of the way; otherwise LTE+WCDMA.
        const std::vector<qmi_observer::Rat> modes =
            hop_lock ? std::vector{qmi_observer::Rat::Lte}
                     : std::vector{qmi_observer::Rat::Lte, qmi_observer::Rat::Wcdma};
        auto set = qmi_session->control().set_mode_preference(modes);
        if (!set) {
          std::cerr << "QMI mode preference failed: " << set.error().message << "\n";
        } else {
          std::cout << "QMI mode preference set to " << (hop_lock ? "LTE-only (hop)" : "LTE+WCDMA")
                    << "\n";
        }
      }
      if (plmn_search) {
        std::cout << "PLMN search every " << search_period_sec
                  << "s (QMI force-search → B0C2/SIB1 during sweep)\n";
      } else {
        std::cout << "QMI PLMN search OFF (default)\n";
      }
      if (at_cops && modem->at) {
        std::cout << "AT+COPS=? + QMI force-search every " << search_period_sec
                  << "s on " << *modem->at << " (first kick ~5s)\n";
      } else if (at_cops) {
        std::cout << "AT+COPS=? requested but no AT path in catalog\n";
      }
      std::cout << "QMI NAS poll every " << qmi_period_ms << "ms on " << *modem->qmi << "\n";
      // Undo leftover search/deregister from prior --deep-search runs.
      if (at_sess) {
        if (auto rsp = at_do("AT+COPS=0", 4000); rsp && at_reply_ok(*rsp)) {
          std::cout << "AT+COPS=0 (auto select) on " << at_sess->path() << "\n";
        } else {
          std::cerr << "AT+COPS=0 failed on " << at_sess->path() << "\n";
        }
      }
      qmi_thread = std::thread([&] {
        // Seed last_search to "now" so period does not fire before the 5s camp gate.
        auto last_search = std::chrono::steady_clock::now();
        const auto started = std::chrono::steady_clock::now();
        bool first_cops_gate = false;
        while (!qmi_stop.load(std::memory_order_relaxed)) {
          ++qmi_polls;
          auto snap = qmi_session->nas().snapshot_cells();
          if (snap) {
            ++qmi_ok;
            auto envs = qmi_observer::to_rrc_envelopes(snap.value());
            if (!envs.empty()) engine.inject_envelopes(std::move(envs));
          }
          if (auto st = qmi_session->nas().snapshot_status(); st) {
            std::lock_guard qlock(qmi_status_mu);
            qmi_status = std::move(st.value());
          }

          const auto now = std::chrono::steady_clock::now();
          const bool want_force = plmn_search || at_cops;
          const bool warmed = now - started >= std::chrono::seconds(5);
          const bool period_ok =
              warmed &&
              now - last_search >= std::chrono::seconds(std::max(5, search_period_sec));
          const bool first_ok = warmed && !first_cops_gate;
          if (want_force && (period_ok || first_ok)) {
            last_search = now;
            first_cops_gate = true;

            if (auto fs = qmi_session->control().force_network_search(); fs) {
              ++search_kicks;
              dash.note("[ota-kick] QMI force-search ok (#" + std::to_string(search_kicks.load()) +
                        ")");
            } else {
              dash.note("[ota-kick] QMI force-search failed: " + fs.error().message);
            }

            if (at_cops && modem->at) {
              if (deep_search && !deep_dereg_done.exchange(true)) {
                if (auto rsp = at_do("AT+COPS=2", 8000); rsp && at_reply_ok(*rsp)) {
                  dash.note("[deep-search] AT+COPS=2 once (deregister)");
                } else {
                  dash.note("[deep-search] AT+COPS=2 failed");
                }
                std::this_thread::sleep_for(std::chrono::seconds(3));
              }
              // Full drain; DIAG keeps running on its own thread (aggressive read_loop).
              if (at_do("AT+COPS=?", 180000)) {
                dash.note(std::string("[at-cops] AT+COPS=? done on ") +
                          (at_sess ? at_sess->path() : "?"));
              } else {
                dash.note("[at-cops] AT+COPS=? timeout/fail");
              }
            }
          }

          for (int i = 0; i < qmi_period_ms / 50 && !qmi_stop.load(std::memory_order_relaxed);
               ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
        }
      });
    }
  } else if (use_qmi) {
    std::cerr << "No QMI path — DIAG only (--qmi /dev/cdc-wdm0 to force)\n";
  }

  // AT-only COPS when QMI missing but user asked for sweep (periodic).
  std::thread at_only_thread;
  std::atomic<bool> at_only_stop{false};
  if (at_cops && at_sess && !qmi_session) {
    if (deep_search && !deep_dereg_done.exchange(true)) {
      if (auto rsp = at_do("AT+COPS=2", 8000); rsp && at_reply_ok(*rsp)) {
        dash.note("[deep-search] AT+COPS=2 once (deregister, no QMI)");
      } else {
        dash.note("[deep-search] AT+COPS=2 failed");
      }
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    at_only_thread = std::thread([&] {
      const auto started = std::chrono::steady_clock::now();
      auto last = started;
      bool first = false;
      while (!at_only_stop.load(std::memory_order_relaxed)) {
        const auto now = std::chrono::steady_clock::now();
        const bool warmed = now - started >= std::chrono::seconds(5);
        const bool due =
            warmed && now - last >= std::chrono::seconds(std::max(5, search_period_sec));
        const bool gate = warmed && !first;
        if (due || gate) {
          last = now;
          first = true;
          if (at_do("AT+COPS=?", 180000)) {
            ++search_kicks;
            dash.note("[at-cops] AT+COPS=? done (no QMI, #" +
                      std::to_string(search_kicks.load()) + ")");
          } else {
            dash.note("[at-cops] AT+COPS=? timeout/fail");
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
    });
  }

  auto diag_serving_key = [&]() -> QCom::LocalCellKey {
    for (const auto& c : engine.tracker().get_snapshot()) {
      if (c.rat == QCom::RatType::LTE && c.is_serving && c.radio.freq() != 0 &&
          c.radio.pci_bsic() != 0 && c.radio.pci_bsic() <= 503) {
        return {.freq = c.radio.freq(), .pci_bsic = c.radio.pci_bsic()};
      }
    }
    return {};
  };

  enum class ServingStamp : uint8_t { None = 0, Soft = 1, Full = 2 };

  /// Full serving passport ONLY when we have EARFCN|PCI from CPSI or QMI.
  /// CEREG alone has TAC/CID without PCI — stamping it onto "strongest" minted fake FULL.
  auto inject_serving_identity_once = [&]() -> ServingStamp {
    if (!at_sess && !qmi_session) return ServingStamp::None;

    // 1) SIMCOM AT+CPSI? — authoritative physical key + ECI.
    if (at_sess) {
      if (auto cpsi_raw = at_do("AT+CPSI?", 1500)) {
        auto cpsi = parse_cpsi_lte(*cpsi_raw);
        if (cpsi.ok) {
          QCom::LocalCellKey key{.freq = cpsi.earfcn, .pci_bsic = cpsi.pci};
          QCom::CellPassport pass;
          pass.mcc = cpsi.mcc;
          pass.mnc = cpsi.mnc;
          pass.tac = cpsi.tac;
          pass.cell_id = cpsi.cell_id;

          QCom::Events::RadioParamsEvent<QCom::LteRadioParams> radio;
          radio.data.earfcn = cpsi.earfcn;
          radio.data.pci = cpsi.pci;
          radio.data.dl_bw = cpsi.dl_bw_mhz;
          radio.data.ul_bw = cpsi.ul_bw_mhz;

          std::vector<QCom::Events::RrcEventEnvelope> envs;
          envs.push_back(QCom::Events::RrcEventEnvelope{
              .key = key,
              .rat = QCom::RatType::LTE,
              .event_data = QCom::Events::RrcEvent{std::move(radio)},
          });
          envs.push_back(QCom::Events::RrcEventEnvelope{
              .key = key,
              .rat = QCom::RatType::LTE,
              .event_data = QCom::Events::PassportEvent{.passport = pass},
          });
          if (QCom::Utils::valid_lte_rsrp(cpsi.rsrp)) {
            QCom::CellSignal sig;
            QCom::LteSignalParams lp;
            lp.rsrp = cpsi.rsrp;
            if (cpsi.rsrq > -30.0f && cpsi.rsrq <= -1.0f) lp.rsrq = cpsi.rsrq;
            sig.signal_data = lp;
            envs.push_back(QCom::Events::RrcEventEnvelope{
                .key = key,
                .rat = QCom::RatType::LTE,
                .event_data = QCom::Events::SignalUpdateEvent{.signal = std::move(sig)},
            });
          }
          envs.push_back(QCom::Events::RrcEventEnvelope{
              .key = key,
              .rat = QCom::RatType::LTE,
              .event_data = QCom::Events::ServingChangedEvent{.is_serving = true},
          });
          engine.inject_envelopes(std::move(envs));
          ++cpsi_ok;
          ++cereg_ok;
          return ServingStamp::Full;
        }
      }
    }

    // 2) QMI serving with EARFCN|PCI (+ optional GCI) — safe physical key.
    if (qmi_session) {
      if (auto snap = qmi_session->nas().snapshot_cells(); snap) {
        for (const auto& c : snap.value().cells) {
          if (c.rat != qmi_observer::Rat::Lte || !c.serving) continue;
          if (!c.rf_channel || !c.phy_id || *c.rf_channel == 0 || *c.phy_id > 503) continue;
          QCom::LocalCellKey key{.freq = *c.rf_channel, .pci_bsic = *c.phy_id};
          QCom::CellPassport pass;
          if (c.plmn) {
            pass.mcc = c.plmn->mcc;
            pass.mnc = c.plmn->mnc;
          }
          if (c.lac_or_tac && QCom::Utils::valid_lte_tac(*c.lac_or_tac)) pass.tac = *c.lac_or_tac;
          if (c.cell_id && QCom::Utils::valid_lte_eci(*c.cell_id))
            pass.cell_id = static_cast<uint32_t>(*c.cell_id);
          if (!pass.has_identity() && pass.mcc == 0) continue;

          std::vector<QCom::Events::RrcEventEnvelope> envs;
          QCom::Events::RadioParamsEvent<QCom::LteRadioParams> radio;
          radio.data.earfcn = key.freq;
          radio.data.pci = key.pci_bsic;
          envs.push_back(QCom::Events::RrcEventEnvelope{
              .key = key,
              .rat = QCom::RatType::LTE,
              .event_data = QCom::Events::RrcEvent{std::move(radio)},
          });
          envs.push_back(QCom::Events::RrcEventEnvelope{
              .key = key,
              .rat = QCom::RatType::LTE,
              .event_data = QCom::Events::PassportEvent{.passport = pass},
          });
          envs.push_back(QCom::Events::RrcEventEnvelope{
              .key = key,
              .rat = QCom::RatType::LTE,
              .event_data = QCom::Events::ServingChangedEvent{.is_serving = true},
          });
          engine.inject_envelopes(std::move(envs));
          ++cereg_ok;
          return pass.has_identity() && pass.tac != 0 ? ServingStamp::Full : ServingStamp::Soft;
        }
      }
    }

    // 3) AT+CNWINFO EGCI onto DIAG serving key (has CID, no PCI — needs existing key).
    if (at_sess) {
      QCom::LocalCellKey key = diag_serving_key();
      if (key.freq != 0) {
        if (auto cnw_raw = at_do("AT+CNWINFO?", 1500)) {
          auto cnw = parse_cnwinfo_lte(*cnw_raw);
          if (cnw.ok) {
            QCom::CellPassport pass;
            pass.mcc = cnw.mcc;
            pass.mnc = cnw.mnc;
            pass.cell_id = cnw.cell_id;
            engine.inject_envelopes({QCom::Events::RrcEventEnvelope{
                .key = key,
                .rat = QCom::RatType::LTE,
                .event_data = QCom::Events::PassportEvent{.passport = pass},
            }});
            ++cnw_ok;
            ++cereg_ok;
            return ServingStamp::Soft;
          }
        }
      }
    }

    // 4) COPS: PLMN only on DIAG-marked serving — NEVER TAC/CID without PCI.
    if (at_sess) {
      auto cops = at_do("AT+COPS?", 1500);
      if (!cops) return ServingStamp::None;
      auto plmn = parse_cops_numeric_plmn(*cops);
      if (!plmn) return ServingStamp::None;
      QCom::LocalCellKey key = diag_serving_key();
      if (key.freq == 0) return ServingStamp::None;
      QCom::CellPassport pass;
      pass.mcc = plmn->first;
      pass.mnc = plmn->second;
      engine.inject_envelopes({QCom::Events::RrcEventEnvelope{
          .key = key,
          .rat = QCom::RatType::LTE,
          .event_data = QCom::Events::PassportEvent{.passport = pass},
      }});
      return ServingStamp::Soft;
    }
    return ServingStamp::None;
  };

  std::thread cereg_thread;
  std::atomic<bool> cereg_stop{false};
  if (at_cereg && at_sess) {
    (void)at_do("AT+CEREG=2", 2000);
    (void)at_do("AT+CREG=2", 2000);
    (void)at_do("AT+COPS=3,2", 2000);
    std::cout << "AT+CPSI? serving FULL poll on " << at_sess->path()
              << " (CEREG CID stamp disabled — needs EARFCN|PCI)\n";
    cereg_thread = std::thread([&] {
      while (!cereg_stop.load(std::memory_order_relaxed)) {
        (void)inject_serving_identity_once();
        for (int i = 0; i < 20 && !cereg_stop.load(std::memory_order_relaxed); ++i)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }

  std::thread hop_thread;
  std::atomic<bool> hop_stop{false};
  std::optional<std::string> saved_lte_bands;  // restore after hop
  std::optional<std::string> saved_cnmp;
  if (earfcn_hop && modem->at) {
    std::cout << "EARFCN hop: "
              << (hop_lock ? "sticky CCELLCFG grind (hold until target FULL)" : "CFUN 4↔1 bounce")
              << ", dwell=" << hop_dwell_sec << "s, max targets=" << hop_max
              << (hop_band_clip ? ", band-clip ON" : "")
              << (cops_between_hops
                      ? (", between-hop COPS=? every " + std::to_string(std::max(30, search_period_sec)) +
                         "s")
                      : ", between-hop COPS=? OFF")
              << (full_walk ? ", full-walk ON" : ", full-walk OFF") << "\n";
    hop_thread = std::thread([&] {
      // Warm-up: let DIAG ML1 populate neighbour RADIO rows before first lock.
      for (int i = 0; i < 100 && !hop_stop.load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

      auto at_cmd = [&](const char* cmd, int timeout_ms = 2000,
                        AtSession::TickFn tick = {}) -> std::optional<std::string> {
        return at_do(cmd, timeout_ms, std::move(tick));
      };

      auto unlock_cell = [&]() { (void)at_cmd("AT+CCELLCFG=0", 2000); };

      auto read_cpsi = [&]() -> std::optional<std::string> { return at_cmd("AT+CPSI?", 1500); };

      auto lte_online = [&]() -> bool {
        auto raw = read_cpsi();
        // parse_cpsi_lte rejects BAND0 / EARFCN=0xFFFFFFFF transitional garbage
        return raw && parse_cpsi_lte(*raw).ok;
      };

      auto best_full_hint = [&]() -> std::optional<HopTarget> {
        std::optional<HopTarget> hint;
        float best = -999.0f;
        for (const auto& c : engine.tracker().get_snapshot()) {
          if (c.rat != QCom::RatType::LTE || !c.passport.has_identity() || c.passport.tac == 0 ||
              c.radio.freq() == 0 || c.radio.pci_bsic() == 0)
            continue;
          float rsrp = -160.0f;
          if (auto* s = c.signal.get_if<QCom::LteSignalParams>()) {
            if (QCom::Utils::valid_lte_rsrp(s->rsrp)) rsrp = s->rsrp;
          }
          if (!hint || rsrp > best) {
            best = rsrp;
            hint = HopTarget{.earfcn = c.radio.freq(), .pci = c.radio.pci_bsic(), .rsrp = rsrp};
          }
        }
        return hint;
      };

      /// Tracker already has FULL passport for this physical key (SIB1/B0C2/prior CPSI).
      auto tracker_full_cid = [&](uint32_t earfcn, uint16_t pci) -> std::optional<uint32_t> {
        for (const auto& c : engine.tracker().get_snapshot()) {
          if (c.rat != QCom::RatType::LTE) continue;
          if (c.radio.freq() != earfcn || c.radio.pci_bsic() != pci) continue;
          if (c.passport.mcc == 0) continue;
          if (!QCom::Utils::valid_lte_eci(c.passport.cell_id) ||
              !QCom::Utils::valid_lte_tac(c.passport.tac))
            continue;
          return c.passport.cell_id;
        }
        return std::nullopt;
      };

      auto tracker_camped = [&](uint32_t earfcn, uint16_t pci) -> bool {
        for (const auto& c : engine.tracker().get_snapshot()) {
          if (c.rat != QCom::RatType::LTE) continue;
          if (c.radio.freq() != earfcn || c.radio.pci_bsic() != pci) continue;
          return c.ever_serving;
        }
        return false;
      };

      /// Mark RF key as camped (full-walk success / held CCELLCFG).
      auto mark_camped = [&](uint32_t earfcn, uint16_t pci) {
        QCom::LocalCellKey key{.freq = earfcn, .pci_bsic = pci};
        engine.inject_envelopes({QCom::Events::RrcEventEnvelope{
            .key = key,
            .rat = QCom::RatType::LTE,
            .event_data = QCom::Events::ServingChangedEvent{.is_serving = true},
        }});
      };

      /// Done when FULL; full-walk also requires a real camp on that EARFCN|PCI.
      auto target_done = [&](uint32_t earfcn, uint16_t pci) -> std::optional<uint32_t> {
        auto cid = tracker_full_cid(earfcn, pci);
        if (!cid) return std::nullopt;
        if (!full_walk) return cid;
        if (tracker_camped(earfcn, pci)) return cid;
        return std::nullopt;
      };

      // Soft LTE recover only by default. CFUN=0/4/1,1 re-enumerates USB on SIM8300
      // and caused reconnect storms (#30+). Use --recover-cfun for hard RF bounce.
      auto last_recover_fail = std::chrono::steady_clock::time_point{};
      auto recover_cooldown = std::chrono::seconds(0);

      auto cpsi_snip = [&]() -> std::string {
        auto raw = read_cpsi();
        if (!raw) return "(no CPSI)";
        std::string snip = *raw;
        for (char& c : snip) {
          if (c == '\r' || c == '\n') c = ' ';
        }
        if (snip.size() > 120) snip.resize(120);
        return snip;
      };

      auto wait_lte_online = [&](int ticks_500ms, const char* phase) -> bool {
        for (int i = 0; i < ticks_500ms && !hop_stop.load(std::memory_order_relaxed); ++i) {
          if (!diag_alive.load(std::memory_order_relaxed)) return false;
          if (lte_online()) {
            dash.note(std::string("[earfcn-hop] recovered LTE Online (") + phase + ")");
            recover_cooldown = std::chrono::seconds(0);
            return true;
          }
          if (i > 0 && (i % 30) == 0) {  // nudge every 15s — don't AT-spam
            (void)at_cmd("AT+CNMP=38", 5000);
            (void)at_cmd("AT+COPS=0", 5000);
            if (qmi_session) (void)qmi_session->control().force_network_search();
            dash.note(std::string("[earfcn-hop] recover ") + phase + " nudge — CPSI=" +
                      cpsi_snip());
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return false;
      };

      auto recover_lte = [&](std::optional<HopTarget> lock_hint = std::nullopt,
                            bool unlock_after = true) {
        unlock_cell();
        if (!diag_alive.load(std::memory_order_relaxed)) return false;
        if (qmi_session) {
          (void)qmi_session->control().set_mode_preference({qmi_observer::Rat::Lte});
        }

        // Soft only: LTE-only mode + auto PLMN. No CFUN — keeps USB composite alive.
        dash.note("[earfcn-hop] recover soft: CNMP=38 / COPS=0 (no CFUN)");
        (void)at_cmd("AT+CNMP=38", 10000);
        (void)at_cmd("AT+COPS=0", 10000);
        if (qmi_session) (void)qmi_session->control().force_network_search();
        if (lock_hint && lock_hint->pci != 0 && lock_hint->rsrp >= -110.0f) {
          const std::string cmd = "AT+CCELLCFG=1," + std::to_string(lock_hint->pci) + "," +
                                  std::to_string(lock_hint->earfcn);
          (void)at_cmd(cmd.c_str(), 2500);
        }
        if (wait_lte_online(90, "soft")) {  // 45s
          if (unlock_after) unlock_cell();
          return true;
        }

        if (!recover_cfun) {
          if (unlock_after) unlock_cell();
          dash.note("[earfcn-hop] soft recover timed out (no CFUN; pass --recover-cfun); CPSI=" +
                    cpsi_snip());
          return false;
        }

        // Optional hard path — expect USB re-enumerate + diag reconnect.
        dash.note("[earfcn-hop] recover airplane: CFUN=4 → 1 (may USB-reset)");
        (void)at_cmd("AT+CFUN=4", 10000);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        (void)at_cmd("AT+CNMP=38", 10000);
        (void)at_cmd("AT+CFUN=1", 10000);
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        if (at_sess) at_sess->reconnect();
        (void)at_cmd("AT+CNMP=38", 10000);
        (void)at_cmd("AT+COPS=0", 10000);
        if (wait_lte_online(80, "airplane")) {
          if (unlock_after) unlock_cell();
          return true;
        }

        if (unlock_after) unlock_cell();
        dash.note("[earfcn-hop] LTE recover timed out; CPSI=" + cpsi_snip());
        return false;
      };

      auto ensure_lte = [&](std::optional<HopTarget> hint = std::nullopt) {
        if (lte_online()) {
          recover_cooldown = std::chrono::seconds(0);
          return true;
        }
        if (!diag_alive.load(std::memory_order_relaxed)) return false;
        const auto now = std::chrono::steady_clock::now();
        if (recover_cooldown.count() > 0 && now - last_recover_fail < recover_cooldown) {
          dash.note("[earfcn-hop] not LTE — recover cooldown " +
                    std::to_string(recover_cooldown.count()) + "s; CPSI=" + cpsi_snip());
          return false;
        }
        dash.note("[earfcn-hop] not LTE — soft recover; CPSI=" + cpsi_snip());
        if (!hint) hint = best_full_hint();
        std::optional<HopTarget> use_hint;
        if (hint && hint->pci != 0 && hint->rsrp >= -110.0f) use_hint = hint;
        const bool ok = recover_lte(use_hint, /*unlock_after=*/true);
        if (!ok) {
          last_recover_fail = std::chrono::steady_clock::now();
          // Long cooldown — thrashing recover was killing USB.
          recover_cooldown = std::chrono::seconds(60);
        }
        return ok;
      };

      // Manual PLMN from AT+COPS=1,2,"mccmnc" — must not be undone by COPS=0 during locks.
      std::optional<std::pair<uint16_t, uint16_t>> forced_plmn;
      std::set<std::pair<uint16_t, uint16_t>> cops_seen_plmns;

      auto cpsi_plmn = [&]() -> std::optional<std::pair<uint16_t, uint16_t>> {
        auto raw = read_cpsi();
        if (!raw) return std::nullopt;
        auto c = parse_cpsi_lte(*raw);
        if (!c.ok || c.mcc == 0) return std::nullopt;
        return std::make_pair(c.mcc, c.mnc);
      };

      auto ensure_plmn = [&](uint16_t mcc, uint16_t mnc) -> bool {
        if (mcc == 0) return true;
        if (auto cur = cpsi_plmn(); cur && cur->first == mcc && cur->second == mnc) {
          forced_plmn = *cur;
          return true;
        }
        if (forced_plmn && forced_plmn->first == mcc && forced_plmn->second == mnc) {
          // Already selected; wait for camp if needed.
          if (lte_online()) return true;
        }
        unlock_cell();
        (void)at_cmd("AT+COPS=3,2", 2000);
        const std::string plmn = format_plmn_numeric(mcc, mnc);
        const std::string cmd = "AT+COPS=1,2,\"" + plmn + "\"";
        dash.note("[full-walk] PLMN select " + plmn + "…");
        auto rsp = at_cmd(cmd.c_str(), 120000);
        if (!rsp || !at_reply_ok(*rsp)) {
          dash.note("[full-walk] PLMN select failed for " + plmn);
          // Failed attach often re-fills FPLMN — wipe once and retry select.
          if (clear_fplmn) {
            auto rd = at_cmd("AT+CRSM=176,28539,0,0,12", 4000);
            if (rd && !crsm_fplmn_empty(*rd)) {
              dash.note("[fplmn] wipe after select fail, retry " + plmn);
              (void)at_cmd("AT+CRSM=214,28539,0,0,12,\"FFFFFFFFFFFFFFFFFFFFFFFF\"", 5000);
              rsp = at_cmd(cmd.c_str(), 120000);
            }
          }
          if (!rsp || !at_reply_ok(*rsp)) {
            forced_plmn.reset();
            (void)at_cmd("AT+COPS=0", 10000);
            return false;
          }
        }
        forced_plmn = {mcc, mnc};
        for (int i = 0; i < 300 && !hop_stop.load(std::memory_order_relaxed); ++i) {
          if (auto cur = cpsi_plmn(); cur && cur->first == mcc && cur->second == mnc) {
            dash.note("[full-walk] camped on PLMN " + plmn);
            (void)inject_serving_identity_once();
            return true;
          }
          if (i > 0 && (i % 40) == 0) {
            (void)at_cmd(cmd.c_str(), 60000);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        dash.note("[full-walk] PLMN select timeout for " + plmn);
        return false;
      };

      // AT+COPS=? only when unlocked — never during CCELLCFG grind. Full drain (up to 180s).
      // Rate-limited by --search-period (min 30s) so we don't stall every PCI.
      std::chrono::steady_clock::time_point last_cops{};
      auto maybe_cops_between = [&](const char* why) {
        if (!cops_between_hops || hop_stop.load(std::memory_order_relaxed)) return;
        const auto now = std::chrono::steady_clock::now();
        const auto min_gap = std::chrono::seconds(std::max(30, search_period_sec));
        if (last_cops.time_since_epoch().count() != 0 && now - last_cops < min_gap) return;

        unlock_cell();
        last_cops = now;
        ++cops_between_kicks;
        dash.note(std::string("[hop-cops] AT+COPS=? (") + why + ", #" +
                  std::to_string(cops_between_kicks.load()) + ")…");

        if (deep_search && !deep_dereg_done.exchange(true)) {
          if (auto rsp = at_cmd("AT+COPS=2", 8000); rsp && at_reply_ok(*rsp)) {
            dash.note("[hop-cops] AT+COPS=2 once (deregister)");
          } else {
            dash.note("[hop-cops] AT+COPS=2 failed");
          }
          for (int i = 0; i < 30 && !hop_stop.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // COPS=? floods DIAG with ML1/SIB — keep draining on LinuxSource thread and
        // report that bytes/logs keep growing while we wait on AT.
        auto* linux = dynamic_cast<QCom::LinuxSource*>(engine.source());
        const uint64_t bytes0 = linux ? linux->bytes_raw() : 0;
        const uint64_t logs0 = linux ? linux->logs_delivered() : 0;
        auto rsp = at_cmd("AT+COPS=?", 180000, [&](int elapsed_ms, std::string_view) {
          if (!linux || elapsed_ms < 1000 || (elapsed_ms % 5000) >= 1000) return;
          const uint64_t db = linux->bytes_raw() - bytes0;
          const uint64_t dl = linux->logs_delivered() - logs0;
          dash.note("[hop-cops] waiting " + std::to_string(elapsed_ms / 1000) +
                    "s — DIAG +" + std::to_string(db) + "B / +" + std::to_string(dl) +
                    " logs (still collecting)");
        });
        if (rsp) {
          auto found = parse_cops_plmn_list(*rsp);
          for (const auto& p : found) cops_seen_plmns.insert(p);
          const uint64_t db = linux ? linux->bytes_raw() - bytes0 : 0;
          const uint64_t dl = linux ? linux->logs_delivered() - logs0 : 0;
          dash.note("[hop-cops] done (plmns=" + std::to_string(found.size()) +
                    " seen_total=" + std::to_string(cops_seen_plmns.size()) + ", " + why +
                    ", DIAG +" + std::to_string(db) + "B / +" + std::to_string(dl) + " logs)");
        } else {
          dash.note(std::string("[hop-cops] timeout/fail (") + why + ")");
        }

        // Drop forced PLMN — search leaves radio unsettled; re-select before next foreign hop.
        forced_plmn.reset();
        (void)at_cmd("AT+COPS=0", 10000);
        for (int i = 0; i < 200 && !hop_stop.load(std::memory_order_relaxed); ++i) {
          if (lte_online()) break;
          if ((i % 20) == 19) (void)at_cmd("AT+COPS=0", 3000);
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!lte_online()) {
          dash.note("[hop-cops] LTE not back — recover");
          (void)ensure_lte(best_full_hint());
        }
        (void)inject_serving_identity_once();
        if (qmi_session) (void)qmi_session->control().force_network_search();
      };

      if (auto raw = at_cmd("AT+CNMP?", 1500)) {
        auto pos = raw->find("+CNMP:");
        if (pos != std::string::npos) {
          std::string line = raw->substr(pos + 6);
          if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);
          while (!line.empty() && line.front() == ' ') line.erase(line.begin());
          if (!line.empty()) saved_cnmp = line;
        }
      }
      if (hop_band_clip) {
        if (auto raw = at_cmd("AT+CSYSSEL=\"lte_band\"", 2000))
          saved_lte_bands = parse_csyssel_lte_band_list(*raw);
      }

      // Wipe SIM forbidden-PLMN list so COPS=1,2 foreign select is not blocked by EF_FPLMN.
      // Verified on SIM8300: AT+CRSM read/write 28539 works (RF on). QFPLMNCFG Delete → ERROR.
      auto wipe_fplmn = [&](const char* why) -> bool {
        if (!clear_fplmn) return false;
        auto rd = at_cmd("AT+CRSM=176,28539,0,0,12", 4000);
        if (!rd || (!crsm_sw_ok(*rd) && !crsm_fplmn_empty(*rd))) {
          dash.note(std::string("[fplmn] read failed (") + why + ")");
          return false;
        }
        if (crsm_fplmn_empty(*rd)) {
          dash.note(std::string("[fplmn] already empty (") + why + ")");
          return true;
        }
        dash.note(std::string("[fplmn] clearing EF_FPLMN (") + why + ")…");
        auto wr =
            at_cmd("AT+CRSM=214,28539,0,0,12,\"FFFFFFFFFFFFFFFFFFFFFFFF\"", 5000);
        if (!wr || !at_reply_ok(*wr)) {
          // Some firmwares want RF down for UPDATE BINARY.
          (void)at_cmd("AT+CFUN=4", 5000);
          wr = at_cmd("AT+CRSM=214,28539,0,0,12,\"FFFFFFFFFFFFFFFFFFFFFFFF\"", 5000);
          (void)at_cmd("AT+CFUN=1", 5000);
          (void)at_cmd("AT+COPS=0", 5000);
        }
        auto rd2 = at_cmd("AT+CRSM=176,28539,0,0,12", 4000);
        const bool ok = rd2 && crsm_fplmn_empty(*rd2);
        dash.note(ok ? std::string("[fplmn] cleared (") + why + ")"
                     : std::string("[fplmn] clear verify failed (") + why + ")");
        return ok;
      };

      // Pin LTE-only before anything else — WCDMA Online at start is common after prior runs.
      (void)at_cmd("AT+CNMP=38", 10000);
      (void)at_cmd("AT+COPS=0", 5000);
      (void)wipe_fplmn("warmup");
      const bool lte_ok = ensure_lte();
      (void)inject_serving_identity_once();
      // Never COPS=? while stuck on WCDMA / NO SERVICE — it deepens the hole.
      if (lte_ok)
        maybe_cops_between("warmup");
      else
        dash.note("[earfcn-hop] warmup: not LTE yet — skip COPS, will retry in loop");

      auto cfun_bounce = [&](uint32_t earfcn) {
        dash.note("[earfcn-hop] CFUN bounce toward EARFCN " + std::to_string(earfcn) + " (#" +
                  std::to_string(hop_kicks.load()) + ")");
        unlock_cell();
        (void)at_cmd("AT+CFUN=4", 3000);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (hop_stop.load(std::memory_order_relaxed)) return;
        (void)at_cmd("AT+CFUN=1", 3000);
        std::this_thread::sleep_for(std::chrono::seconds(2));
        (void)at_cmd("AT+COPS=0", 3000);
      };

      // SIM8200 AT+CCELLCFG=<enable>[,<pci>,<freq>] — verify with AT+CCELLCFG? → +CCELLCFG: pci,freq
      auto ccellcfg_matches = [&](uint16_t pci, uint32_t earfcn) -> bool {
        auto raw = at_cmd("AT+CCELLCFG?", 1500);
        if (!raw) return false;
        auto pos = raw->find("+CCELLCFG:");
        if (pos == std::string::npos) return false;
        std::string line = raw->substr(pos + 10);
        if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);
        auto toks = split_csv_tokens(line);
        if (toks.size() < 2) return false;
        try {
          return static_cast<uint16_t>(std::stoul(toks[0])) == pci &&
                 static_cast<uint32_t>(std::stoul(toks[1])) == earfcn;
        } catch (...) {
          return false;
        }
      };

      auto send_ccell_lock = [&](const HopTarget& t) -> bool {
        const std::string cmd =
            "AT+CCELLCFG=1," + std::to_string(t.pci) + "," + std::to_string(t.earfcn);
        auto rsp = at_cmd(cmd.c_str(), 2500);
        if (!rsp || !at_reply_ok(*rsp)) return false;
        // COPS=0 would undo AT+COPS=1,2 foreign select during full-walk.
        if (!forced_plmn) (void)at_cmd("AT+COPS=0", 3000);
        return ccellcfg_matches(t.pci, t.earfcn);
      };

      auto ccell_lock = [&](const HopTarget& t) -> bool {
        if (t.pci == 0) return false;
        // Unknown RSRP (SIB5 ghost = -155) is allowed; measured-weak is not.
        if (t.rsrp > -159.0f && t.rsrp < -118.0f && t.rsrp > -154.0f) {
          dash.note("[earfcn-hop] skip weak " + std::to_string(t.earfcn) + "/" +
                    std::to_string(t.pci) + " RSRP=" + std::to_string(static_cast<int>(t.rsrp)));
          return false;
        }
        unlock_cell();
        // Lock while RF on; if stuck on WCDMA, hard-recover INTO this lock.
        if (!lte_online()) {
          if (!recover_lte(t, /*unlock_after=*/false)) return false;
        } else if (!send_ccell_lock(t)) {
          dash.note("[earfcn-hop] CCELLCFG lock failed for " + std::to_string(t.earfcn) + "/" +
                    std::to_string(t.pci));
          unlock_cell();
          return false;
        }
        if (!ccellcfg_matches(t.pci, t.earfcn)) {
          dash.note("[earfcn-hop] CCELLCFG? mismatch for " + std::to_string(t.earfcn) + "/" +
                    std::to_string(t.pci));
          unlock_cell();
          return false;
        }
        ++hop_locks;
        dash.note("[earfcn-hop] CCELLCFG grind " + std::to_string(t.earfcn) + "/" +
                  std::to_string(t.pci) + " (#" + std::to_string(hop_kicks.load()) + ")");
        return true;
      };

      // Extra NAS cell-location + serving-system/signal pulls around each hop (like qmicli).
      // Does not force_network_search — that stays once after lock.
      auto pull_qmi_enrich = [&](const char* /*why*/) {
        if (!qmi_session) return;
        ++qmi_hop_snaps;
        if (auto snap = qmi_session->nas().snapshot_cells(); snap) {
          auto envs = qmi_observer::to_rrc_envelopes(snap.value());
          if (!envs.empty()) engine.inject_envelopes(std::move(envs));
        }
        if (auto st = qmi_session->nas().snapshot_status(); st) {
          std::lock_guard qlock(qmi_status_mu);
          qmi_status = std::move(st.value());
        }
      };

      bool bands_clipped = false;
      // Failed grinds: skip ~120s so we don't thrash one unreachable PCI.
      constexpr auto k_cooldown = std::chrono::seconds(120);
      std::map<std::pair<uint32_t, uint16_t>, std::chrono::steady_clock::time_point> cooldown;

      auto is_weak_measured = [](float rsrp) {
        return rsrp > -159.0f && rsrp < -118.0f && rsrp > -154.0f;
      };

      while (!hop_stop.load(std::memory_order_relaxed)) {
        // USB unplug: pause hop/AT until main reconnects DIAG (avoid AT storms on dead tty).
        if (!diag_alive.load(std::memory_order_relaxed)) {
          dash.note("[earfcn-hop] paused — waiting for DIAG USB reconnect");
          for (int i = 0; i < 20 && !hop_stop.load(std::memory_order_relaxed) &&
                          !diag_alive.load(std::memory_order_relaxed);
               ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }

        const auto now = std::chrono::steady_clock::now();
        const std::size_t frontier_n =
            static_cast<std::size_t>(std::max(1, full_walk ? std::max(hop_max, 32) : hop_max));
        auto targets =
            full_walk ? pick_full_walk_targets(engine.tracker().get_snapshot(), frontier_n)
                      : pick_hop_targets(engine.tracker().get_snapshot(), frontier_n);
        if (hop_lock) {
          std::vector<HopTarget> usable;
          for (const auto& t : targets) {
            if (t.pci == 0 || is_weak_measured(t.rsrp)) continue;
            if (full_walk) {
              if (target_done(t.earfcn, t.pci)) continue;  // FULL+camped
            } else if (tracker_full_cid(t.earfcn, t.pci)) {
              continue;
            }
            auto cd = cooldown.find({t.earfcn, t.pci});
            if (cd != cooldown.end() && now - cd->second < k_cooldown) continue;
            usable.push_back(t);
          }
          targets = std::move(usable);
        }
        if (targets.empty()) {
          const bool on_lte = lte_online();
          if (!on_lte) {
            dash.note("[earfcn-hop] no LTE rows — forcing RAT recover before ML1 listen");
            (void)ensure_lte(best_full_hint());
          } else {
            dash.note(full_walk ? "[full-walk] frontier empty — idle / COPS kick"
                                : "[earfcn-hop] no incomplete targets — idle / COPS kick (listening ML1)");
            maybe_cops_between("idle");
          }
          (void)inject_serving_identity_once();
          // Longer idle after failed recover so we don't thrash CFUN every 20s.
          const int idle_ds = on_lte || lte_online() ? 150 : 250;
          for (int i = 0; i < idle_ds && !hop_stop.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }

        if (hop_lock && hop_band_clip && !bands_clipped) {
          const std::string clip = bands_csv_from_earfcns(targets);
          const std::string cmd = "AT+CSYSSEL=\"lte_band\"," + clip;
          if (auto rsp = at_cmd(cmd.c_str(), 2500); rsp && at_reply_ok(*rsp)) {
            bands_clipped = true;
            dash.note("[earfcn-hop] CSYSSEL lte_band clipped → " + clip);
          }
        }

        // Sticky grind: one strongest target per pass — hold lock until FULL or timeout.
        // CFUN bounce: walk the ranked list (imprecise).
        const std::size_t n_kick = hop_lock ? 1 : targets.size();
        for (std::size_t ti = 0; ti < n_kick; ++ti) {
          const HopTarget& t = targets[ti];
          if (hop_stop.load(std::memory_order_relaxed)) break;
          ++hop_kicks;

          // Prefer manual PLMN before CCELLCFG; on fail still RF-lock (B0C2/SIB may land
          // without Online attach — common on foreign PLMN with home SIM).
          if (full_walk && hop_lock && t.mcc != 0) {
            if (!ensure_plmn(t.mcc, t.mnc)) {
              dash.note("[full-walk] PLMN " + format_plmn_numeric(t.mcc, t.mnc) +
                        " select failed — RF-lock " + std::to_string(t.earfcn) + "/" +
                        std::to_string(t.pci) + " anyway");
            }
          }

          if (hop_lock) {
            if (!ccell_lock(t)) {
              cooldown[{t.earfcn, t.pci}] = std::chrono::steady_clock::now();
              continue;
            }
          } else {
            cfun_bounce(t.earfcn);
          }

          // Soft QMI nudge only — never AT+COPS=? here. Then enrich cell list + NAS status.
          if (qmi_session) (void)qmi_session->control().force_network_search();
          pull_qmi_enrich("post-lock");

          const int dwell =
              hop_lock ? std::max(25, hop_dwell_sec) : std::max(8, hop_dwell_sec);
          bool target_stamped = false;
          bool left_lte = false;
          bool relocked = false;
          uint32_t stamped_cid = 0;

          for (int i = 0; i < dwell * 10 && !hop_stop.load(std::memory_order_relaxed); ++i) {
            // Poll identity ~1 Hz; keep CCELLCFG held the whole time.
            if (i > 0 && (i % 10) == 0) {
              auto raw = read_cpsi();
              if (raw && cpsi_is_wcdma_or_noservice(*raw)) {
                dash.note("[earfcn-hop] left LTE during grind — abort");
                left_lte = true;
                break;
              }

              (void)inject_serving_identity_once();

              if (raw) {
                auto cpsi = parse_cpsi_lte(*raw);
                if (cpsi.ok && cpsi.earfcn == t.earfcn && cpsi.pci == t.pci &&
                    QCom::Utils::valid_lte_eci(cpsi.cell_id) &&
                    QCom::Utils::valid_lte_tac(cpsi.tac)) {
                  mark_camped(t.earfcn, t.pci);
                  target_stamped = true;
                  stamped_cid = cpsi.cell_id;
                  break;
                }
              }

              // Held CCELLCFG + FULL on target → success (marks camped for full-walk).
              const bool locked_on_target =
                  !hop_lock || ccellcfg_matches(t.pci, t.earfcn);
              if (locked_on_target) {
                if (auto cid = tracker_full_cid(t.earfcn, t.pci)) {
                  mark_camped(t.earfcn, t.pci);
                  target_stamped = true;
                  stamped_cid = *cid;
                  break;
                }
              }

              // ~5 s QMI enrich during grind (cell-location + serving-system).
              if ((i % 50) == 0) pull_qmi_enrich("grind");

              // Lock dropped? Re-assert once without unlocking.
              if (hop_lock && (i % 50) == 0 && !ccellcfg_matches(t.pci, t.earfcn)) {
                dash.note("[earfcn-hop] CCELLCFG dropped — re-lock " +
                          std::to_string(t.earfcn) + "/" + std::to_string(t.pci));
                if (!send_ccell_lock(t)) {
                  if (!relocked) {
                    relocked = true;
                    if (!recover_lte(t, /*unlock_after=*/false) ||
                        !ccellcfg_matches(t.pci, t.earfcn)) {
                      left_lte = true;
                      break;
                    }
                    // recover may have COPS=0 — restore foreign PLMN.
                    if (full_walk && t.mcc != 0) (void)ensure_plmn(t.mcc, t.mnc);
                  } else {
                    left_lte = true;
                    break;
                  }
                }
              }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }

          if (target_stamped) {
            ++hop_fulls;
            mark_camped(t.earfcn, t.pci);
            dash.note(std::string(full_walk ? "[full-walk]" : "[earfcn-hop]") + " FULL+camp on " +
                      std::to_string(t.earfcn) + "/" + std::to_string(t.pci) +
                      " CID=" + std::to_string(stamped_cid) + " (held lock)");
            pull_qmi_enrich("full");
            // Settle so B0C0 SIB1/neighbors merge while still locked.
            for (int j = 0; j < 50 && !hop_stop.load(std::memory_order_relaxed); ++j)
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            cooldown.erase({t.earfcn, t.pci});
          } else if (!left_lte) {
            if (auto stamp = inject_serving_identity_once(); stamp == ServingStamp::Full) {
              if (auto cid = tracker_full_cid(t.earfcn, t.pci)) {
                mark_camped(t.earfcn, t.pci);
                ++hop_fulls;
                target_stamped = true;
                dash.note(std::string(full_walk ? "[full-walk]" : "[earfcn-hop]") + " FULL+camp on " +
                          std::to_string(t.earfcn) + "/" + std::to_string(t.pci) +
                          " CID=" + std::to_string(*cid) + " (late stamp)");
                pull_qmi_enrich("full");
                cooldown.erase({t.earfcn, t.pci});
              } else {
                dash.note("[earfcn-hop] grind timeout — serving FULL elsewhere, not " +
                          std::to_string(t.earfcn) + "/" + std::to_string(t.pci));
              }
            } else {
              dash.note("[earfcn-hop] grind timeout — no FULL for " +
                        std::to_string(t.earfcn) + "/" + std::to_string(t.pci) +
                        (full_walk && tracker_full_cid(t.earfcn, t.pci)
                             ? " (had CID, no camp)"
                             : ""));
            }
          }
          if (hop_lock && !target_stamped)
            cooldown[{t.earfcn, t.pci}] = std::chrono::steady_clock::now();

          if (hop_lock) {
            unlock_cell();
            if (left_lte || !lte_online()) {
              (void)ensure_lte(best_full_hint());
              if (full_walk && forced_plmn) (void)ensure_plmn(forced_plmn->first, forced_plmn->second);
            }
            (void)inject_serving_identity_once();
            // Between targets only — never while CCELLCFG held. Rate-limited inside.
            maybe_cops_between(target_stamped ? "after-full" : "after-miss");
          } else {
            maybe_cops_between("after-cfun");
          }
        }
      }

      unlock_cell();
      if (saved_lte_bands && !saved_lte_bands->empty()) {
        const std::string restore = "AT+CSYSSEL=\"lte_band\"," + *saved_lte_bands;
        (void)at_cmd(restore.c_str(), 2500);
      }
      if (saved_cnmp && !saved_cnmp->empty()) {
        const std::string restore = "AT+CNMP=" + *saved_cnmp;
        (void)at_cmd(restore.c_str(), 10000);
      } else {
        (void)at_cmd("AT+CNMP=54", 10000);  // WCDMA+LTE default restore
      }
      if (qmi_session && (prefer_lte || hop_lock)) {
        (void)qmi_session->control().set_mode_preference(
            {qmi_observer::Rat::Lte, qmi_observer::Rat::Wcdma});
      }
      (void)at_cmd("AT+CFUN=1", 3000);
      (void)at_cmd("AT+COPS=0", 3000);
      (void)inject_serving_identity_once();
    });
  } else if (earfcn_hop) {
    std::cerr << "--earfcn-hop needs AT path\n";
  }

  {
    std::ostringstream ban;
    ban << "live_scanner  diag=" << modem->diag;
    if (modem->at) ban << "  at=" << *modem->at;
    if (modem->qmi) ban << "  qmi=" << *modem->qmi;
    ban << "\nTree: eNB site → FULL cells → n=neighbors  |  *=serving  |  Fill=FULL/PLMN/RADIO\n";
    if (earfcn_hop)
      ban << "earfcn-hop ON (" << (hop_lock ? "sticky CCELLCFG grind" : "CFUN bounce")
          << (hop_band_clip ? ", band-clip" : "")
          << (cops_between_hops ? ", between-hop COPS=?" : "")
          << (full_walk ? ", full-walk" : "")
          << (clear_fplmn ? ", clear-fplmn" : "") << ")\n";
    if (at_cereg) ban << "AT+CPSI?/CNWINFO serving FULL poll ON (no CEREG CID stamp)\n";
    ban << "(in-place refresh; Ctrl+C / duration end → summary)\n\n";
    dash.set_banner(ban.str());
  }

  auto last_reconnect_attempt = std::chrono::steady_clock::time_point{};
  auto try_reconnect_diag = [&]() -> bool {
    auto* linux = dynamic_cast<QCom::LinuxSource*>(engine.source());
    if (!linux) return false;

    // Rate-limit — CFUN churn used to reconnect every few seconds (#30+).
    const auto now = std::chrono::steady_clock::now();
    if (last_reconnect_attempt.time_since_epoch().count() != 0 &&
        now - last_reconnect_attempt < std::chrono::seconds(8)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      return engine.source() && engine.source()->is_running();
    }
    last_reconnect_attempt = now;

    dash.note("[diag] USB/DIAG down — waiting for device…");
    std::string diag_path = modem->diag;
    // Wait up to ~90s for tty to reappear (re-enumerate may change path).
    for (int i = 0; i < 90 && !g_user_stop.load(std::memory_order_relaxed); ++i) {
      if (i > 0 && (i % 5) == 0) {
        if (auto again = resolve_modem(device, qmi_path); again) {
          *modem = *again;
          diag_path = modem->diag;
          if (modem->at && at_sess && at_sess->path() != *modem->at)
            at_sess = std::make_unique<AtSession>(*modem->at);
        }
        dash.note("[diag] still waiting… (" + std::to_string(i) + "s) path=" + diag_path);
      }
      if (::access(diag_path.c_str(), R_OK | W_OK) == 0) break;
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (g_user_stop.load(std::memory_order_relaxed)) return false;
    if (::access(diag_path.c_str(), R_OK | W_OK) != 0) {
      dash.note("[diag] device still missing: " + diag_path);
      return false;
    }

    linux->set_device_path(diag_path);
    if (!linux->reconnect()) {
      dash.note(std::string("[diag] reconnect failed: ") + std::string(linux->last_error()));
      return false;
    }
    if (at_sess) {
      if (modem->at && at_sess->path() != *modem->at)
        at_sess = std::make_unique<AtSession>(*modem->at);
      else
        at_sess->reconnect();
    } else if (modem->at) {
      at_sess = std::make_unique<AtSession>(*modem->at);
    }
    // QMI left alone here (shared with qmi_thread). Snapshots may fail until next process
    // if cdc-wdm died; DIAG+AT survey continues.
    ++diag_reconnects;
    diag_alive.store(true, std::memory_order_relaxed);
    diag_needs_reconnect.store(false, std::memory_order_relaxed);
    dash.note("[diag] reconnected OK on " + diag_path + " (#" +
              std::to_string(diag_reconnects.load()) + ")");
    return true;
  };

  if (duration_sec <= 0) {
    std::cout << "Running until Ctrl+C… (USB disconnect → auto-reconnect)\n" << std::flush;
  } else {
    std::cout << "Running for " << duration_sec << "s… (USB disconnect → auto-reconnect)\n"
              << std::flush;
  }
  const auto run_started = std::chrono::steady_clock::now();
  while (!g_user_stop.load(std::memory_order_relaxed)) {
    if (duration_sec > 0) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - run_started)
                               .count();
      if (elapsed >= duration_sec) break;
    }

    const bool down = diag_needs_reconnect.load(std::memory_order_relaxed) ||
                      !engine.source() || !engine.source()->is_running();
    if (down) {
      diag_alive.store(false, std::memory_order_relaxed);
      // Keep hopping paused until DIAG is back; don't exit the process.
      if (!try_reconnect_diag()) {
        // Brief backoff then retry — modem may still be enumerating.
        for (int i = 0; i < 3 && !g_user_stop.load(std::memory_order_relaxed); ++i)
          std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
    }

    write_live_json(true);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  write_live_json(true);

  qmi_stop = true;
  at_only_stop = true;
  cereg_stop = true;
  hop_stop = true;
  g_user_stop = true;
  if (qmi_thread.joinable()) qmi_thread.join();
  if (at_only_thread.joinable()) at_only_thread.join();
  if (cereg_thread.joinable()) cereg_thread.join();
  if (hop_thread.joinable()) hop_thread.join();
  if (qmi_session) qmi_session->close();
  engine.stop();
  dash.leave_inplace();

  // Restore RF / operator selection after survey kicks (hop thread may have already done this).
  if (at_sess && (at_cops || earfcn_hop || at_cereg || cops_between_hops)) {
    if (earfcn_hop) {
      (void)at_do("AT+CCELLCFG=0", 2000);
      if (saved_lte_bands && !saved_lte_bands->empty()) {
        const std::string restore = "AT+CSYSSEL=\"lte_band\"," + *saved_lte_bands;
        (void)at_do(restore.c_str(), 2500);
      }
      if (saved_cnmp && !saved_cnmp->empty()) {
        const std::string restore = "AT+CNMP=" + *saved_cnmp;
        (void)at_do(restore.c_str(), 10000);
      } else if (prefer_lte) {
        (void)at_do("AT+CNMP=54", 10000);
      }
    }
    (void)at_do("AT+CFUN=1", 3000);
    (void)at_do("AT+COPS=0", 5000);
  }

  std::cout << "\nDone. updates=" << updates.load() << " cells=" << engine.tracker().cell_count();
  if (auto* linux = dynamic_cast<QCom::LinuxSource*>(engine.source())) {
    std::cout << " raw_bytes=" << linux->bytes_raw() << " msgs=" << linux->frames_ok()
              << " logs=" << linux->logs_delivered() << " hdlc_bad_crc=" << linux->frames_bad_crc()
              << " diag_revives=" << linux->silent_revives();
    if (linux->bytes_raw() == 0) {
      std::cerr << "\nNo bytes from DIAG. Modem silent or wrong port — re-plug USB / "
                   "qmi_recover / check masks were not left disabled.\n";
    }
  }
  if (use_qmi || at_cops || at_cereg || earfcn_hop || cops_between_hops) {
    std::cout << " qmi_polls=" << qmi_polls.load() << " qmi_ok=" << qmi_ok.load()
              << " ota_kicks=" << search_kicks.load() << " cereg_ok=" << cereg_ok.load()
              << " cpsi_ok=" << cpsi_ok.load() << " cnw_ok=" << cnw_ok.load()
              << " hop_kicks=" << hop_kicks.load() << " hop_locks=" << hop_locks.load()
              << " hop_fulls=" << hop_fulls.load()
              << " hop_cops=" << cops_between_kicks.load()
              << " qmi_hop_snaps=" << qmi_hop_snaps.load()
              << " diag_reconnects=" << diag_reconnects.load();
  }
  std::cout << "\n";

  {
    std::lock_guard lock(code_mu);
    print_log_code_table(code_hist, engine.parser());
    if (!b0c0_pdu_hist.empty()) {
      std::cout << "B0C0 OTA wrapper:\n";
      for (const auto& [ver, n] : b0c0_ver_hist)
        std::cout << "  ver=0x" << std::hex << ver << std::dec << "  x" << n << "\n";
      std::cout << "B0C0 pdu_num (SIB1≈2/3/9 by version; MIB≈1/8):\n";
      for (const auto& [pdu, n] : b0c0_pdu_hist) {
        if (pdu < 0)
          std::cout << "  (undecoded wrapper) x" << n << "\n";
        else
          std::cout << "  pdu=" << pdu << "  x" << n << "\n";
      }
      const auto asn1_empty = QCom::Lte::lte_rrc_ota_asn1_empty_count();
      if (asn1_empty)
        std::cout << "  ASN.1 empty/fail (BCCH-DL-SCH only) x" << asn1_empty << "\n";
    }
  }

  {
    const auto snap = engine.tracker().get_snapshot();
    size_t with_id = 0;
    size_t with_plmn = 0;
    size_t complete = 0;
    size_t radio_only = 0;
    size_t plmn_only_weak = 0;
    for (const auto& c : snap) {
      if (c.passport.has_identity()) ++with_id;
      if (c.passport.mcc != 0) ++with_plmn;
      const uint32_t freq = c.radio.freq();
      const uint16_t pci = c.radio.pci_bsic();
      const bool full = c.rat == QCom::RatType::LTE && freq != 0 && pci != 0 &&
                        c.passport.mcc != 0 && QCom::Utils::valid_lte_tac(c.passport.tac) &&
                        QCom::Utils::valid_lte_eci(c.passport.cell_id);
      if (full) ++complete;
      else if (c.rat == QCom::RatType::LTE && freq != 0 && pci != 0 && c.passport.mcc == 0)
        ++radio_only;
      else if (c.rat == QCom::RatType::LTE && freq != 0 && pci == 0 && c.passport.mcc != 0)
        ++plmn_only_weak;
    }
    std::cout << "Identity coverage: " << with_id << "/" << snap.size()
              << " cells have passport (CID); " << with_plmn << "/" << snap.size()
              << " have PLMN\n";
    std::cout << "LTE towers: complete=" << complete << " radio_only=" << radio_only
              << " plmn_weak(EARFCN|0)=" << plmn_only_weak << "\n";
    print_cells_table(snap, "Final snapshot");
  }
  return 0;
}
