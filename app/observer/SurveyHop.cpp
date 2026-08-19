#include "observer/SurveyHop.h"

#include "observer/AtParse.h"
#include "observer/Fmt.h"
#include "observer/ServingInject.h"
#include "observer/Signals.h"
#include "observer/WcdmaWalk.h"

#include <observer/engine/SurveyDomain.h>
#include <observer/lte/LteHopPlanner.h>
#include <qcom/at/Cmgrmi.h>
#include <qcom/linux/LinuxSource.h>
#include <qcom/linux/SimcomAtControl.h>
#include <observer/model/Events.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Observer {

using QCom::Engine::SurveyPhase;
using QCom::Lte::cell_is_full_lte;
using QCom::Lte::hop_earfcn_is_tdd;
using QCom::Lte::hop_earfcn_ok;
using QCom::Lte::HopTarget;
using QCom::Lte::HopKey;
using QCom::Lte::pick_neigh_targets;
using QCom::Lte::pick_seed_targets;
using QCom::Lte::measured_intra_hop_keys;
using QCom::Lte::pending_neigh_hop_keys;
using QCom::Lte::serving_neigh_hop_keys;
using QCom::Lte::sib5_bare_earfcns;

void SurveyHop::start() {
  if (!ctx_.opt.surveying()) return;
  if (!ctx_.modem.at) {
    std::cerr << "survey walk needs AT path\n";
    return;
  }
  const auto& rec = ctx_.opt.recipe;
  std::cout << "EARFCN hop: "
            << (ctx_.opt.hop_lock() ? "sticky CCELLCFG grind (hold until target FULL)"
                                    : "CFUN 4↔1 bounce")
            << ", dwell=" << rec.hop_dwell_sec << "s, max targets=" << rec.hop_max
            << (rec.band_clip ? ", band-clip ON" : "")
            << (rec.scan_between_hops
                    ? (", between-hop COPS=? every " +
                       std::to_string(std::max(30, rec.search_period_sec)) + "s")
                    : ", between-hop COPS=? OFF")
            << (rec.foreign_plmn ? ", full-walk ON" : ", full-walk OFF")
            << (rec.irat_wcdma ? ", wcdma-walk ON" : ", wcdma-walk OFF")
            << ", diag-mask search/serving\n";
  g_hop_stop_ptr = &ctx_.hop_stop;
  th_ = std::thread([this] { run(); });
}

void SurveyHop::join() {
  if (th_.joinable()) th_.join();
}

void SurveyHop::run() {
  auto& hop_stop = ctx_.hop_stop;
  auto& dash = *ctx_.dash;
  auto& engine = *ctx_.engine;
  auto& simcom_at = *ctx_.simcom;
  auto* qmi_session = ctx_.qmi;
  auto& diag_alive = ctx_.diag_alive;
  auto& observed_rat_code = ctx_.observed_rat_code;
  auto& deep_dereg_done = ctx_.deep_dereg_done;
  auto& hop_kicks = ctx_.hop_kicks;
  auto& hop_locks = ctx_.hop_locks;
  auto& hop_fulls = ctx_.hop_fulls;
  auto& wcdma_walk_kicks = ctx_.wcdma_walk_kicks;
  auto& wcdma_walk_fulls = ctx_.wcdma_walk_fulls;
  auto& cops_between_kicks = ctx_.cops_between_kicks;
  auto& ghost_kicks = ctx_.ghost_kicks;
  auto& qmi_hop_snaps = ctx_.qmi_hop_snaps;
  auto& survey_phase = ctx_.survey_phase;
  auto& intentional_search = ctx_.intentional_search;
  auto& intentional_wcdma = ctx_.intentional_wcdma;
  auto& saved_lte_bands = ctx_.saved_lte_bands;
  auto& saved_cnmp = ctx_.saved_cnmp;
  auto& last_fplmn_wipe = ctx_.last_fplmn_wipe;
  auto& qmi_status_mu = ctx_.qmi_status_mu;
  auto& qmi_status = ctx_.qmi_status;
  [[maybe_unused]] AtSession* at_sess = ctx_.at ? ctx_.at->session() : nullptr;

  const auto& rec = ctx_.opt.recipe;
  const bool hop_lock = ctx_.opt.hop_lock();
  const bool hop_cfun = rec.cfun_bounce;
  const bool hop_band_clip = rec.band_clip;
  const bool recover_cfun = rec.cfun_recover;
  const bool full_walk = rec.foreign_plmn;
  const bool wcdma_walk = rec.irat_wcdma;
  const bool wcdma_only = ctx_.opt.wcdma_only();
  const bool cops_ghost = rec.ghost;
  const bool cops_between_hops = rec.scan_between_hops;
  const bool prefer_lte = rec.pin_lte;
  const bool clear_fplmn = rec.wipe_fplmn;
  const bool deep_search = rec.deep_search;
  const int hop_dwell_sec = rec.hop_dwell_sec;
  const int hop_max = rec.hop_max;
  const int search_period_sec = rec.search_period_sec;
  const int ghost_dwell_sec = rec.ghost_dwell_sec;
  const int cops_act = rec.cops_act;
  const int wcdma_dwell_sec = rec.wcdma_dwell_sec;
  const auto ghost_plmn = rec.ghost_plmn;

  auto at_do = [&](const char* cmd, int timeout_ms = 2000,
                   AtSession::TickFn tick = {}) -> std::optional<std::string> {
    return ctx_.at_cmd(cmd, timeout_ms, std::move(tick));
  };
  auto inject_serving_identity_once = [&]() -> ServingStamp { return inject_serving_identity(ctx_); };
  auto wipe_fplmn = [&](const char* why, bool force = false) -> bool {
    return ctx_.wipe_fplmn(why, force);
  };
  auto maybe_wipe_fplmn = [&](const char* why) { ctx_.maybe_wipe_fplmn(why); };
  auto rf_lte_alive = [&]() -> bool { return ctx_.rf_lte_alive(); };
  auto ensure_scan_rat = [&](const char* why) -> bool { return ctx_.ensure_scan_rat(why); };

      // Warm-up: let DIAG ML1 populate neighbour RADIO rows before first lock.
      for (int i = 0; i < 100 && !hop_stop.load(std::memory_order_relaxed); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

      auto at_cmd = [&](const char* cmd, int timeout_ms = 2000,
                        AtSession::TickFn tick = {}) -> std::optional<std::string> {
        return at_do(cmd, timeout_ms, std::move(tick));
      };

      // CLI-verified SIM8300 locks (2026-08-10):
      //   LTE:  AT+CCELLCFG=1,<pci>,<earfcn>   + AT+CLECELL=<earfcn>,<pci>
      //         AT+CLEARFCN=<band>,<earfcn>    clear: AT+CLECELL / AT+CLEARFCN / AT+CCELLCFG=0
      //   WCDMA: AT+CLUCELL=<uarfcn>,<psc>    + AT+CLUARFCN=<uarfcn>
      //         (need CNMP=14 / in-WCDMA; else "+CLUCELL: NOT IN WCDMA")
      //         clear: AT+CLUCELL / AT+CLUARFCN
      auto unlock_cell = [&]() {
        (void)simcom_at.apply(QCom::Engine::SurveyIntent::unlock());
        (void)at_cmd("AT+CLUCELL", 2000);
        (void)at_cmd("AT+CLUARFCN", 2000);
      };

      auto send_clecell_lock = [&](uint32_t earfcn, uint16_t pci) -> bool {
        return simcom_at.lock_clecell(earfcn, pci);
      };

      auto send_clearfcn_lock = [&](uint32_t earfcn) -> bool {
        return simcom_at.lock_earfcn(earfcn);
      };

      auto clucell_matches = [&](uint32_t uarfcn, uint16_t psc) -> bool {
        auto raw = at_cmd("AT+CLUCELL?", 1500);
        if (!raw) return false;
        auto pos = raw->find("+CLUCELL:");
        if (pos == std::string::npos) return false;
        // Accept "+CLUCELL: uarfcn,psc" or "NOT IN WCDMA"
        if (raw->find("NOT IN WCDMA") != std::string::npos) return false;
        std::string line = raw->substr(pos + 9);
        if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);
        auto toks = split_csv_tokens(line);
        if (toks.size() < 2) return false;
        try {
          return static_cast<uint32_t>(std::stoul(toks[0])) == uarfcn &&
                 static_cast<uint16_t>(std::stoul(toks[1])) == psc;
        } catch (...) { return false; }
      };

      auto send_clucell_lock = [&](uint32_t uarfcn, uint16_t psc) -> bool {
        if (psc == 0 || psc > 511 || uarfcn == 0 || uarfcn > 16383) return false;
        const std::string cmd = "AT+CLUCELL=" + std::to_string(uarfcn) + "," + std::to_string(psc);
        auto rsp = at_cmd(cmd.c_str(), 3000);
        if (!rsp) return false;
        if (rsp->find("NOT IN WCDMA") != std::string::npos) return false;
        if (!at_reply_ok(*rsp)) return false;
        // ? may ERROR while not camped — treat OK write as lock asserted.
        if (clucell_matches(uarfcn, psc)) return true;
        return true;
      };

      auto send_cluarfcn_lock = [&](uint32_t uarfcn) -> bool {
        if (uarfcn == 0 || uarfcn > 16383) return false;
        const std::string cmd = "AT+CLUARFCN=" + std::to_string(uarfcn);
        auto rsp = at_cmd(cmd.c_str(), 3000);
        if (!rsp) return false;
        if (rsp->find("NOT IN WCDMA") != std::string::npos) return false;
        return at_reply_ok(*rsp);
      };

      /// Back off CMGRMI when modem returns ERROR (NO_SERVICE storms burn AT USB).
      auto cmgrmi_backoff_until = std::chrono::steady_clock::time_point{};
      int last_cmgrmi_nb = 0;
      std::set<HopKey> last_cmgrmi_allow;
      std::set<uint32_t> sib5_hunted;
      bool cnlsa_tried = false;
      std::string last_cmgrmi_raw;
      std::string last_cpsi_raw;
      int last_cmgrmi_inter = 0;

      auto at_one_line = [](std::string_view raw, std::size_t max_n = 240) {
        std::string s;
        s.reserve(std::min(raw.size(), max_n + 4));
        for (unsigned char c : raw) {
          if (c == '\r' || c == '\n') {
            if (!s.empty() && s.back() != '|') s.push_back('|');
          } else if (c >= 32 && c < 127) {
            s.push_back(static_cast<char>(c));
          }
          if (s.size() >= max_n) {
            s += "...";
            break;
          }
        }
        return s;
      };

      auto diag_code_n = [&](QCom::LogCode code) -> uint64_t {
        std::lock_guard lock(ctx_.code_mu);
        auto it = ctx_.code_hist.find(code);
        return it == ctx_.code_hist.end() ? 0 : it->second;
      };

      /// AT+CMGRMI=4 → serving FULL + intra/inter/CA neighbors into tracker (LTE goldmine).
      auto enrich_cmgrmi_lte = [&](const char* tag, bool force = false) -> int {
        // NAS report: RF-lock + NO SERVICE always ERROR. force= must not bypass that.
        if (!cpsi_cmgrmi_ready(last_cpsi_raw)) {
          last_cmgrmi_nb = 0;
          last_cmgrmi_inter = 0;
          last_cmgrmi_raw = "skip:NO_SERVICE";
          return 0;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!force && now < cmgrmi_backoff_until) return 0;
        auto rsp = at_cmd("AT+CMGRMI=4", 3500);
        if (rsp) last_cmgrmi_raw = *rsp;
        else last_cmgrmi_raw.clear();
        if (!rsp || rsp->find("ERROR") != std::string::npos) {
          cmgrmi_backoff_until = now + std::chrono::seconds(4);
          last_cmgrmi_nb = 0;
          last_cmgrmi_inter = 0;
          if (rsp)
            dash.note(std::string("[cmgrmi] LTE ") + tag + " ERROR " + at_one_line(*rsp, 160));
          return 0;
        }
        if (!at_reply_ok(*rsp) && rsp->find("+CMGRMI:") == std::string::npos) {
          cmgrmi_backoff_until = now + std::chrono::seconds(3);
          return 0;
        }
        cmgrmi_backoff_until = {};
        auto snap = QCom::AtCmgrmi::parse_lte(*rsp);
        auto envs = QCom::AtCmgrmi::to_envelopes(snap);
        if (envs.empty()) {
          last_cmgrmi_nb = 0;
          last_cmgrmi_inter = 0;
          return 0;
        }
        const int n_nb = static_cast<int>(snap.neighbors.size());
        const int n_inter = static_cast<int>(snap.inter_carriers.size());
        last_cmgrmi_nb = n_nb;
        last_cmgrmi_inter = n_inter;
        auto keys = QCom::AtCmgrmi::neighbor_hop_keys(snap);
        if (!keys.empty())
          last_cmgrmi_allow = std::move(keys);
        else if (snap.serving.ok)
          last_cmgrmi_allow.erase({snap.serving.earfcn, snap.serving.pci});
        engine.inject_envelopes(std::move(envs));
        if (snap.serving.ok) {
          dash.note(std::string("[cmgrmi] LTE ") + tag + " srv=" +
                    std::to_string(snap.serving.earfcn) + "/" + std::to_string(snap.serving.pci) +
                    " CID=" + std::to_string(snap.serving.cell_id) + " nb=" + std::to_string(n_nb) +
                    " inter=" + std::to_string(n_inter));
        } else {
          dash.note(std::string("[cmgrmi] LTE ") + tag +
                    " (no serving, nb=" + std::to_string(n_nb) + ")");
        }
        return (snap.serving.ok ? 1 : 0) + n_nb;
      };

      auto pull_cmgrmi = [&](int mode, const char* tag) {
        if (mode == 4) {
          (void)enrich_cmgrmi_lte(tag);
          return;
        }
        // Mode 3 (WCDMA) — keep raw OK note until a dedicated parser lands.
        const std::string cmd = "AT+CMGRMI=" + std::to_string(mode);
        auto rsp = at_cmd(cmd.c_str(), 4000);
        if (!rsp || !at_reply_ok(*rsp) || rsp->find("ERROR") != std::string::npos) return;
        dash.note(std::string("[cmgrmi]") + tag + " ok (" + std::to_string(rsp->size()) + "B)");
      };

      auto read_cpsi = [&]() -> std::optional<std::string> {
        auto raw = at_cmd("AT+CPSI?", 1500);
        if (raw) last_cpsi_raw = *raw;
        else last_cpsi_raw.clear();
        return raw;
      };

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

      /// PCI neighbors attached to this EARFCN|PCI (meas + SIB4 + SIB5 neigh_pcis).
      auto pci_neigh_on = [&](uint32_t earfcn, uint16_t pci) -> int {
        for (const auto& c : engine.tracker().get_snapshot()) {
          if (c.rat != QCom::RatType::LTE) continue;
          if (c.radio.freq() != earfcn || c.radio.pci_bsic() != pci) continue;
          int n = static_cast<int>(c.radio.meas_neighbors.size() +
                                   c.radio.intra_freq_neighbors.size());
          for (const auto& car : c.radio.inter_freq_carriers)
            n += static_cast<int>(car.neigh_pcis.size());
          return n;
        }
        return 0;
      };

      /// MIB/B0C2 BW or SFN landed on this key (not just CID).
      auto harvest_mib_on = [&](uint32_t earfcn, uint16_t pci) -> bool {
        for (const auto& c : engine.tracker().get_snapshot()) {
          if (c.rat != QCom::RatType::LTE) continue;
          if (c.radio.freq() != earfcn || c.radio.pci_bsic() != pci) continue;
          const auto* r = c.radio_as_if<QCom::LteRadioParams>();
          if (!r) return false;
          return r->dl_bw != 0 || r->sfn != 0 || r->has_sfn_sf;
        }
        return false;
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

      /// Mark RF key as camped only when we already have a real FULL passport.
      auto mark_camped = [&](uint32_t earfcn, uint16_t pci) -> bool {
        if (!tracker_full_cid(earfcn, pci)) {
          dash.note("[camp] refuse " + std::to_string(earfcn) + "/" + std::to_string(pci) +
                    " — no CID/TAC yet");
          return false;
        }
        QCom::LocalCellKey key{.freq = earfcn, .pci_bsic = pci};
        engine.inject_envelopes({QCom::Events::RrcEventEnvelope{
            .key = key,
            .rat = QCom::RatType::LTE,
            .event_data = QCom::Events::ServingChangedEvent{.is_serving = true},
        }});
        return true;
      };

      /// Done when FULL; full-walk also requires a real camp on that EARFCN|PCI.
      auto target_done = [&](uint32_t earfcn, uint16_t pci) -> std::optional<uint32_t> {
        auto cid = tracker_full_cid(earfcn, pci);
        if (!cid) return std::nullopt;
        if (!full_walk) return cid;
        if (tracker_camped(earfcn, pci)) return cid;
        return std::nullopt;
      };

      // Soft LTE recover: CNMP+COPS. Airplane CFUN only when WCDMA is sticky
      // (NO_SERVICE after a miss is normal — do not bounce RF).
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
            dash.note(std::string("[earfcn-hop] recover ") + phase +
                      " nudge — CPSI=" + cpsi_snip());
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return false;
      };

      auto recover_lte = [&](std::optional<HopTarget> lock_hint = std::nullopt,
                             bool unlock_after = true) {
        unlock_cell();
        if (!diag_alive.load(std::memory_order_relaxed)) return false;
        // Prefer shared rat-guard path (FPLMN wipe + CNMP + CFUN escalate).
        if (ensure_scan_rat("hop-recover") && lte_online()) {
          if (unlock_after) unlock_cell();
          return true;
        }
        if (qmi_session) {
          (void)qmi_session->control().set_mode_preference({QCom::Qmi::Rat::Lte});
        }

        // Fallback if guard could not finish alone.
        last_fplmn_wipe = {};
        (void)wipe_fplmn("recover", /*force=*/true);
        auto raw0 = read_cpsi();
        const bool stuck_wcdma =
            raw0 && raw0->find("WCDMA") != std::string_view::npos && !parse_cpsi_lte(*raw0).ok;
        const bool stuck_nosvc = raw0 && cpsi_is_wcdma_or_noservice(*raw0);

        // Soft: LTE-only + auto PLMN. Shorten wait if already stuck on 3G.
        dash.note(std::string("[earfcn-hop] recover soft: CNMP=38 / COPS=0 / FPLMN wipe") +
                  (stuck_wcdma   ? " (WCDMA sticky)"
                   : stuck_nosvc ? " (no LTE)"
                                 : ""));
        (void)at_cmd("AT+CNMP=38", 10000);
        (void)at_cmd("AT+COPS=0", 10000);
        if (qmi_session) (void)qmi_session->control().force_network_search();
        if (lock_hint && lock_hint->pci != 0 && lock_hint->rsrp >= -110.0f) {
          (void)send_clearfcn_lock(lock_hint->earfcn);
          (void)send_clecell_lock(lock_hint->earfcn, lock_hint->pci);
          const std::string cmd = "AT+CCELLCFG=1," + std::to_string(lock_hint->pci) + "," +
                                  std::to_string(lock_hint->earfcn);
          (void)at_cmd(cmd.c_str(), 2500);
        }
        const int soft_ticks = stuck_wcdma ? 40 : (stuck_nosvc ? 20 : 40);  // 20s / 10s / 20s
        if (wait_lte_online(soft_ticks, "soft")) {
          if (unlock_after) unlock_cell();
          return true;
        }

        // Airplane only when 3G camp blocks LTE. NO_SERVICE after a miss is normal —
        // --recover-cfun must not CFUN 4↔1 there (minutes of Online/no camp).
        const bool do_cfun = stuck_wcdma;
        if (!do_cfun) {
          if (unlock_after) unlock_cell();
          dash.note("[earfcn-hop] soft recover timed out (no CFUN on NO_SERVICE); CPSI=" +
                    cpsi_snip());
          return false;
        }
        dash.note("[earfcn-hop] auto CFUN — WCDMA sticky");

        // Airplane — expect USB re-enumerate + diag reconnect on some SIMCOM.
        dash.note("[earfcn-hop] recover airplane: CFUN=4 → 1 (may USB-reset)");
        (void)at_cmd("AT+CFUN=4", 10000);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        (void)at_cmd("AT+CNMP=38", 10000);
        last_fplmn_wipe = {};
        (void)wipe_fplmn("recover-airplane", /*force=*/true);
        (void)at_cmd("AT+CFUN=1", 10000);
        std::this_thread::sleep_for(std::chrono::milliseconds(3000));
        if (ctx_.at) ctx_.at->reconnect();
        (void)at_cmd("AT+CNMP=38", 10000);
        (void)at_cmd("AT+COPS=0", 10000);
        if (qmi_session) {
          (void)qmi_session->control().set_mode_preference({QCom::Qmi::Rat::Lte});
          (void)qmi_session->control().force_network_search();
        }
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
        auto raw = read_cpsi();
        const bool dropped = raw && cpsi_is_wcdma_or_noservice(*raw);
        if (recover_cooldown.count() > 0 && now - last_recover_fail < recover_cooldown) {
          // RAT drop overrides long cooldown after 15s — don't sit on MegaFon 3G forever.
          if (!dropped || now - last_recover_fail < std::chrono::seconds(15)) {
            dash.note("[earfcn-hop] not LTE — recover cooldown " +
                      std::to_string(recover_cooldown.count()) + "s; CPSI=" + cpsi_snip());
            return false;
          }
          dash.note("[earfcn-hop] cooldown override — RAT drop; CPSI=" + cpsi_snip());
        }
        dash.note(std::string("[earfcn-hop] not LTE — auto recover") +
                  (dropped ? " (RAT drop)" : "") + "; CPSI=" + cpsi_snip());
        if (!hint) hint = best_full_hint();
        std::optional<HopTarget> use_hint;
        if (hint && hint->pci != 0 && hint->rsrp >= -110.0f) use_hint = hint;
        const bool ok = recover_lte(use_hint, /*unlock_after=*/true);
        if (!ok) {
          last_recover_fail = std::chrono::steady_clock::now();
          // Shorter cooldown on WCDMA stickiness so we keep fighting back to LTE.
          recover_cooldown = std::chrono::seconds(dropped ? 20 : 60);
        }
        return ok;
      };

      // Manual PLMN from AT+COPS=1,2,"mccmnc" — must not be undone by COPS=0 during locks.
      // CMSSN pin sets forced_plmn but is NOT COPS=1; skipping COPS=0 then leaves NAS
      // in NO SERVICE and CMGRMI=4 storms ERROR.
      std::optional<std::pair<uint16_t, uint16_t>> forced_plmn;
      bool cops1_held = false;
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
        // CLI-verified: AT+CMSSN=<mccmnc> hard-pins operator (complement to COPS).
        {
          const std::string cmssn = "AT+CMSSN=" + plmn;
          if (auto rsp = at_cmd(cmssn.c_str(), 5000); rsp && at_reply_ok(*rsp))
            dash.note("[full-walk] CMSSN " + plmn);
        }
        const std::string cmd = format_cops_manual_select(plmn, cops_act);
        dash.note("[full-walk] PLMN select " + cmd + "…");
        auto rsp = at_cmd(cmd.c_str(), 120000);
        if (!rsp || !at_reply_ok(*rsp)) {
          dash.note("[full-walk] PLMN select failed for " + plmn);
          // Failed attach often re-fills FPLMN — wipe once and retry select.
          if (clear_fplmn) {
            last_fplmn_wipe = {};  // force immediate wipe
            if (wipe_fplmn("select-fail")) { rsp = at_cmd(cmd.c_str(), 120000); }
          }
          if (!rsp || !at_reply_ok(*rsp)) {
            cops1_held = false;
            forced_plmn.reset();
            (void)at_cmd("AT+CMSSN", 2000);
            (void)at_cmd("AT+COPS=0", 10000);
            return false;
          }
        }
        cops1_held = true;
        forced_plmn = {mcc, mnc};
        for (int i = 0; i < 300 && !hop_stop.load(std::memory_order_relaxed); ++i) {
          if (auto cur = cpsi_plmn(); cur && cur->first == mcc && cur->second == mnc) {
            dash.note("[full-walk] camped on PLMN " + plmn);
            (void)inject_serving_identity_once();
            (void)enrich_cmgrmi_lte("plmn-camp");
            return true;
          }
          if (i > 0 && (i % 40) == 0) { (void)at_cmd(cmd.c_str(), 60000); }
          if (i > 0 && (i % 50) == 0) (void)enrich_cmgrmi_lte("plmn-wait");
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        dash.note("[full-walk] PLMN select timeout for " + plmn);
        return false;
      };

      // ═══════════════════════════════════════════════════════════════════
      // Survey phases:
      //   DISCOVER  — AT+COPS=? → wipe FPLMN → ghost 99999,AcT → listen + CMGRMI=4
      //   COMPLETE  — CLEARFCN+CCELLCFG+CLECELL grind; CMGRMI/QMI soak for intra/inter/CA;
      //               rediscover only on empty frontier or real 3G drop (not every K FULL)
      //   WCDMA     — CNMP=14 sticky UARFCN|PSC grind (CLUCELL / CLUARFCN);
      //               IRAT restores LTE after a wave
      // Misses must NOT count as camps (was burning ~25% session on COPS+ghost loops).
      // ═══════════════════════════════════════════════════════════════════
      std::chrono::steady_clock::time_point last_cops{};
      std::chrono::steady_clock::time_point last_ghost{};
      std::chrono::steady_clock::time_point last_wcdma_walk{};
      uint64_t last_cops_diag_bytes{0};
      uint64_t last_cops_diag_logs{0};
      int camps_since_search{0};
      constexpr int k_camps_per_rediscover = 4;
      constexpr int k_discover_listen_sec = 8;
      /// Soft EARFCN cool after many distinct PCI misses (even if the carrier already has a FULL).
      /// TDD (B40) burns the session if we grind 5 PCI — cool after 2.
      constexpr int k_earfcn_distinct_pci_ban = 5;
      constexpr int k_tdd_earfcn_pci_ban = 2;
      constexpr auto k_earfcn_cooldown = std::chrono::minutes(3);
      constexpr auto k_pci_cooldown = std::chrono::seconds(180);
      constexpr auto k_wcdma_walk_cooldown = std::chrono::seconds(90);
      std::map<uint32_t, std::set<uint16_t>> earfcn_failed_pcis;
      std::map<uint32_t, std::chrono::steady_clock::time_point> earfcn_cooldown;
      std::set<std::pair<uint32_t, uint16_t>> wcdma_done;  // UARFCN|PSC camped FULL
      std::map<std::pair<uint32_t, uint16_t>, std::chrono::steady_clock::time_point> wcdma_cooldown;
      std::map<uint32_t, int> uarfcn_fail_streak;
      std::map<uint32_t, std::chrono::steady_clock::time_point> uarfcn_wcdma_cooldown;
      std::optional<std::string> saved_w_bands;
      int wcdma_camps_since_search{0};
      constexpr int k_uarfcn_fail_ban = 3;
      constexpr auto k_uarfcn_wcdma_cool = std::chrono::minutes(10);
      constexpr auto k_wcdma_psc_cool = std::chrono::seconds(120);
      constexpr int k_wcdma_post_full_sib_sec = 18;

      auto set_phase = [&](SurveyPhase ph) {
        survey_phase.store(static_cast<int>(ph), std::memory_order_relaxed);
        dash.note(sfmt("[survey] PHASE {}", QCom::Engine::to_string(ph)));
      };

      auto count_rf_lte = [&]() -> int {
        int n = 0;
        for (const auto& c : engine.tracker().get_snapshot()) {
          if (c.rat == QCom::RatType::LTE && c.radio.freq() != 0 && c.radio.pci_bsic() != 0 &&
              c.radio.pci_bsic() <= 503)
            ++n;
        }
        return n;
      };

      auto pin_lte4g = [&](const char* why) {
        intentional_wcdma.store(false, std::memory_order_relaxed);
        (void)at_cmd("AT+CNMP=38", 10000);
        if (!cnlsa_tried) {
          cnlsa_tried = true;
          if (auto rsp = at_cmd("AT+CNLSA=1", 2000); rsp && at_reply_ok(*rsp))
            dash.note("[lte-pin] CNLSA=1");
          else
            dash.note("[lte-pin] CNLSA=1 skipped");
        }
        if (!cops1_held) (void)at_cmd("AT+COPS=0", 5000);
        if (qmi_session) {
          (void)qmi_session->control().set_mode_preference({QCom::Qmi::Rat::Lte});
          (void)qmi_session->control().force_network_search();
        }
        dash.note(std::string("[lte-pin] CNMP=38 (") + why + ")");
      };

      auto wipe_fplmn_now = [&](const char* why) {
        last_fplmn_wipe = {};
        (void)wipe_fplmn(why, /*force=*/true);
      };

      auto pin_wcdma3g = [&](const char* why) {
        unlock_cell();
        intentional_wcdma.store(true, std::memory_order_relaxed);
        (void)at_cmd("AT+CNMP=14", 10000);  // WCDMA-only
        if (qmi_session) {
          (void)qmi_session->control().set_mode_preference({QCom::Qmi::Rat::Wcdma});
          (void)qmi_session->control().force_network_search();
        }
        dash.note(std::string("[wcdma-pin] CNMP=14 (") + why + ")");
      };

      auto tracker_full_wcdma = [&](uint32_t uarfcn, uint16_t psc) -> std::optional<uint32_t> {
        for (const auto& c : engine.tracker().get_snapshot()) {
          if (!cell_is_full_wcdma(c)) continue;
          if (c.radio.freq() != uarfcn) continue;
          if (psc != 0 && c.radio.pci_bsic() != psc) continue;
          return c.passport.cell_id;
        }
        return std::nullopt;
      };

      auto count_wcdma_full = [&]() -> int {
        int n = 0;
        for (const auto& c : engine.tracker().get_snapshot()) {
          if (cell_is_full_wcdma(c)) ++n;
        }
        return n;
      };

      auto mark_camped_wcdma = [&](uint32_t uarfcn, uint16_t psc) {
        QCom::LocalCellKey key{.freq = uarfcn, .pci_bsic = psc};
        engine.inject_envelopes({QCom::Events::RrcEventEnvelope{
            .key = key,
            .rat = QCom::RatType::WCDMA,
            .event_data = QCom::Events::ServingChangedEvent{.is_serving = true},
        }});
      };

      /// Primary WCDMA cell lock (CLI-verified AT+CLUCELL); CLUARFCN as freq-only assist.
      auto send_wcdma_cell_lock = [&](uint16_t psc, uint32_t uarfcn) -> bool {
        (void)send_cluarfcn_lock(uarfcn);
        return send_clucell_lock(uarfcn, psc);
      };

      auto clip_w_band_for = [&](uint32_t uarfcn, const char* tag) {
        const std::string clip = band_csv_for_uarfcn(uarfcn);
        const std::string cmd = "AT+CSYSSEL=\"w_band\"," + clip;
        if (auto rsp = at_cmd(cmd.c_str(), 2500); rsp && at_reply_ok(*rsp))
          dash.note(std::string("[wcdma-hop] CSYSSEL w_band → ") + clip + " (" + tag + ")");
      };

      auto restore_w_band = [&]() {
        if (!saved_w_bands || saved_w_bands->empty()) return;
        const std::string cmd = "AT+CSYSSEL=\"w_band\"," + *saved_w_bands;
        (void)at_cmd(cmd.c_str(), 2500);
      };

      auto ensure_plmn_utran = [&](uint16_t mcc, uint16_t mnc) -> bool {
        if (mcc == 0) return true;
        if (auto raw = read_cpsi()) {
          auto w = parse_cpsi_wcdma(*raw);
          if (w.ok && w.mcc == mcc && w.mnc == mnc) {
            forced_plmn = {mcc, mnc};
            return true;
          }
        }
        unlock_cell();
        (void)at_cmd("AT+COPS=3,2", 2000);
        const std::string plmn = format_plmn_numeric(mcc, mnc);
        const std::string cmd = format_cops_manual_select(plmn, kCopsActUtran);
        dash.note("[wcdma-hop] PLMN select " + cmd + "…");
        auto rsp = at_cmd(cmd.c_str(), 90000);
        if (!rsp || !at_reply_ok(*rsp)) {
          dash.note("[wcdma-hop] PLMN select failed for " + plmn);
          if (clear_fplmn) {
            last_fplmn_wipe = {};
            if (wipe_fplmn("wcdma-select-fail")) rsp = at_cmd(cmd.c_str(), 90000);
          }
          if (!rsp || !at_reply_ok(*rsp)) {
            cops1_held = false;
            forced_plmn.reset();
            (void)at_cmd("AT+COPS=0", 10000);
            return false;
          }
        }
        forced_plmn = {mcc, mnc};
        return true;
      };

      auto pick_wcdma_frontier = [&](std::size_t max_n) -> std::vector<WcdmaHopTarget> {
        auto targets = pick_wcdma_walk_targets(engine.tracker().get_snapshot(), max_n);
        const auto now = std::chrono::steady_clock::now();
        std::vector<WcdmaHopTarget> todo;
        todo.reserve(targets.size());
        for (const auto& t : targets) {
          if (t.psc == 0 || t.psc > 511) continue;  // need concrete PSC for grind
          if (wcdma_done.contains({t.uarfcn, t.psc})) continue;
          if (tracker_full_wcdma(t.uarfcn, t.psc) && !full_walk) {
            wcdma_done.insert({t.uarfcn, t.psc});
            continue;
          }
          if (full_walk && tracker_full_wcdma(t.uarfcn, t.psc)) {
            // Still need a real camp in full-walk — keep unless already marked done.
          }
          if (auto it = uarfcn_wcdma_cooldown.find(t.uarfcn);
              it != uarfcn_wcdma_cooldown.end() && now - it->second < k_uarfcn_wcdma_cool)
            continue;
          if (auto it = wcdma_cooldown.find({t.uarfcn, t.psc});
              it != wcdma_cooldown.end() && now - it->second < k_wcdma_psc_cool)
            continue;
          todo.push_back(t);
        }
        return todo;
      };

      // Local QMI enrich (LTE hop defines pull_qmi_enrich later in this thread).
      auto pull_qmi_w = [&](const char* /*why*/) {
        if (!qmi_session) return;
        ++qmi_hop_snaps;
        if (auto snap = qmi_session->nas().snapshot_cells(); snap) {
          auto envs = QCom::Qmi::to_rrc_envelopes(snap.value());
          if (!envs.empty()) engine.inject_envelopes(std::move(envs));
        }
        if (auto st = qmi_session->nas().snapshot_status(); st) {
          std::lock_guard qlock(qmi_status_mu);
          qmi_status = std::move(st.value());
        }
      };

      /// Sticky UARFCN|PSC grind — LTE CCELLCFG pipeline counterpart for 3G.
      /// Returns true on FULL+camp for the requested target.
      auto grind_wcdma_target = [&](const WcdmaHopTarget& t) -> bool {
        if (t.psc == 0 || hop_stop.load(std::memory_order_relaxed)) return false;
        ++wcdma_walk_kicks;

        // Skip measured-weak; SIB6 ghosts (~-155/-158) are allowed.
        if (t.rscp > -159.0f && t.rscp < -118.0f && t.rscp > -154.0f) {
          dash.note("[wcdma-hop] skip weak " + std::to_string(t.uarfcn) + "/" +
                    std::to_string(t.psc) + " RSCP=" + std::to_string(static_cast<int>(t.rscp)));
          return false;
        }

        unlock_cell();
        clip_w_band_for(t.uarfcn, "grind");

        if (full_walk && t.mcc != 0) {
          if (!ensure_plmn_utran(t.mcc, t.mnc)) {
            dash.note("[wcdma-hop] PLMN " + format_plmn_numeric(t.mcc, t.mnc) +
                      " select failed — RF-grind " + std::to_string(t.uarfcn) + "/" +
                      std::to_string(t.psc) + " anyway");
          }
        }

        bool hard_lock = false;
        if (hop_lock) {
          // Must be in WCDMA for CLUCELL (else NOT IN WCDMA). Pin + brief wait.
          pin_wcdma3g("pre-clucell");
          for (int w = 0; w < 30 && !hop_stop.load(std::memory_order_relaxed); ++w) {
            if (auto raw = read_cpsi()) {
              if (raw->find("WCDMA") != std::string_view::npos) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          if (send_wcdma_cell_lock(t.psc, t.uarfcn)) {
            hard_lock = true;
            dash.note("[wcdma-hop] CLUCELL grind " + std::to_string(t.uarfcn) + "/" +
                      std::to_string(t.psc));
            if (!forced_plmn) (void)at_cmd("AT+COPS=0", 3000);
          } else {
            dash.note("[wcdma-hop] CLUCELL miss/NOT-IN-WCDMA — soft grind " +
                      std::to_string(t.uarfcn) + "/" + std::to_string(t.psc) +
                      " (w_band + search)");
            if (!forced_plmn) (void)at_cmd("AT+COPS=0", 10000);
            if (recover_cfun || hop_cfun) {
              (void)at_cmd("AT+CFUN=4", 10000);
              std::this_thread::sleep_for(std::chrono::milliseconds(1200));
              (void)at_cmd("AT+CNMP=14", 10000);
              (void)at_cmd("AT+CFUN=1", 10000);
              if (ctx_.at) ctx_.at->reconnect();
              pin_wcdma3g("soft-grind");
              clip_w_band_for(t.uarfcn, "post-cfun");
              if (forced_plmn) (void)ensure_plmn_utran(forced_plmn->first, forced_plmn->second);
              // Retry CLUCELL once we forced WCDMA RF path.
              for (int w = 0; w < 40 && !hop_stop.load(std::memory_order_relaxed); ++w) {
                if (auto raw = read_cpsi(); raw && raw->find("WCDMA") != std::string_view::npos)
                  break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
              }
              if (send_wcdma_cell_lock(t.psc, t.uarfcn)) {
                hard_lock = true;
                dash.note("[wcdma-hop] CLUCELL grind " + std::to_string(t.uarfcn) + "/" +
                          std::to_string(t.psc) + " (post-cfun)");
              }
            }
          }
        } else {
          dash.note("[wcdma-hop] CFUN bounce toward UARFCN " + std::to_string(t.uarfcn));
          (void)at_cmd("AT+CFUN=4", 3000);
          std::this_thread::sleep_for(std::chrono::seconds(2));
          (void)at_cmd("AT+CNMP=14", 10000);
          (void)at_cmd("AT+CFUN=1", 3000);
          (void)at_cmd("AT+COPS=0", 3000);
        }

        if (qmi_session) (void)qmi_session->control().force_network_search();
        pull_qmi_w("wcdma-post-lock");

        const int dwell = std::max(25, wcdma_dwell_sec);
        bool target_stamped = false;
        bool left_wcdma = false;
        uint32_t stamped_cid = 0;
        int stamped_at_i = -1;

        for (int i = 0; i < dwell * 10 && !hop_stop.load(std::memory_order_relaxed); ++i) {
          if (i > 0 && (i % 10) == 0) {
            auto raw = read_cpsi();
            if (raw && raw->find("LTE") != std::string_view::npos && parse_cpsi_lte(*raw).ok) {
              dash.note("[wcdma-hop] left WCDMA during grind — abort + re-pin");
              left_wcdma = true;
              pin_wcdma3g("grind-lte-escape");
              break;
            }

            (void)inject_serving_identity_once();

            if (!target_stamped) {
              if (raw) {
                auto w = parse_cpsi_wcdma(*raw);
                if (w.ok && w.uarfcn == t.uarfcn && w.psc == t.psc && w.cell_id != 0 &&
                    w.lac != 0) {
                  mark_camped_wcdma(t.uarfcn, t.psc);
                  target_stamped = true;
                  stamped_cid = w.cell_id;
                  stamped_at_i = i;
                  dash.note(std::string(full_walk ? "[wcdma-full]" : "[wcdma-hop]") + " FULL on " +
                            std::to_string(t.uarfcn) + "/" + std::to_string(t.psc) +
                            " CID=" + std::to_string(stamped_cid) + " — soaking SIB/neigh " +
                            std::to_string(k_wcdma_post_full_sib_sec) + "s");
                } else if (w.ok && w.cell_id != 0 && w.lac != 0) {
                  // Opportunistic: camped FULL elsewhere — bank it, keep waiting for target.
                  if (!wcdma_done.contains({w.uarfcn, w.psc})) {
                    wcdma_done.insert({w.uarfcn, w.psc});
                    mark_camped_wcdma(w.uarfcn, w.psc);
                    wcdma_walk_fulls.fetch_add(1, std::memory_order_relaxed);
                    dash.note("[wcdma-hop] opportunistic FULL " + std::to_string(w.uarfcn) + "/" +
                              std::to_string(w.psc) + " CID=" + std::to_string(w.cell_id));
                  }
                }
              }
              if (!target_stamped) {
                if (auto cid = tracker_full_wcdma(t.uarfcn, t.psc)) {
                  // Prefer CPSI match; tracker FULL on target is enough when locked/soft.
                  mark_camped_wcdma(t.uarfcn, t.psc);
                  target_stamped = true;
                  stamped_cid = *cid;
                  stamped_at_i = i;
                  dash.note(std::string(full_walk ? "[wcdma-full]" : "[wcdma-hop]") + " FULL on " +
                            std::to_string(t.uarfcn) + "/" + std::to_string(t.psc) +
                            " CID=" + std::to_string(stamped_cid) + " — soaking SIB/neigh " +
                            std::to_string(k_wcdma_post_full_sib_sec) + "s");
                }
              }
            } else if (stamped_at_i >= 0 && (i - stamped_at_i) >= k_wcdma_post_full_sib_sec * 10) {
              break;
            }

            if ((i % 50) == 0) {
              pull_qmi_w("wcdma-grind");
              pull_cmgrmi(3, " wcdma-grind");
            }
            if (hard_lock && (i % 50) == 0 && !clucell_matches(t.uarfcn, t.psc)) {
              dash.note("[wcdma-hop] CLUCELL dropped — re-lock " + std::to_string(t.uarfcn) + "/" +
                        std::to_string(t.psc));
              if (!send_wcdma_cell_lock(t.psc, t.uarfcn)) {
                left_wcdma = true;
                break;
              }
            }
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        unlock_cell();

        if (target_stamped) {
          wcdma_done.insert({t.uarfcn, t.psc});
          wcdma_walk_fulls.fetch_add(1, std::memory_order_relaxed);
          ++wcdma_camps_since_search;
          uarfcn_fail_streak[t.uarfcn] = 0;
          wcdma_cooldown.erase({t.uarfcn, t.psc});
          dash.note(std::string(full_walk ? "[wcdma-full]" : "[wcdma-hop]") + " FULL+camp on " +
                    std::to_string(t.uarfcn) + "/" + std::to_string(t.psc) +
                    " CID=" + std::to_string(stamped_cid) +
                    " (sib-soak done, rf_w=" + std::to_string(count_wcdma_full()) + ")");
          pull_qmi_w("wcdma-full");
          return true;
        }

        if (!left_wcdma) {
          dash.note("[wcdma-hop] grind timeout — no FULL for " + std::to_string(t.uarfcn) + "/" +
                    std::to_string(t.psc) +
                    (tracker_full_wcdma(t.uarfcn, t.psc) ? " (had CID, no camp)" : ""));
        }
        wcdma_cooldown[{t.uarfcn, t.psc}] = std::chrono::steady_clock::now();
        const int streak = ++uarfcn_fail_streak[t.uarfcn];
        if (streak >= k_uarfcn_fail_ban) {
          uarfcn_wcdma_cooldown[t.uarfcn] = std::chrono::steady_clock::now();
          uarfcn_fail_streak[t.uarfcn] = 0;
          dash.note("[wcdma-hop] UARFCN " + std::to_string(t.uarfcn) + " banned 10min after " +
                    std::to_string(k_uarfcn_fail_ban) + " grind misses (ghost PSCs)");
        }
        return false;
      };

      /// IRAT / shared wave: pin WCDMA, grind frontier UARFCN|PSC like LTE complete queue.
      auto run_wcdma_walk = [&](const char* why) {
        if (!wcdma_walk || hop_stop.load(std::memory_order_relaxed)) return;
        if (!wcdma_only) {
          const auto now = std::chrono::steady_clock::now();
          if (last_wcdma_walk.time_since_epoch().count() != 0 &&
              now - last_wcdma_walk < k_wcdma_walk_cooldown) {
            dash.note(std::string("[wcdma-walk] cooldown — skip (") + why + ")");
            return;
          }
          last_wcdma_walk = now;
        }

        set_phase(SurveyPhase::Wcdma);
        const int before_full = count_wcdma_full();
        cops1_held = false;
        forced_plmn.reset();
        pin_wcdma3g(why);
        maybe_wipe_fplmn("pre-wcdma-walk");
        if (!saved_w_bands) {
          if (auto raw = at_cmd("AT+CSYSSEL=\"w_band\"", 2000))
            saved_w_bands = parse_csyssel_named_band_list(*raw, "w_band");
        }

        auto targets = pick_wcdma_frontier(
            static_cast<std::size_t>(std::max(8, full_walk ? std::max(hop_max, 32) : hop_max)));
        if (targets.empty()) {
          dash.note(std::string("[wcdma-hop] frontier empty — free dwell (") + why + ")");
          ++wcdma_walk_kicks;
          (void)at_cmd("AT+COPS=0", 10000);
          const int dwell = std::max(15, wcdma_dwell_sec);
          for (int i = 0; i < dwell * 10 && !hop_stop.load(std::memory_order_relaxed); ++i) {
            if ((i % 10) == 0) {
              (void)inject_serving_identity_once();
              if (auto raw = read_cpsi()) {
                auto w = parse_cpsi_wcdma(*raw);
                if (w.ok && w.cell_id != 0) {
                  wcdma_done.insert({w.uarfcn, w.psc});
                  mark_camped_wcdma(w.uarfcn, w.psc);
                }
              }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
        } else {
          // Sticky: one strongest target per IRAT wave (wcdma_only loops outside).
          const std::size_t n_kick =
              wcdma_only ? 1
                         : (hop_lock ? std::min<std::size_t>(targets.size(), 4) : targets.size());
          for (std::size_t ti = 0; ti < n_kick && !hop_stop.load(std::memory_order_relaxed); ++ti) {
            (void)grind_wcdma_target(targets[ti]);
            pin_wcdma3g("between-wcdma-grind");
          }
        }

        const int gained = count_wcdma_full() - before_full;
        dash.note(std::string("[wcdma-hop] done (") + why +
                  ") Δfull=" + std::to_string(std::max(0, gained)) +
                  " total=" + std::to_string(count_wcdma_full()) +
                  " done_keys=" + std::to_string(wcdma_done.size()) +
                  " kicks=" + std::to_string(wcdma_walk_kicks.load()));

        if (wcdma_only) {
          pin_wcdma3g("wcdma-survey-hold");
          wipe_fplmn_now("after-wcdma-grind");
          (void)inject_serving_identity_once();
          set_phase(SurveyPhase::Wcdma);
          return;
        }

        restore_w_band();
        pin_lte4g("after-wcdma-walk");
        wipe_fplmn_now("after-wcdma-walk");
        for (int i = 0; i < 100 && !hop_stop.load(std::memory_order_relaxed); ++i) {
          if (lte_online() || rf_lte_alive()) break;
          if ((i % 25) == 24) pin_lte4g("after-wcdma-wait");
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        (void)inject_serving_identity_once();
        set_phase(SurveyPhase::Complete);
      };

      auto recover_after_search = [&](const char* tag) {
        // Keep CMSSN pin — resetting forced_plmn used to COPS=0 then COPS=1 ERROR.
        unlock_cell();
        pin_lte4g(tag);
        const std::string wipe_tag = std::string(tag) + "-fplmn";
        wipe_fplmn_now(wipe_tag.c_str());
        for (int i = 0; i < 80 && !hop_stop.load(std::memory_order_relaxed); ++i) {
          if (lte_online() || rf_lte_alive()) break;
          if ((i % 20) == 19) pin_lte4g(tag);
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (forced_plmn) {
          const std::string plmn = format_plmn_numeric(forced_plmn->first, forced_plmn->second);
          (void)at_cmd(("AT+CMSSN=" + plmn).c_str(), 5000);
          dash.note(sfmt("[full-walk] CMSSN {} (after-search)", plmn));
        }
        (void)inject_serving_identity_once();
      };

      auto* linux_src = dynamic_cast<QCom::LinuxSource*>(engine.source());
      auto want_diag = [&](QCom::LteDiagPack pack, const char* why) {
        if (!linux_src) return;
        if (linux_src->wanted_lte_diag_pack() == pack) return;
        linux_src->request_lte_diag_pack(pack);
        dash.note(std::string("[diag-mask] ") + why + " → " + QCom::lte_diag_pack_name(pack));
      };

      const std::vector<std::pair<uint16_t, uint16_t>> k_oper_wave{
          {250, 1}, {250, 2}, {250, 20}, {250, 99}};
      const std::vector<uint16_t> k_lte_wave{1, 3, 7, 8, 20, 28, 38, 40, 41};
      std::size_t oper_wave_i = 0;
      std::size_t band_wave_i = 0;

      auto pin_cmssn_only = [&](uint16_t mcc, uint16_t mnc) {
        unlock_cell();
        const std::string plmn = format_plmn_numeric(mcc, mnc);
        const std::string cmd = "AT+CMSSN=" + plmn;
        (void)at_cmd(cmd.c_str(), 5000);
        forced_plmn = {mcc, mnc};
        dash.note(sfmt("[full-walk] CMSSN {} (oper-wave)", plmn));
      };

      auto apply_lte_band_one = [&](uint16_t band, const char* why) {
        const std::string cmd = "AT+CSYSSEL=\"lte_band\"," + std::to_string(band);
        if (auto rsp = at_cmd(cmd.c_str(), 2500); rsp && at_reply_ok(*rsp))
          dash.note(sfmt("[earfcn-hop] CSYSSEL lte_band={} ({})", band, why));
        else
          dash.note(sfmt("[earfcn-hop] CSYSSEL lte_band={} failed ({})", band, why));
      };

      auto restore_lte_bands = [&](const char* why) {
        if (!saved_lte_bands || saved_lte_bands->empty()) return;
        const std::string cmd = "AT+CSYSSEL=\"lte_band\"," + *saved_lte_bands;
        if (auto rsp = at_cmd(cmd.c_str(), 2500); rsp && at_reply_ok(*rsp))
          dash.note(sfmt("[earfcn-hop] CSYSSEL lte_band restore ({})", why));
        else
          dash.note(sfmt("[earfcn-hop] CSYSSEL lte_band restore failed ({})", why));
      };

      auto listen_for_cmgrmi = [&](int sec, const char* why) {
        want_diag(QCom::LteDiagPack::Search, why);
        dash.note(sfmt("[survey] listen {}s ({})", sec, why));
        for (int i = 0; i < sec * 10 && !hop_stop.load(std::memory_order_relaxed); ++i) {
          if (i > 0 && (i % 20) == 0) {
            (void)inject_serving_identity_once();
            (void)enrich_cmgrmi_lte(why);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        (void)inject_serving_identity_once();
        (void)enrich_cmgrmi_lte(why, /*force=*/true);
      };

      auto advance_wave = [&]() -> bool {
        // Clip one band only to listen for new RF, then restore so CCELLCFG can lock any EARFCN.
        if (hop_lock && hop_band_clip && band_wave_i < k_lte_wave.size()) {
          unlock_cell();
          apply_lte_band_one(k_lte_wave[band_wave_i], "wave");
          ++band_wave_i;
          last_cmgrmi_allow.clear();
          listen_for_cmgrmi(5, "band-wave");
          restore_lte_bands("after-band-wave");
          return true;
        }
        if (full_walk && oper_wave_i + 1 < k_oper_wave.size()) {
          ++oper_wave_i;
          band_wave_i = 0;
          unlock_cell();
          restore_lte_bands("oper-wave");
          pin_cmssn_only(k_oper_wave[oper_wave_i].first, k_oper_wave[oper_wave_i].second);
          last_cmgrmi_allow.clear();
          return true;
        }
        return false;
      };

      auto run_ghost_now = [&](const char* why) {
        if (hop_stop.load(std::memory_order_relaxed)) return;
        want_diag(QCom::LteDiagPack::Search, "ghost");
        if (!cops_ghost || hop_stop.load(std::memory_order_relaxed)) {
          if (!cops_ghost) dash.note("[ghost] skipped — pass --cops-ghost-plmn for discover ghost");
          return;
        }
        const int dwell = ghost_dwell_sec;
        unlock_cell();
        last_ghost = std::chrono::steady_clock::now();
        cops1_held = false;
        forced_plmn.reset();
        pin_lte4g("pre-ghost");
        wipe_fplmn_now("pre-ghost");
        const std::string plmn = format_plmn_numeric(ghost_plmn.first, ghost_plmn.second);
        const std::string cmd = format_cops_manual_select(plmn, cops_act);
        ++ghost_kicks;
        dash.note(std::string("[ghost] ") + cmd + " (" + why + ", #" +
                  std::to_string(ghost_kicks.load()) + ", dwell=" + std::to_string(dwell) + "s)…");

        {
          intentional_search.store(true, std::memory_order_relaxed);
          struct GhostFlag {
            std::atomic<bool>& f;
            ~GhostFlag() { f.store(false, std::memory_order_relaxed); }
          } ghost_flag{intentional_search};

          (void)at_cmd("AT+COPS=3,2", 2000);
          auto rsp = at_cmd(cmd.c_str(), 60000);
          const bool ok = rsp && at_reply_ok(*rsp);
          const int linger = ghost_linger_sec(ok, dwell);
          if (ok)
            dash.note("[ghost] select accepted — lingering " + std::to_string(linger) + "s");
          else
            dash.note("[ghost] select ERROR/timeout (expected) — linger " +
                      std::to_string(linger) + "s (not full dwell)");

          auto* linux = dynamic_cast<QCom::LinuxSource*>(engine.source());
          const uint64_t bytes0 = linux ? linux->bytes_raw() : 0;
          const uint64_t logs0 = linux ? linux->logs_delivered() : 0;
          for (int i = 0; i < linger * 10 && !hop_stop.load(std::memory_order_relaxed); ++i) {
            if (linux && i > 0 && (i % 50) == 0) {
              const uint64_t db = linux->bytes_raw() - bytes0;
              const uint64_t dl = linux->logs_delivered() - logs0;
              dash.note("[ghost] search " + std::to_string(i / 10) + "s — DIAG +" +
                        std::to_string(db) + "B / +" + std::to_string(dl) + " logs");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          {
            const uint64_t db = linux ? linux->bytes_raw() - bytes0 : 0;
            const uint64_t dl = linux ? linux->logs_delivered() - logs0 : 0;
            dash.note(std::string("[ghost] done (") + why + ", DIAG +" + std::to_string(db) +
                      "B / +" + std::to_string(dl) + " logs)");
          }
        }
        recover_after_search("[ghost]");
      };

      auto run_cops_now = [&](const char* why) {
        if (hop_stop.load(std::memory_order_relaxed)) return;
        want_diag(QCom::LteDiagPack::Search, "cops");
        unlock_cell();
        last_cops = std::chrono::steady_clock::now();
        ++cops_between_kicks;
        pin_lte4g("pre-cops");
        wipe_fplmn_now("pre-cops");
        dash.note(std::string("[hop-cops] AT+COPS=? (") + why + ", #" +
                  std::to_string(cops_between_kicks.load()) + ")…");

        {
          intentional_search.store(true, std::memory_order_relaxed);
          struct SearchFlag {
            std::atomic<bool>& f;
            ~SearchFlag() { f.store(false, std::memory_order_relaxed); }
          } search_flag{intentional_search};

          if (deep_search && !deep_dereg_done.exchange(true)) {
            if (auto rsp = at_cmd("AT+COPS=2", 8000); rsp && at_reply_ok(*rsp)) {
              dash.note("[hop-cops] AT+COPS=2 once (deregister)");
            } else {
              dash.note("[hop-cops] AT+COPS=2 failed");
            }
            for (int i = 0; i < 30 && !hop_stop.load(std::memory_order_relaxed); ++i)
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }

          auto* linux = dynamic_cast<QCom::LinuxSource*>(engine.source());
          const uint64_t bytes0 = linux ? linux->bytes_raw() : 0;
          const uint64_t logs0 = linux ? linux->logs_delivered() : 0;
          auto rsp = at_cmd("AT+COPS=?", 180000, [&](int elapsed_ms, std::string_view) {
            if (!linux || elapsed_ms < 1000 || (elapsed_ms % 5000) >= 1000) return;
            const uint64_t b = linux->bytes_raw() - bytes0;
            const uint64_t l = linux->logs_delivered() - logs0;
            dash.note("[hop-cops] waiting " + std::to_string(elapsed_ms / 1000) + "s — DIAG +" +
                      std::to_string(b) + "B / +" + std::to_string(l) + " logs (still collecting)");
          });
          last_cops_diag_bytes = linux ? linux->bytes_raw() - bytes0 : 0;
          last_cops_diag_logs = linux ? linux->logs_delivered() - logs0 : 0;
          if (rsp) {
            auto found = parse_cops_plmn_list(*rsp);
            for (const auto& p : found) cops_seen_plmns.insert(p);
            dash.note("[hop-cops] done (plmns=" + std::to_string(found.size()) +
                      " seen_total=" + std::to_string(cops_seen_plmns.size()) + ", " + why +
                      ", DIAG +" + std::to_string(last_cops_diag_bytes) + "B / +" +
                      std::to_string(last_cops_diag_logs) +
                      " logs, rf_lte=" + std::to_string(count_rf_lte()) + ")");
          } else {
            dash.note(std::string("[hop-cops] timeout/fail (") + why + ", DIAG +" +
                      std::to_string(last_cops_diag_bytes) + "B / +" +
                      std::to_string(last_cops_diag_logs) + " logs)");
          }
        }
        recover_after_search("[hop-cops]");
        camps_since_search = 0;
      };

      /// Discover cycle: COPS=? → (ghost only on initial discover) → listen. No CCELLCFG.
      auto run_discover = [&](const char* why) {
        dash.note(std::string("[survey] DISCOVER start (") + why + ")");
        pin_lte4g(why);
        wipe_fplmn_now(why);
        (void)inject_serving_identity_once();
        run_cops_now(why);
        // Ghost after fat COPS is usually dead air (~2KB). Keep it for the first discover
        // only; rediscover is COPS-only so complete can keep camping.
        const bool initial = (std::string_view(why) == "discover");
        if (cops_ghost && initial)
          run_ghost_now(why);
        else if (cops_ghost)
          dash.note(std::string("[ghost] skipped on rediscover (") + why + ")");
        else
          dash.note("[survey] no --cops-ghost-plmn — skip ghost leg");

        dash.note(std::string("[survey] discover listen ") + std::to_string(k_discover_listen_sec) +
                  "s unlocked (rf_lte=" + std::to_string(count_rf_lte()) + ")");
        auto* linux = dynamic_cast<QCom::LinuxSource*>(engine.source());
        const uint64_t bytes0 = linux ? linux->bytes_raw() : 0;
        const uint64_t logs0 = linux ? linux->logs_delivered() : 0;
        for (int i = 0; i < k_discover_listen_sec * 10 && !hop_stop.load(std::memory_order_relaxed);
             ++i) {
          if (i > 0 && (i % 20) == 0) {
            (void)inject_serving_identity_once();
            (void)enrich_cmgrmi_lte("discover-listen");
            if (linux) {
              dash.note("[survey] listen " + std::to_string(i / 10) + "s — DIAG +" +
                        std::to_string(linux->bytes_raw() - bytes0) + "B / +" +
                        std::to_string(linux->logs_delivered() - logs0) +
                        " logs rf=" + std::to_string(count_rf_lte()));
            }
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        pin_lte4g("discover-done");
        (void)inject_serving_identity_once();
        (void)enrich_cmgrmi_lte("discover-done");
        dash.note(std::string("[survey] DISCOVER done (") + why +
                  ") rf_lte=" + std::to_string(count_rf_lte()) + " copsDIAG=+" +
                  std::to_string(last_cops_diag_bytes) + "B");
      };

      auto after_camp_policy = [&](const char* why, bool force_rediscover, bool full_success) {
        pin_lte4g(why);
        (void)inject_serving_identity_once();
        if (full_success) (void)enrich_cmgrmi_lte("after-full");
        if (full_success) ++camps_since_search;
        if (force_rediscover) {
          dash.note(std::string("[survey] rediscover (") + why + ")");
          set_phase(SurveyPhase::Rediscover);
          run_discover(why);
          set_phase(SurveyPhase::Complete);
          sib5_hunted.clear();
        } else if (full_success) {
          dash.note(sfmt("[survey] complete queue — {} FULL this discover, next camp (no COPS=?)",
                         camps_since_search));
        }
        // Misses: stay in complete, try next target (PCI cooldown handles thrash).
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

      // ── WCDMA-only survey: DISCOVER → COMPLETE UARFCN|PSC grind (LTE pipeline twin) ──
      if (wcdma_only) {
        set_phase(SurveyPhase::Wcdma);
        dash.note("[survey] WCDMA-only: DISCOVER (COPS=?) → COMPLETE sticky UARFCN|PSC grind");
        intentional_wcdma.store(true, std::memory_order_relaxed);
        pin_wcdma3g("wcdma-survey-start");
        if (!saved_w_bands) {
          if (auto raw = at_cmd("AT+CSYSSEL=\"w_band\"", 2000))
            saved_w_bands = parse_csyssel_named_band_list(*raw, "w_band");
        }

        auto wcdma_discover = [&](const char* why) {
          wipe_fplmn_now("wcdma-cops");
          intentional_wcdma.store(true, std::memory_order_relaxed);
          pin_wcdma3g("pre-cops");
          if (auto rsp = at_cmd("AT+COPS=?", 180000); rsp && at_reply_ok(*rsp)) {
            ++cops_between_kicks;
            dash.note(std::string("[wcdma-survey] AT+COPS=? done (") + why + ", #" +
                      std::to_string(cops_between_kicks.load()) + ")");
          } else {
            dash.note(std::string("[wcdma-survey] AT+COPS=? timeout/fail (") + why + ")");
          }
          // Seed RF: free camp + short listen (like LTE discover listen).
          (void)at_cmd("AT+COPS=0", 10000);
          if (qmi_session) (void)qmi_session->control().force_network_search();
          for (int i = 0;
               i < k_discover_listen_sec * 10 && !hop_stop.load(std::memory_order_relaxed); ++i) {
            if ((i % 10) == 0) (void)inject_serving_identity_once();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          wcdma_camps_since_search = 0;
        };

        wcdma_discover("discover");
        dash.note("[survey] WCDMA COMPLETE: UARFCN|PSC queue (COPS=? every " +
                  std::to_string(k_camps_per_rediscover) + " FULL camps / empty frontier)");

        while (!hop_stop.load(std::memory_order_relaxed)) {
          if (!diag_alive.load(std::memory_order_relaxed)) {
            dash.note("[wcdma-hop] paused — waiting for DIAG USB reconnect");
            for (int i = 0; i < 20 && !hop_stop.load(std::memory_order_relaxed); ++i)
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
          }

          intentional_wcdma.store(true, std::memory_order_relaxed);
          // Keep CNMP=14 — rat-guard must not yank us to LTE.
          pin_wcdma3g("complete-hold");

          auto targets = pick_wcdma_frontier(
              static_cast<std::size_t>(std::max(1, full_walk ? std::max(hop_max, 32) : hop_max)));
          if (targets.empty()) {
            dash.note("[wcdma-hop] frontier empty — rediscover");
            wcdma_discover("idle-empty");
            for (int i = 0; i < 30 && !hop_stop.load(std::memory_order_relaxed); ++i)
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
          }

          const WcdmaHopTarget& t = targets.front();
          const bool ok = grind_wcdma_target(t);
          pin_wcdma3g(ok ? "after-full" : "after-miss");

          if (ok && wcdma_camps_since_search >= k_camps_per_rediscover) {
            dash.note("[survey] wcdma queue " + std::to_string(wcdma_camps_since_search) + "/" +
                      std::to_string(k_camps_per_rediscover) + " FULL — rediscover");
            wcdma_discover("between-camps");
          } else if (!ok) {
            // Brief pause so DIAG/neigh lists can refresh between misses.
            for (int i = 0; i < 20 && !hop_stop.load(std::memory_order_relaxed); ++i)
              std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
        }

        intentional_wcdma.store(false, std::memory_order_relaxed);
        unlock_cell();
        restore_w_band();
        (void)at_cmd("AT+CNMP=54", 10000);
        (void)at_cmd("AT+COPS=0", 3000);
        return;
      }

      // ── Phase 1: DISCOVER ─────────────────────────────────────────────
      set_phase(SurveyPhase::Discover);
      run_discover("discover");
      set_phase(SurveyPhase::Complete);
      dash.note("[survey] COMPLETE: seed=COPS=? RF (new FDD carrier first; QMI FULL does not skip "
                "that); serving-neigh only after a real CCELLCFG FULL; no CFUN after lock");
      if (full_walk) {
        pin_cmssn_only(k_oper_wave[0].first, k_oper_wave[0].second);
      }

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

      auto send_ccell_lock = [&](const HopTarget& t, bool invite_nas = true) -> bool {
        // Dual-lock dialect lives in SimcomAtControl (CLEARFCN → CCELLCFG → CLECELL).
        // Do not CFUN 4→1 here: it hung AT ~30s and dropped lock (session 202718).
        const auto st = simcom_at.apply(QCom::Engine::SurveyIntent::lock(t.earfcn, t.pci));
        // COPS=0 after lock lets NAS camp on the sticky cell. Skip only when COPS=1
        // was actually accepted (CMSSN pin must not block this).
        if (invite_nas && !cops1_held) (void)at_cmd("AT+COPS=0", 3000);
        return st == QCom::Engine::ControlStatus::Ok || simcom_at.lte_lock_held(t.earfcn, t.pci);
      };

      auto lte_lock_held = [&](const HopTarget& t) -> bool {
        return simcom_at.lte_lock_held(t.earfcn, t.pci);
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
        (void)at_cmd("AT+CNMP=38", 5000);
        // Manual LTE scan style: lock EARFCN|PCI immediately — do not wait for CPSI Online.
        if (send_ccell_lock(t) && lte_lock_held(t)) {
          ++hop_locks;
          dash.note("[earfcn-hop] CLECELL+CCELLCFG grind " + std::to_string(t.earfcn) + "/" +
                    std::to_string(t.pci) + " (#" + std::to_string(hop_kicks.load()) + ")");
          return true;
        }
        dash.note("[earfcn-hop] LTE lock first miss " + std::to_string(t.earfcn) + "/" +
                  std::to_string(t.pci) + " — soft pin + retry");
        (void)at_cmd("AT+COPS=0", 5000);
        if (qmi_session) (void)qmi_session->control().force_network_search();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        if (send_ccell_lock(t) && lte_lock_held(t)) {
          ++hop_locks;
          dash.note("[earfcn-hop] CLECELL+CCELLCFG grind " + std::to_string(t.earfcn) + "/" +
                    std::to_string(t.pci) + " (#" + std::to_string(hop_kicks.load()) + ", retry)");
          return true;
        }
        // Last resort recover is FDD-only: TDD CCELLCFG never camps, CFUN won't help.
        if (hop_earfcn_is_tdd(t.earfcn)) {
          dash.note("[earfcn-hop] LTE lock failed for " + std::to_string(t.earfcn) + "/" +
                    std::to_string(t.pci) + " (TDD, skip recover)");
          unlock_cell();
          return false;
        }
        if (!recover_lte(t, /*unlock_after=*/false)) {
          dash.note("[earfcn-hop] LTE lock failed for " + std::to_string(t.earfcn) + "/" +
                    std::to_string(t.pci));
          unlock_cell();
          return false;
        }
        if (!send_ccell_lock(t) || !lte_lock_held(t)) {
          dash.note("[earfcn-hop] LTE lock mismatch for " + std::to_string(t.earfcn) + "/" +
                    std::to_string(t.pci));
          unlock_cell();
          return false;
        }
        ++hop_locks;
        dash.note("[earfcn-hop] CLECELL+CCELLCFG grind " + std::to_string(t.earfcn) + "/" +
                  std::to_string(t.pci) + " (#" + std::to_string(hop_kicks.load()) +
                  ", post-recover)");
        return true;
      };

      // Extra NAS cell-location + serving-system/signal pulls around each hop (like qmicli).
      // Does not force_network_search — that stays once after lock.
      auto pull_qmi_enrich = [&](const char* /*why*/) {
        if (!qmi_session) return;
        ++qmi_hop_snaps;
        if (auto snap = qmi_session->nas().snapshot_cells(); snap) {
          auto envs = QCom::Qmi::to_rrc_envelopes(snap.value());
          if (!envs.empty()) engine.inject_envelopes(std::move(envs));
        }
        if (auto st = qmi_session->nas().snapshot_status(); st) {
          std::lock_guard qlock(qmi_status_mu);
          qmi_status = std::move(st.value());
        }
      };

      std::map<std::pair<uint32_t, uint16_t>, std::chrono::steady_clock::time_point> cooldown;

      auto is_weak_measured = [](float rsrp) {
        // Measured but hopelessly weak — skip unless it's a SIB ghost seed (−155).
        return rsrp > -154.0f && rsrp < -118.0f;
      };

      auto earfcn_has_full = [&](uint32_t earfcn) -> bool {
        for (const auto& c : engine.tracker().get_snapshot()) {
          if (c.rat != QCom::RatType::LTE || c.radio.freq() != earfcn) continue;
          if (cell_is_full_lte(c)) return true;
        }
        return false;
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

        // Clockwork survey: pick frontier first. Ghost/CFUN only when no LTE targets.
        // CPSI NO_SERVICE is normal while ML1/QMI already hear LTE.
        {
          auto raw = read_cpsi();
          if (raw) {
            const ObservedRat obs = observe_rat_from_cpsi(*raw);
            observed_rat_code.store(static_cast<int>(obs), std::memory_order_relaxed);
            if ((obs == ObservedRat::Wcdma || obs == ObservedRat::Gsm) && !rf_lte_alive() &&
                !intentional_wcdma.load(std::memory_order_relaxed)) {
              dash.note("[earfcn-hop] CPSI 3G + no LTE RF — pin CNMP");
              (void)at_cmd("AT+CNMP=38", 10000);
              (void)at_cmd("AT+COPS=0", 5000);
              if (qmi_session) {
                (void)qmi_session->control().set_mode_preference({QCom::Qmi::Rat::Lte});
                (void)qmi_session->control().force_network_search();
              }
            }
          }
        }

        const auto now = std::chrono::steady_clock::now();
        const std::size_t frontier_n =
            static_cast<std::size_t>(std::max(1, full_walk ? std::max(hop_max, 32) : hop_max));
        const auto snap = engine.tracker().get_snapshot();
        auto from_srv = serving_neigh_hop_keys(snap);
        auto from_meas = measured_intra_hop_keys(snap);
        auto allow = last_cmgrmi_allow;
        allow.insert(from_srv.begin(), from_srv.end());
        allow.insert(from_meas.begin(), from_meas.end());
        const auto pending_neigh = pending_neigh_hop_keys(snap);
        const bool neigh_q = !allow.empty();
        // QMI measured TDD (src=qmi-meas) must not steal the first kicks: COPS=? FDD
        // seeds first, neighbour whitelist only after we actually CCELLCFG-camped.
        const bool stay_on_neigh = neigh_q && hop_fulls.load() > 0;
        auto usable_of = [&](std::vector<HopTarget> raw, bool from_neigh) {
          if (!hop_lock) return raw;
          std::vector<HopTarget> usable;
          for (const auto& t : raw) {
            if (t.pci == 0 || !hop_earfcn_ok(t.earfcn)) continue;
            if (!from_neigh && is_weak_measured(t.rsrp)) continue;
            if (full_walk) {
              if (target_done(t.earfcn, t.pci)) continue;  // FULL+camped
            } else if (tracker_full_cid(t.earfcn, t.pci)) {
              continue;
            }
            // Neighbours of a camped cell: PCI cooldown only — do not 3min-ban the carrier.
            if (!from_neigh) {
              auto ecd = earfcn_cooldown.find(t.earfcn);
              if (ecd != earfcn_cooldown.end() && now - ecd->second < k_earfcn_cooldown) continue;
            }
            auto cd = cooldown.find({t.earfcn, t.pci});
            if (cd != cooldown.end() && now - cd->second < k_pci_cooldown) continue;
            usable.push_back(t);
          }
          return usable;
        };
        auto targets =
            usable_of(stay_on_neigh ? pick_neigh_targets(snap, allow, frontier_n, full_walk)
                                    : pick_seed_targets(snap, frontier_n, full_walk),
                      stay_on_neigh);
        if (targets.empty() && stay_on_neigh)
          targets = usable_of(pick_seed_targets(snap, frontier_n, full_walk), /*from_neigh=*/false);
        if (targets.empty()) {
          if (!pending_neigh.empty()) {
            dash.note(sfmt("[earfcn-hop] {} neigh not kickable now — wave/next (no idle)",
                           pending_neigh.size()));
          }
          // Inspector SIB5 often has EARFCN-only (no PCI). Pin the carrier, let SSS mint, then grind.
          bool hunting = false;
          for (uint32_t e : sib5_bare_earfcns(snap)) {
            if (sib5_hunted.contains(e)) continue;
            auto ecd = earfcn_cooldown.find(e);
            if (ecd != earfcn_cooldown.end() && now - ecd->second < k_earfcn_cooldown) continue;
            sib5_hunted.insert(e);
            unlock_cell();
            if (simcom_at.lock_earfcn(e)) {
              dash.note(sfmt("[earfcn-hop] SIB5 hunt CLEARFCN {} (no PCI yet)", e));
              listen_for_cmgrmi(8, "sib5-hunt");
            } else {
              dash.note(sfmt("[earfcn-hop] SIB5 hunt CLEARFCN {} failed", e));
            }
            hunting = true;
            break;
          }
          if (hunting) continue;
          if (advance_wave()) continue;
          dash.note("[survey] COMPLETE frontier empty — wcdma-walk then rediscover");
          (void)inject_serving_identity_once();
          run_wcdma_walk("lte-frontier-empty");
          set_phase(SurveyPhase::Rediscover);
          run_discover("idle-empty");
          set_phase(SurveyPhase::Complete);
          oper_wave_i = 0;
          band_wave_i = 0;
          sib5_hunted.clear();
          last_cmgrmi_allow.clear();
          if (full_walk) pin_cmssn_only(k_oper_wave[0].first, k_oper_wave[0].second);
          if (hop_lock && hop_band_clip) restore_lte_bands("after-rediscover");
          for (int i = 0; i < 50 && !hop_stop.load(std::memory_order_relaxed); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }

        // Sticky grind: one strongest target per pass — hold lock until FULL or timeout.
        // CFUN bounce: walk the ranked list (imprecise).
        const std::size_t n_kick = hop_lock ? 1 : targets.size();
        for (std::size_t ti = 0; ti < n_kick; ++ti) {
          const HopTarget& t = targets[ti];
          if (hop_stop.load(std::memory_order_relaxed)) break;
          ++hop_kicks;
          const HopKey kick_key{t.earfcn, t.pci};
          const char* kick_src = "seed";
          if (last_cmgrmi_allow.contains(kick_key))
            kick_src = "cmgrmi";
          else if (from_srv.contains(kick_key))
            kick_src = "serving-neigh";
          else if (from_meas.contains(kick_key))
            kick_src = "qmi-meas";
          const bool stay_kick = std::string_view(kick_src) != "seed";
          const uint64_t b0c0_0 = diag_code_n(0xB0C0);
          const uint64_t b115_0 = diag_code_n(0xB115);

          want_diag(QCom::LteDiagPack::Search, "grind");

          // Outer CMSSN wave already pinned a home PLMN. Do not COPS=1 per target
          // (SIM8300 ERROR + extra-PLMN 250-21/22 used to stall 60s).
          if (full_walk && hop_lock && t.mcc != 0 && !forced_plmn) {
            if (!ensure_plmn(t.mcc, t.mnc)) {
              dash.note("[full-walk] PLMN " + format_plmn_numeric(t.mcc, t.mnc) +
                        " select failed — RF-lock " + std::to_string(t.earfcn) + "/" +
                        std::to_string(t.pci) + " anyway");
            }
          }

          if (hop_lock) {
            if (!ccell_lock(t)) {
              cooldown[{t.earfcn, t.pci}] = std::chrono::steady_clock::now();
              dash.note(sfmt("[kick] #{} {}/{} src={} full=0 cid=0 lock=fail qmi={}", hop_kicks.load(),
                             t.earfcn, t.pci, kick_src, qmi_session ? 1 : 0));
              continue;
            }
          } else {
            cfun_bounce(t.earfcn);
          }

          // Soft QMI nudge only — never AT+COPS=? here. Then enrich cell list + NAS status.
          if (qmi_session) (void)qmi_session->control().force_network_search();
          pull_qmi_enrich("post-lock");
          (void)read_cpsi();
          (void)enrich_cmgrmi_lte("post-lock", /*force=*/true);

          // Cap miss dwell: ghosts shouldn't burn full hop_dwell (often 40s).
          // Intra/CMGRMI/QMI kicks soak longer — 15s early-out was missing SIB1 on 6275/133.
          const int miss_dwell =
              !hop_lock ? std::max(8, hop_dwell_sec)
              : stay_kick ? std::max(24, std::min(hop_dwell_sec, 36))
                          : std::max(18, std::min(hop_dwell_sec, 28));
          int post_full_sib_sec = (last_cmgrmi_nb >= 2) ? 12 : 18;
          bool target_stamped = false;
          bool left_lte = false;
          bool relocked = false;
          uint32_t stamped_cid = 0;
          int stamped_at_i = -1;

          auto soak_ready = [&](int i) -> bool {
            if (stamped_at_i < 0) return false;
            const int soaked = i - stamped_at_i;
            const int pci_n = pci_neigh_on(t.earfcn, t.pci);
            const bool mib = harvest_mib_on(t.earfcn, t.pci);
            // 8s + MIB + PCI neigh → enough. Empty SIB4 is valid after full soak.
            if (soaked >= 80 && mib && pci_n > 0) return true;
            if (soaked >= post_full_sib_sec * 10 && (mib || pci_n > 0)) return true;
            return false;
          };

          // Instant win: CMGRMI/post-lock already minted FULL on target.
          if (auto cid = tracker_full_cid(t.earfcn, t.pci)) {
            if (mark_camped(t.earfcn, t.pci)) {
              target_stamped = true;
              stamped_cid = *cid;
              stamped_at_i = 0;
              dash.note(std::string(full_walk ? "[full-walk]" : "[earfcn-hop]") + " FULL on " +
                        std::to_string(t.earfcn) + "/" + std::to_string(t.pci) +
                        " CID=" + std::to_string(stamped_cid) + " — soaking MIB/SIB/CMGRMI up to " +
                        std::to_string(post_full_sib_sec) + "s (post-lock)");
              (void)enrich_cmgrmi_lte("full-start", /*force=*/true);
              want_diag(QCom::LteDiagPack::Serving, "soak");
            }
          }

          const int hard_ticks = std::max(miss_dwell, hop_dwell_sec) * 10;
          for (int i = 0; i < hard_ticks && !hop_stop.load(std::memory_order_relaxed); ++i) {
            // Poll identity ~1 Hz; keep CCELLCFG held the whole time.
            if (i > 0 && (i % 10) == 0) {
              auto raw = read_cpsi();
              // NO_SERVICE during CCELLCFG grind is OK (common). Abort only on real 3G camp.
              if (raw && !parse_cpsi_lte(*raw).ok &&
                  (raw->find("WCDMA") != std::string_view::npos ||
                   raw->find("GSM") != std::string_view::npos)) {
                dash.note("[earfcn-hop] left LTE during grind — abort + auto recover");
                left_lte = true;
                break;
              }

              (void)inject_serving_identity_once();

              if (!target_stamped) {
                if (raw) {
                  auto cpsi = parse_cpsi_lte(*raw);
                  if (cpsi.ok && cpsi.earfcn == t.earfcn && cpsi.pci == t.pci &&
                      QCom::Utils::valid_lte_eci(cpsi.cell_id) &&
                      QCom::Utils::valid_lte_tac(cpsi.tac)) {
                    if (mark_camped(t.earfcn, t.pci)) {
                      target_stamped = true;
                      stamped_cid = cpsi.cell_id;
                      stamped_at_i = i;
                      if (last_cmgrmi_nb >= 2) post_full_sib_sec = 12;
                      dash.note(std::string(full_walk ? "[full-walk]" : "[earfcn-hop]") +
                                " FULL on " + std::to_string(t.earfcn) + "/" +
                                std::to_string(t.pci) + " CID=" + std::to_string(stamped_cid) +
                                " — soaking MIB/SIB/CMGRMI up to " +
                                std::to_string(post_full_sib_sec) + "s");
                      (void)enrich_cmgrmi_lte("full-start", /*force=*/true);
              want_diag(QCom::LteDiagPack::Serving, "soak");
                    }
                  }
                }

                // Held CLECELL/CCELLCFG + FULL on target → success (marks camped for full-walk).
                if (!target_stamped) {
                  const bool locked_on_target = !hop_lock || lte_lock_held(t);
                  if (locked_on_target) {
                    if (auto cid = tracker_full_cid(t.earfcn, t.pci)) {
                      if (mark_camped(t.earfcn, t.pci)) {
                        target_stamped = true;
                        stamped_cid = *cid;
                        stamped_at_i = i;
                        if (last_cmgrmi_nb >= 2) post_full_sib_sec = 12;
                        dash.note(std::string(full_walk ? "[full-walk]" : "[earfcn-hop]") +
                                  " FULL on " + std::to_string(t.earfcn) + "/" +
                                  std::to_string(t.pci) + " CID=" + std::to_string(stamped_cid) +
                                  " — soaking MIB/SIB/CMGRMI up to " +
                                  std::to_string(post_full_sib_sec) + "s");
                        (void)enrich_cmgrmi_lte("full-start", /*force=*/true);
              want_diag(QCom::LteDiagPack::Serving, "soak");
                      }
                    }
                  }
                }

                // Ghost miss early-out: no FULL after 15s → don't burn hop_dwell.
                // Advertised/measured neighbours get the full miss_dwell (SIB1 is slow).
                if (!target_stamped && !stay_kick && i >= 150) {
                  dash.note("[earfcn-hop] early miss " + std::to_string(t.earfcn) + "/" +
                            std::to_string(t.pci) + " — no FULL in 15s");
                  break;
                }
                // Lock registers held but radio never retuned (typical TDD 375): no SIB1.
                if (!target_stamped && hop_lock && i >= 80) {
                  const uint64_t db =
                      (diag_code_n(0xB0C0) - b0c0_0) + (diag_code_n(0xB115) - b115_0);
                  if (db == 0 && last_cpsi_raw.find("NO SERVICE") != std::string::npos) {
                    dash.note("[earfcn-hop] lock RF dead " + std::to_string(t.earfcn) + "/" +
                              std::to_string(t.pci) + " — no DIAG in 8s, miss");
                    break;
                  }
                }
              } else if (soak_ready(i)) {
                const int pci_n = pci_neigh_on(t.earfcn, t.pci);
                const bool mib = harvest_mib_on(t.earfcn, t.pci);
                (void)enrich_cmgrmi_lte("soak-end", /*force=*/true);
                dash.note("[earfcn-hop] soak done " + std::to_string(t.earfcn) + "/" +
                          std::to_string(t.pci) + " pci_nb=" + std::to_string(pci_n) +
                          (mib ? " mib=1" : " mib=0"));
                break;
              } else if (stamped_at_i >= 0 &&
                         (i - stamped_at_i) >= std::min(hop_dwell_sec, post_full_sib_sec + 10) * 10) {
                // Hard soak cap: MIB/SIB never landed.
                (void)enrich_cmgrmi_lte("soak-end", /*force=*/true);
                dash.note("[earfcn-hop] soak timeout " + std::to_string(t.earfcn) + "/" +
                          std::to_string(t.pci) + " pci_nb=" +
                          std::to_string(pci_neigh_on(t.earfcn, t.pci)) +
                          (harvest_mib_on(t.earfcn, t.pci) ? " mib=1" : " mib=0"));
                break;
              }

              // QMI + CMGRMI during grind; denser while hunting FULL / soaking.
              if ((i % 50) == 0) pull_qmi_enrich("grind");
              if (target_stamped) {
                if ((i % 20) == 0) (void)enrich_cmgrmi_lte("soak");
              } else if ((i % 20) == 0) {
                // 2s CMGRMI while hunting — often stamps FULL before CPSI Online.
                (void)enrich_cmgrmi_lte("grind");
              }

              // Lock dropped? Re-assert once without unlocking.
              if (hop_lock && (i % 50) == 0 && !lte_lock_held(t)) {
                dash.note("[earfcn-hop] LTE lock dropped — re-lock " + std::to_string(t.earfcn) +
                          "/" + std::to_string(t.pci));
                if (!send_ccell_lock(t, /*invite_nas=*/false)) {
                  if (!relocked) {
                    relocked = true;
                    if (hop_earfcn_is_tdd(t.earfcn) ||
                        !recover_lte(t, /*unlock_after=*/false) || !lte_lock_held(t)) {
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

          want_diag(QCom::LteDiagPack::Search, "after-kick");

          if (target_stamped) {
            ++hop_fulls;
            (void)mark_camped(t.earfcn, t.pci);
            dash.note(std::string(full_walk ? "[full-walk]" : "[earfcn-hop]") + " FULL+camp on " +
                      std::to_string(t.earfcn) + "/" + std::to_string(t.pci) +
                      " CID=" + std::to_string(stamped_cid) +
                      " (pci_nb=" + std::to_string(pci_neigh_on(t.earfcn, t.pci)) +
                      ", rf_lte=" + std::to_string(count_rf_lte()) + ")");
            pull_qmi_enrich("full");
            cooldown.erase({t.earfcn, t.pci});
            earfcn_failed_pcis.erase(t.earfcn);
          } else if (!left_lte) {
            if (auto stamp = inject_serving_identity_once(); stamp == ServingStamp::Full) {
              if (auto cid = tracker_full_cid(t.earfcn, t.pci)) {
                if (mark_camped(t.earfcn, t.pci)) {
                  ++hop_fulls;
                  target_stamped = true;
                  dash.note(std::string(full_walk ? "[full-walk]" : "[earfcn-hop]") +
                            " FULL+camp on " + std::to_string(t.earfcn) + "/" +
                            std::to_string(t.pci) + " CID=" + std::to_string(*cid) +
                            " (late stamp)");
                  pull_qmi_enrich("full");
                  cooldown.erase({t.earfcn, t.pci});
                  earfcn_failed_pcis.erase(t.earfcn);
                }
              } else {
                dash.note("[earfcn-hop] grind timeout — serving FULL elsewhere, not " +
                          std::to_string(t.earfcn) + "/" + std::to_string(t.pci));
              }
            } else {
              dash.note(
                  "[earfcn-hop] grind timeout — no FULL for " + std::to_string(t.earfcn) + "/" +
                  std::to_string(t.pci) +
                  (full_walk && tracker_full_cid(t.earfcn, t.pci) ? " (had CID, no camp)" : ""));
            }
          }
          if (hop_lock && !target_stamped) {
            cooldown[{t.earfcn, t.pci}] = std::chrono::steady_clock::now();
            earfcn_failed_pcis[t.earfcn].insert(t.pci);
            const auto n_bad = earfcn_failed_pcis[t.earfcn].size();
            const int ban = hop_earfcn_is_tdd(t.earfcn) ? k_tdd_earfcn_pci_ban
                                                       : k_earfcn_distinct_pci_ban;
            const uint64_t miss_b0c0 = diag_code_n(0xB0C0) - b0c0_0;
            const bool rf_dead = miss_b0c0 == 0;
            if (n_bad >= static_cast<std::size_t>(ban)) {
              bool neigh_left = stay_kick;
              if (!neigh_left) {
                for (const auto& k : pending_neigh) {
                  if (k.first == t.earfcn) {
                    neigh_left = true;
                    break;
                  }
                }
              }
              // Dead TDD (lock held, no SIB1) must not pin the walk on those PCI.
              if (neigh_left && !(hop_earfcn_is_tdd(t.earfcn) && rf_dead)) {
                dash.note(sfmt("[earfcn-hop] EARFCN {} skip soft-cool — {} PCI misses, neigh pending",
                               t.earfcn, n_bad));
              } else {
                earfcn_cooldown[t.earfcn] = std::chrono::steady_clock::now();
                dash.note(sfmt("[earfcn-hop] EARFCN {} soft-cool 3min — {} distinct PCI misses{}{}",
                               t.earfcn, n_bad,
                               earfcn_has_full(t.earfcn) ? " (already FULL)" : " (no FULL yet)",
                               hop_earfcn_is_tdd(t.earfcn) ? " (TDD)" : ""));
              }
            }
          }

          {
            const uint64_t db0 = diag_code_n(0xB0C0) - b0c0_0;
            const uint64_t db1 = diag_code_n(0xB115) - b115_0;
            const bool lock_held = hop_lock && simcom_at.lte_lock_held(t.earfcn, t.pci);
            auto cpsi = read_cpsi();
            dash.note(sfmt(
                "[kick] #{} {}/{} src={} full={} cid={} lock={} cpsi={} cmgrmi_nb={} inter={} "
                "diag=B0C0:+{} B115:+{} qmi={} cmgrmi={}",
                hop_kicks.load(), t.earfcn, t.pci, kick_src, target_stamped ? 1 : 0, stamped_cid,
                lock_held ? "held" : "drop", at_one_line(cpsi.value_or(""), 90), last_cmgrmi_nb,
                last_cmgrmi_inter, db0, db1, qmi_session ? 1 : 0,
                last_cmgrmi_raw.empty() ? "-" : at_one_line(last_cmgrmi_raw, 180)));
          }

          if (hop_lock) {
            unlock_cell();
            if (full_walk && forced_plmn)
              pin_cmssn_only(forced_plmn->first, forced_plmn->second);
            // No CFUN on miss — rediscover only on real 3G drop or empty frontier.
            const bool force_rd = left_lte && !rf_lte_alive();
            if (force_rd) wipe_fplmn_now("after-3g");
            after_camp_policy(
                target_stamped ? "after-full" : (force_rd ? "after-3g" : "after-miss"), force_rd,
                target_stamped);
          } else {
            after_camp_policy("after-cfun", /*force_rediscover=*/false, /*full_success=*/false);
          }
        }
      }

      unlock_cell();
      intentional_wcdma.store(false, std::memory_order_relaxed);
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
            {QCom::Qmi::Rat::Lte, QCom::Qmi::Rat::Wcdma});
      }
      (void)at_cmd("AT+CFUN=1", 3000);
      (void)at_cmd("AT+COPS=0", 3000);
      (void)inject_serving_identity_once();

}

}  // namespace Observer
