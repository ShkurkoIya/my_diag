#include "observer/Options.h"

#include "observer/AtParse.h"

#include <CLI/CLI.hpp>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace Observer {

void print_usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " --survey-mode lte|wcdma|irat [options]\n"
      << "       " << argv0 << " [--duration SEC]                 # listen (DIAG+QMI)\n"
      << "       " << argv0 << " --list\n"
      << "\n"
      << "  Product (GUI Start buttons). Implies hop walk until Ctrl+C:\n"
      << "  --survey-mode MODE   lte | wcdma | irat   (alias: --rats)\n"
      << "  --no-ghost           skip fake-PLMN Discover (lte/irat default ON)\n"
      << "  --no-full-walk       incomplete cells only, no foreign PLMN-select\n"
      << "  --no-wcdma-walk      skip IRAT 3G phase (lte already skips it)\n"
      << "  --no-clear-fplmn     do not touch SIM EF_FPLMN\n"
      << "  --recover-cfun       allow CFUN 4↔1 recover (USB re-enum risk)\n"
      << "  --hop-dwell SEC      max seconds per lock (default 30)\n"
      << "  --hop-max N          ranked targets per pass (default 12; wcdma 32)\n"
      << "  --search-period SEC  COPS=? / ghost cadence (default 60)\n"
      << "\n"
      << "  Process:\n"
      << "  --device PATH        DIAG tty (default: catalog)\n"
      << "  --qmi PATH           cdc-wdm (default: catalog)\n"
      << "  --duration SEC       0 = until Ctrl+C (survey default 0)\n"
      << "  --no-qmi             DIAG only\n"
      << "  --no-init            skip DIAG mask init\n"
      << "  --live-json [P]      default /tmp/qcom_live_towers.json\n"
      << "  --no-live-json\n"
      << "  --scanner-log [P]    default /tmp/qcom_live_scanner.log\n"
      << "  --no-scanner-log\n"
      << "\n"
      << "  Legacy aliases (still work; map into Job+Recipe):\n"
      << "  --earfcn-hop  --hop-lock  --hop-cfun  --prefer-lte\n"
      << "  --cops-ghost-plmn [MCCMNC]  --at-cops  --plmn-search  --at-cereg\n"
      << "  --deep-search  --hop-band-clip  --cops-between-hops\n"
      << "  --full-walk  --wcdma-walk  --clear-fplmn  --ghost-dwell  --cops-act\n"
      << "\n"
      << "  Neighbor FULL needs camp on that EARFCN|PCI. Grind holds CCELLCFG until match.\n";
}

static bool parse_rats(std::string_view s, Rats& out) {
  if (s == "lte" || s == "4g") {
    out = Rats::Lte;
    return true;
  }
  if (s == "wcdma" || s == "3g" || s == "umts") {
    out = Rats::Wcdma;
    return true;
  }
  if (s == "irat" || s == "multi" || s == "both") {
    out = Rats::Irat;
    return true;
  }
  return false;
}

int parse_options(int argc, char** argv, Options& out) {
  {
    std::ostringstream cmd;
    cmd << "# cmdline:";
    for (int i = 0; i < argc; ++i) cmd << ' ' << argv[i];
    cmd << '\n';
    out.cmdline = cmd.str();
  }

  CLI::App app{"Observer RF survey"};
  app.set_help_flag();
  app.allow_windows_style_options(false);

  auto& rec = out.recipe;
  auto& io = out.io;

  std::optional<bool> cops_between_hops_flag;
  std::optional<bool> full_walk_flag;
  std::optional<bool> wcdma_walk_flag;
  std::optional<bool> clear_fplmn_flag;
  std::optional<bool> ghost_flag;
  std::vector<std::string> ghost_vals;
  std::vector<std::string> live_json_vals;
  std::vector<std::string> scanner_log_vals;
  std::string survey_mode_str;
  int live_json_mode = 0;
  int scanner_log_mode = 0;
  bool earfcn_hop_flag = false;
  bool prefer_lte_flag = false;
  bool at_cops_flag = false;
  bool at_cereg_flag = false;
  bool plmn_search_set = false;

  app.add_flag("-h,--help", out.help_only);
  app.add_flag("--list", out.list_only);
  app.add_option("--device", io.device);
  app.add_option("--qmi", io.qmi_path);
  app.add_option("--duration", io.duration_sec)->each([&](const std::string&) {
    io.duration_explicit = true;
  });
  app.add_option("--baud", io.baud);
  app.add_option("--qmi-period", io.qmi_period_ms);
  app.add_option("--search-period", rec.search_period_sec);
  app.add_option("--hop-dwell", rec.hop_dwell_sec);
  app.add_option("--hop-max", rec.hop_max);
  app.add_flag("--no-init", [&](std::int64_t) { rec.init_masks = false; });
  app.add_flag("--no-qmi", [&](std::int64_t) { rec.use_qmi = false; });
  app.add_flag("--prefer-lte", prefer_lte_flag);
  app.add_flag("--plmn-search", [&](std::int64_t) {
    rec.qmi_plmn_search = true;
    plmn_search_set = true;
  });
  app.add_flag("--no-plmn-search", [&](std::int64_t) {
    rec.qmi_plmn_search = false;
    plmn_search_set = true;
  });
  app.add_flag("--at-cops", at_cops_flag);
  app.add_flag("--at-cereg", at_cereg_flag);
  app.add_flag("--earfcn-hop", [&](std::int64_t) {
    earfcn_hop_flag = true;
    at_cereg_flag = true;
  });
  app.add_flag("--hop-lock", [&](std::int64_t) {
    rec.cfun_bounce = false;
    earfcn_hop_flag = true;
    at_cereg_flag = true;
  });
  app.add_flag("--hop-cfun", [&](std::int64_t) {
    rec.cfun_bounce = true;
    earfcn_hop_flag = true;
    at_cereg_flag = true;
  });
  app.add_flag("--hop-band-clip", rec.band_clip);
  app.add_flag("--recover-cfun", rec.cfun_recover);
  app.add_flag("--cops-between-hops", [&](std::int64_t) { cops_between_hops_flag = true; });
  app.add_flag("--no-cops-between-hops", [&](std::int64_t) { cops_between_hops_flag = false; });
  auto* ghost_opt = app.add_option("--cops-ghost-plmn", ghost_vals)->expected(0, 1);
  app.add_flag("--no-ghost", [&](std::int64_t) { ghost_flag = false; });
  app.add_option("--ghost-dwell", rec.ghost_dwell_sec);
  app.add_option("--cops-act", rec.cops_act);
  app.add_flag("--full-walk", [&](std::int64_t) { full_walk_flag = true; });
  app.add_flag("--no-full-walk", [&](std::int64_t) { full_walk_flag = false; });
  app.add_flag("--wcdma-walk", [&](std::int64_t) { wcdma_walk_flag = true; });
  app.add_flag("--no-wcdma-walk", [&](std::int64_t) { wcdma_walk_flag = false; });
  auto* wcdma_dwell_opt = app.add_option("--wcdma-dwell", rec.wcdma_dwell_sec);
  app.add_option("--survey-mode,--rats", survey_mode_str);
  app.add_flag("--clear-fplmn", [&](std::int64_t) { clear_fplmn_flag = true; });
  app.add_flag("--no-clear-fplmn", [&](std::int64_t) { clear_fplmn_flag = false; });
  app.add_option("--live-json", live_json_vals)->expected(0, 1);
  app.add_flag("--no-live-json");
  app.add_option("--scanner-log", scanner_log_vals)->expected(0, 1);
  app.add_flag("--no-scanner-log");
  app.add_flag("--deep-search", [&](std::int64_t) {
    rec.deep_search = true;
    at_cops_flag = true;
    rec.qmi_plmn_search = true;
    plmn_search_set = true;
  });

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    std::cerr << e.what() << "\n";
    print_usage(argv[0]);
    return 2;
  }

  for (int i = 1; i < argc; ++i) {
    const std::string_view a{argv[i]};
    if (a == "--live-json") live_json_mode = 1;
    else if (a == "--no-live-json") live_json_mode = -1;
    else if (a == "--scanner-log") scanner_log_mode = 1;
    else if (a == "--no-scanner-log") scanner_log_mode = -1;
  }

  if (out.help_only) {
    print_usage(argv[0]);
    return 0;
  }

  const bool product_mode = !survey_mode_str.empty();
  if (product_mode) {
    if (!parse_rats(survey_mode_str, out.rats)) {
      std::cerr << "Invalid --survey-mode / --rats (want lte|wcdma|irat)\n";
      return 2;
    }
    out.job = Job::Survey;
  }

  if (ghost_opt->count() > 0) {
    ghost_flag = true;
    if (!ghost_vals.empty() && !ghost_vals.front().empty()) {
      auto p = parse_plmn_arg(ghost_vals.front());
      if (!p) {
        std::cerr << "Invalid --cops-ghost-plmn MCCMNC (want 00101 / 25001 / 250-01)\n";
        return 2;
      }
      rec.ghost_plmn = *p;
    }
  }
  if (app.count("--ghost-dwell") > 0) {
    if (rec.ghost_dwell_sec < 5) rec.ghost_dwell_sec = 5;
    if (rec.ghost_dwell_sec > 300) rec.ghost_dwell_sec = 300;
  }
  if (wcdma_dwell_opt->count() > 0) {
    if (rec.wcdma_dwell_sec < 10) rec.wcdma_dwell_sec = 10;
    if (rec.wcdma_dwell_sec > 180) rec.wcdma_dwell_sec = 180;
  }
  if (live_json_mode < 0)
    io.live_json_path = std::nullopt;
  else if (live_json_mode > 0)
    io.live_json_path = live_json_vals.empty() ? std::string{"/tmp/qcom_live_towers.json"}
                                              : live_json_vals.front();
  if (scanner_log_mode < 0)
    io.scanner_log_path = std::nullopt;
  else if (scanner_log_mode > 0)
    io.scanner_log_path = scanner_log_vals.empty() ? std::string{"/tmp/qcom_live_scanner.log"}
                                                   : scanner_log_vals.front();

  if (out.list_only) return 0;

  if (earfcn_hop_flag) out.job = Job::Survey;
  if (out.job == Job::Survey && out.rats == Rats::Auto) out.rats = Rats::Irat;

  if (out.job != Job::Survey && ghost_flag.value_or(false)) {
    out.job = Job::Search;
    if (out.rats == Rats::Auto) out.rats = Rats::Lte;
  }

  rec.periodic_cops = at_cops_flag;
  rec.enrich_serving = at_cereg_flag;
  rec.pin_lte = prefer_lte_flag;

  if (out.job == Job::Survey) {
    if (product_mode) std::cerr << "note: --survey-mode " << to_string(out.rats) << " → survey walk\n";
    rec.enrich_serving = true;
    rec.foreign_plmn = full_walk_flag.value_or(true);
    rec.irat_wcdma = wcdma_walk_flag.value_or(out.rats != Rats::Lte);
    rec.pin_lte = (out.rats != Rats::Wcdma) || prefer_lte_flag;
    if (out.rats == Rats::Wcdma) rec.pin_lte = false;
    rec.ghost = ghost_flag.value_or(product_mode && out.rats != Rats::Wcdma);
    if (cops_between_hops_flag.has_value()) {
      rec.scan_between_hops = *cops_between_hops_flag;
    } else {
      rec.scan_between_hops = true;
      std::cerr << "note: survey → between-hop AT+COPS=? ON (--no-cops-between-hops to disable)\n";
    }
    if (rec.periodic_cops) {
      std::cerr << "note: --at-cops during survey → between-hop COPS=? (not periodic AT flood)\n";
      rec.periodic_cops = false;
      rec.scan_between_hops = true;
    }
    if (!io.duration_explicit) {
      io.duration_sec = 0;
      std::cerr << "note: survey → --duration 0 (until Ctrl+C); pass --duration N to cap\n";
    } else if (io.duration_sec > 0 && io.duration_sec < 180) {
      std::cerr << "warning: --duration " << io.duration_sec
                << "s is short for hop/recover (often needs 60–120s to leave WCDMA)\n";
    }
    if (out.rats == Rats::Wcdma) {
      if (app.count("--hop-max") == 0) rec.hop_max = 32;
      if (app.count("--cops-act") == 0) rec.cops_act = kCopsActUtran;
    }
  } else if (cops_between_hops_flag.value_or(false)) {
    std::cerr << "note: --cops-between-hops needs survey; enabling periodic --at-cops\n";
    rec.periodic_cops = true;
  }

  if (out.job == Job::Search && !io.duration_explicit) {
    io.duration_sec = 0;
    std::cerr << "note: search/ghost → --duration 0 (until Ctrl+C); pass --duration N to cap\n";
  } else if (out.job == Job::Search && io.duration_sec > 0 &&
             io.duration_sec < rec.search_period_sec + rec.ghost_dwell_sec) {
    std::cerr << "warning: --duration " << io.duration_sec << "s is shorter than search-period+dwell ("
              << (rec.search_period_sec + rec.ghost_dwell_sec) << "s) — ghost will be cut mid-flight\n";
  }

  if (out.job != Job::Survey && (rec.ghost || rec.pin_lte)) rec.enrich_serving = true;
  if (out.job == Job::Search) rec.ghost = true;

  rec.wipe_fplmn = clear_fplmn_flag.value_or(rec.foreign_plmn || rec.ghost || out.wcdma_only());
  if (rec.wcdma_dwell_sec <= 0) rec.wcdma_dwell_sec = rec.hop_dwell_sec;
  (void)plmn_search_set;

  if (out.surveying() && rec.foreign_plmn && !out.wcdma_only()) {
    std::cout << "full-walk ON: hop uncamped FULL + PLMN-select for foreign operators\n";
  }
  if (out.surveying() && rec.irat_wcdma) {
    if (out.wcdma_only())
      std::cout << "survey rats=wcdma: CNMP=14 sticky UARFCN|PSC grind (dwell=" << rec.wcdma_dwell_sec
                << "s, full-walk=" << (rec.foreign_plmn ? "ON" : "OFF") << ")\n";
    else
      std::cout << "wcdma-walk ON: after LTE empty → CNMP=14 UARFCN|PSC grind (dwell="
                << rec.wcdma_dwell_sec << "s) → restore LTE\n";
  }
  if (out.job == Job::Survey)
    std::cout << "job=survey rats=" << to_string(out.rats) << "\n";
  else if (out.job == Job::Search)
    std::cout << "job=search (ghost, no cell lock)\n";
  if (rec.wipe_fplmn) {
    std::cout << "clear-fplmn ON: AT+CRSM wipe EF_FPLMN at start + every "
              << std::max(30, rec.search_period_sec) << "s (idle/ghost)\n";
  }
  if (rec.ghost) {
    const std::string gplmn = format_plmn_numeric(rec.ghost_plmn.first, rec.ghost_plmn.second);
    std::cout << "ghost-plmn ON: " << format_cops_manual_select(gplmn, rec.cops_act)
              << " dwell=" << rec.ghost_dwell_sec << "s"
              << (out.surveying() ? " (between-hop / idle only)" : " (periodic)") << "\n";
  }
  if (rec.cops_act >= 0 && (rec.ghost || rec.foreign_plmn)) {
    std::cout << "cops-act=" << rec.cops_act
              << (rec.cops_act == kCopsActLte ? " (E-UTRAN/LTE)" : "") << "\n";
  }
  return 0;
}

}  // namespace Observer
