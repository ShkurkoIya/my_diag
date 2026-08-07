/// Offline journal replay tool — not part of the runtime scanner.
#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "CellCsv.h"
#include "JournalTxtReader.h"
#include "TowerExport.h"
#include "core/QualcomParser.h"

using namespace QCom;
using namespace QCom::Tools;

namespace {

struct Args {
  std::string journal;
  std::string out_cells;
  std::string expect;
  std::string towers_json;
  uint64_t limit{0};
  bool summary{false};
  bool list_cells{false};
  bool towers{false};
  size_t towers_limit{40};  // console cap; 0 = all
};

void usage(const char* argv0) {
  std::cerr << "Usage: " << argv0
            << " --journal <observer_journal_*.txt>\n"
               "       [--out-cells <ours.csv>]\n"
               "       [--expect <observer_cells_*.csv>]\n"
               "       [--summary]          # coverage / compare tables\n"
               "       [--list-cells]       # flat cell list\n"
               "       [--towers]           # nested towers (GUI schema) to console\n"
               "       [--towers-json <path.json>]\n"
               "       [--towers-limit N]   # console tower cap (default 40, 0=all)\n"
               "       [--limit N]\n";
}

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    auto need = [&](std::string& dst) -> bool {
      if (i + 1 >= argc) return false;
      dst = argv[++i];
      return true;
    };
    if (arg == "--journal") {
      if (!need(a.journal)) return false;
    } else if (arg == "--out-cells") {
      if (!need(a.out_cells)) return false;
    } else if (arg == "--expect") {
      if (!need(a.expect)) return false;
    } else if (arg == "--towers-json") {
      if (!need(a.towers_json)) return false;
    } else if (arg == "--limit") {
      std::string s;
      if (!need(s)) return false;
      a.limit = std::stoull(s);
    } else if (arg == "--towers-limit") {
      std::string s;
      if (!need(s)) return false;
      a.towers_limit = std::stoull(s);
    } else if (arg == "--summary") {
      a.summary = true;
    } else if (arg == "--list-cells") {
      a.list_cells = true;
    } else if (arg == "--towers") {
      a.towers = true;
    } else if (arg == "-h" || arg == "--help") {
      return false;
    } else {
      std::cerr << "Unknown arg: " << arg << '\n';
      return false;
    }
  }
  return !a.journal.empty();
}

// ── tiny ASCII table helper ──────────────────────────────────────────────────

using Row = std::vector<std::string>;

void print_table(const std::vector<std::string>& headers, const std::vector<Row>& rows) {
  if (headers.empty()) return;
  std::vector<size_t> w(headers.size(), 0);
  for (size_t i = 0; i < headers.size(); ++i) w[i] = headers[i].size();
  for (const auto& r : rows) {
    for (size_t i = 0; i < headers.size() && i < r.size(); ++i) w[i] = std::max(w[i], r[i].size());
  }

  auto rule = [&] {
    std::cout << '+';
    for (size_t i = 0; i < w.size(); ++i) std::cout << std::string(w[i] + 2, '-') << '+';
    std::cout << '\n';
  };
  auto emit = [&](const Row& r, bool hdr) {
    std::cout << '|';
    for (size_t i = 0; i < w.size(); ++i) {
      std::string cell = (i < r.size()) ? r[i] : "";
      std::cout << ' ' << std::left << std::setw(static_cast<int>(w[i])) << cell << " |";
    }
    std::cout << '\n';
    (void)hdr;
  };

  rule();
  emit(headers, true);
  rule();
  for (const auto& r : rows) emit(r, false);
  rule();
}

std::string hex_code(LogCode c) {
  std::ostringstream oss;
  oss << "0x" << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << c;
  return oss.str();
}

std::string i2s(auto v) { return std::to_string(v); }

std::string plmn_s(uint16_t mcc, uint16_t mnc) {
  if (!mcc) return "-";
  return std::to_string(mcc) + "-" + std::to_string(mnc);
}

void print_snapshot(const std::vector<CellIdentity>& cells) {
  std::cout << "\n=== cells (" << cells.size() << ") ===\n";
  for (const auto& c : cells) {
    CellRow r = from_identity(c);
    std::cout << (r.serving ? "* " : "  ") << r.rat << " arfcn=" << r.arfcn;
    if (r.rat == "GSM")
      std::cout << " bsic=" << r.bsic << " ncc=" << unsigned(r.ncc) << " bcc=" << unsigned(r.bcc);
    else
      std::cout << " pci=" << r.pci;
    if (r.mcc) std::cout << " plmn=" << r.mcc << '-' << r.mnc;
    if (r.lac) std::cout << " lac=" << r.lac;
    if (r.cid) std::cout << " cid=" << r.cid;
    if (r.has_signal) std::cout << " sig=" << static_cast<int>(r.signal_dbm);
    if (r.has_rsrq) std::cout << " rsrq=" << r.rsrq;
    std::cout << '\n';
  }
}

void print_ours_summary(const std::vector<CellIdentity>& cells) {
  struct Agg {
    size_t n{0};
    size_t serving{0};
    size_t with_cid{0};
    size_t with_sig{0};
  };
  std::map<std::string, Agg> by;
  Agg total;
  for (const auto& c : cells) {
    CellRow r = from_identity(c);
    auto& a = by[r.rat];
    ++a.n;
    ++total.n;
    if (r.serving) {
      ++a.serving;
      ++total.serving;
    }
    if (r.cid) {
      ++a.with_cid;
      ++total.with_cid;
    }
    if (r.has_signal) {
      ++a.with_sig;
      ++total.with_sig;
    }
  }

  std::cout << "\n=== ours coverage ===\n";
  std::vector<Row> rows;
  for (const auto& [rat, a] : by) {
    rows.push_back({rat, i2s(a.n), i2s(a.serving), i2s(a.with_cid), i2s(a.with_sig)});
  }
  rows.push_back({"TOTAL", i2s(total.n), i2s(total.serving), i2s(total.with_cid), i2s(total.with_sig)});
  print_table({"RAT", "cells", "serving", "with_cid", "with_sig"}, rows);
}

void print_logcode_table(const std::unordered_map<LogCode, uint64_t>& by_code,
                         const std::unordered_map<LogCode, uint64_t>& rejected_by_code) {
  std::vector<std::pair<LogCode, uint64_t>> sorted(by_code.begin(), by_code.end());
  std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });

  std::cout << "\n=== log codes ===\n";
  std::vector<Row> rows;
  for (auto& [code, n] : sorted) {
    uint64_t rej = 0;
    if (auto it = rejected_by_code.find(code); it != rejected_by_code.end()) rej = it->second;
    uint64_t ok = n - rej;
    int pct = n ? static_cast<int>((100.0 * static_cast<double>(ok)) / static_cast<double>(n)) : 0;
    rows.push_back({hex_code(code), i2s(n), i2s(ok), i2s(rej), i2s(pct) + "%"});
  }
  print_table({"code", "fed", "accepted", "rejected", "ok%"}, rows);
}

void print_expect_summary(const ExpectDiff& d) {
  std::cout << "\n=== vs expect (key = RAT|arfcn|pci/bsic) ===\n";

  // Collect all RATs
  std::map<std::string, bool> rats;
  for (auto& [r, _] : d.ours_by_rat) rats[r] = true;
  for (auto& [r, _] : d.expect_by_rat) rats[r] = true;

  auto get = [](const std::map<std::string, size_t>& m, const std::string& k) -> size_t {
    auto it = m.find(k);
    return it == m.end() ? 0 : it->second;
  };

  std::vector<Row> rows;
  size_t to = 0, te = 0, ts = 0, too = 0, toe = 0;
  for (auto& [rat, _] : rats) {
    size_t o = get(d.ours_by_rat, rat);
    size_t e = get(d.expect_by_rat, rat);
    size_t s = get(d.shared_by_rat, rat);
    size_t oo = get(d.only_ours_by_rat, rat);
    size_t oe = get(d.only_expect_by_rat, rat);
    to += o;
    te += e;
    ts += s;
    too += oo;
    toe += oe;
    int cov = e ? static_cast<int>((100.0 * s) / e) : 0;
    rows.push_back({rat, i2s(o), i2s(e), i2s(s), i2s(oo), i2s(oe), i2s(cov) + "%"});
  }
  int cov_t = te ? static_cast<int>((100.0 * ts) / te) : 0;
  rows.push_back({"TOTAL", i2s(to), i2s(te), i2s(ts), i2s(too), i2s(toe), i2s(cov_t) + "%"});
  print_table({"RAT", "ours", "vlad", "shared", "only_ours", "only_vlad", "cov%"}, rows);

  std::cout << "\n=== identity quality (shared keys) ===\n";
  print_table({"metric", "count"},
              {{"shared keys", i2s(d.shared)},
               {"cid match", i2s(d.shared_cid_match)},
               {"cid mismatch", i2s(d.shared_cid_mismatch)},
               {"both have signal", i2s(d.shared_both_signal)},
               {"ours serving", i2s(d.ours_serving)},
               {"vlad serving", i2s(d.expect_serving)},
               {"ours with cid", i2s(d.ours_with_cid)},
               {"vlad with cid", i2s(d.expect_with_cid)}});

  if (!d.cid_mismatches.empty()) {
    std::cout << "\n=== CID mismatches on shared keys (sample) ===\n";
    std::vector<Row> mrows;
    for (const auto& m : d.cid_mismatches) {
      mrows.push_back({m.rat, i2s(m.arfcn), i2s(m.ours_cid), plmn_s(m.ours_mcc, m.ours_mnc),
                       i2s(m.expect_cid), plmn_s(m.expect_mcc, m.expect_mnc)});
    }
    print_table({"RAT", "arfcn", "ours_cid", "ours_plmn", "vlad_cid", "vlad_plmn"}, mrows);
  }

  if (!d.sample_only_ours.empty() || !d.sample_only_expect.empty()) {
    std::cout << "\n=== key samples ===\n";
    std::vector<Row> srows;
    size_t n = std::max(d.sample_only_ours.size(), d.sample_only_expect.size());
    for (size_t i = 0; i < n; ++i) {
      srows.push_back({i < d.sample_only_ours.size() ? d.sample_only_ours[i] : "",
                       i < d.sample_only_expect.size() ? d.sample_only_expect[i] : ""});
    }
    print_table({"only_ours", "only_vlad"}, srows);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, args)) {
    usage(argv[0]);
    return 2;
  }

  JournalTxtReader reader(args.journal);
  if (!reader.open()) {
    std::cerr << "Failed to open journal: " << args.journal << '\n';
    return 1;
  }

  QualcomParser parser;
  uint64_t fed = 0;
  uint64_t accepted = 0;
  uint64_t rejected = 0;
  std::unordered_map<LogCode, uint64_t> by_code;
  std::unordered_map<LogCode, uint64_t> rejected_by_code;

  while (auto entry = reader.next()) {
    if (args.limit && fed >= args.limit) break;
    ++fed;
    ++by_code[entry->log_code];

    auto result = parser.on_packet(QualcommPacketView{
        .log_code = entry->log_code,
        .timestamp = entry->seq,
        .payload = entry->payload,
        .wall_time = entry->wall_time,
    });
    // Text-line arfcn= fallback for EVENT surround hint (if RAW layout differs).
    if (entry->log_code == 0x0000 && entry->earfcn && *entry->earfcn > 0 &&
        *entry->earfcn <= 1023) {
      parser.tracker().set_gsm_surround_arfcn_hint(static_cast<uint16_t>(*entry->earfcn));
    }
    if (result)
      ++accepted;
    else {
      ++rejected;
      ++rejected_by_code[entry->log_code];
    }
  }

  auto snap = parser.tracker().get_snapshot();

  std::cout << "journal: " << args.journal << '\n';
  std::cout << "entries_with_raw=" << reader.entries() << " parse_errors=" << reader.errors()
            << '\n';
  std::cout << "fed=" << fed << " accepted=" << accepted << " rejected=" << rejected
            << " cells=" << snap.size() << '\n';

  const bool show_summary = args.summary || !args.expect.empty();
  const bool show_towers = args.towers || !args.towers_json.empty();
  const bool show_cells =
      args.list_cells || (!args.summary && args.expect.empty() && !show_towers);

  if (show_summary) {
    print_ours_summary(snap);
    print_logcode_table(by_code, rejected_by_code);
  } else if (!show_towers) {
    std::cout << "\n--- by log code (fed) ---\n";
    for (auto& [code, n] : by_code) {
      auto rej = rejected_by_code[code];
      std::cout << "  " << hex_code(code) << "  n=" << n;
      if (rej) std::cout << "  rejected=" << rej;
      std::cout << '\n';
    }
  }

  if (show_towers) {
    if (args.towers) print_towers_pretty(std::cout, snap, args.journal, args.towers_limit);
    if (!args.towers_json.empty()) {
      if (!write_towers_json(args.towers_json, snap, args.journal)) {
        std::cerr << "Failed to write " << args.towers_json << '\n';
        return 1;
      }
      std::cout << "\nwrote towers JSON: " << args.towers_json << '\n';
    }
  }

  if (show_cells) print_snapshot(snap);

  if (!args.out_cells.empty()) {
    if (!write_cells_csv(args.out_cells, snap)) {
      std::cerr << "Failed to write " << args.out_cells << '\n';
      return 1;
    }
    std::cout << "\nwrote " << args.out_cells << '\n';
  }

  if (!args.expect.empty()) {
    auto expect_rows = read_cells_csv(args.expect);
    if (expect_rows.empty()) {
      std::cerr << "Failed to read expect CSV (or empty): " << args.expect << '\n';
      return 1;
    }
    auto d = diff_cells(snap, expect_rows);
    print_expect_summary(d);
  }

  return 0;
}
