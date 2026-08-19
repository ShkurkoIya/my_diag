/// @file Options.h
/// @brief Observer process config: Job + Recipe + I/O. CLI is a projection.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Observer {

/// What the process is doing. GUI Start LTE/WCDMA/IRAT is Survey + Rats.
enum class Job : uint8_t { Listen = 0, Search, Survey };

/// Which RATs the job cares about. Listen+Auto → all DIAG masks.
enum class Rats : uint8_t { Auto = 0, Lte, Wcdma, Irat };

/// Policy knobs. Defaults come from (Job, Rats); CLI flags only override.
struct Recipe {
  bool ghost{false};              ///< COPS=1 onto a fake PLMN (Discover / Search)
  bool foreign_plmn{false};       ///< walk uncamped FULL + PLMN-select (full-walk)
  bool wipe_fplmn{false};         ///< AT+CRSM EF_FPLMN
  bool irat_wcdma{false};         ///< 3G UARFCN|PSC grind after LTE frontier
  bool pin_lte{false};            ///< CNMP=38 / QMI LTE-only
  bool cfun_recover{false};       ///< allow CFUN 4↔1 (USB re-enum risk)
  bool cfun_bounce{false};        ///< legacy hop: CFUN instead of CCELLCFG
  bool band_clip{false};          ///< CSYSSEL lte_band during hop
  bool scan_between_hops{false};  ///< COPS=? between locks (Survey)
  bool periodic_cops{false};      ///< COPS=? on a timer (Listen/Search)
  bool qmi_plmn_search{false};    ///< periodic QMI force-search
  bool enrich_serving{false};     ///< CPSI/CNWINFO poll
  bool deep_search{false};        ///< one-shot COPS=2 before first scan
  bool init_masks{true};
  bool use_qmi{true};

  int search_period_sec{60};
  int hop_dwell_sec{30};
  int hop_max{12};
  int ghost_dwell_sec{45};
  int wcdma_dwell_sec{0};  ///< 0 → copy hop_dwell_sec in finalize
  int cops_act{7};         ///< 7 = E-UTRAN; 2 = UTRAN; -1 = omit
  std::pair<uint16_t, uint16_t> ghost_plmn{999, 99};
};

/// Process / device / sinks — not survey policy.
struct ProcessIo {
  std::optional<std::string> device;
  std::optional<std::string> qmi_path;
  int duration_sec{30};
  bool duration_explicit{false};
  int baud{921600};
  int qmi_period_ms{2000};
  std::optional<std::string> live_json_path{std::string{"/tmp/qcom_live_towers.json"}};
  std::optional<std::string> scanner_log_path{std::string{"/tmp/qcom_live_scanner.log"}};
};

struct Options {
  Job job{Job::Listen};
  Rats rats{Rats::Auto};
  Recipe recipe{};
  ProcessIo io{};
  bool list_only{false};
  bool help_only{false};
  std::string cmdline;

  [[nodiscard]] bool surveying() const noexcept { return job == Job::Survey; }
  [[nodiscard]] bool searching() const noexcept { return job == Job::Search; }
  [[nodiscard]] bool hop_lock() const noexcept { return surveying() && !recipe.cfun_bounce; }
  [[nodiscard]] bool wcdma_only() const noexcept { return rats == Rats::Wcdma; }
};

/// Linger after fake-PLMN COPS=1. Instant ERROR on SIM8300 is expected — full
/// 45s only dumps LL1 B114, not new RF. Cap that miss at 8s.
[[nodiscard]] inline int ghost_linger_sec(bool select_ok, int dwell_sec) noexcept {
  const int d = std::clamp(dwell_sec, 5, 300);
  return select_ok ? d : std::min(d, 8);
}

[[nodiscard]] constexpr std::string_view to_string(Job j) noexcept {
  switch (j) {
    case Job::Listen: return "listen";
    case Job::Search: return "search";
    case Job::Survey: return "survey";
  }
  return "listen";
}

[[nodiscard]] constexpr std::string_view to_string(Rats r) noexcept {
  switch (r) {
    case Rats::Lte: return "lte";
    case Rats::Wcdma: return "wcdma";
    case Rats::Irat: return "irat";
    case Rats::Auto: return "auto";
  }
  return "auto";
}

void print_usage(const char* argv0);

/// 0 = parsed (run / --help / --list), 2 = usage/error (already printed).
[[nodiscard]] int parse_options(int argc, char** argv, Options& out);

}  // namespace Observer
