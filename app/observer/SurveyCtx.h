#pragma once

#include "observer/AtBus.h"
#include "observer/Dashboard.h"
#include "observer/ModemSelect.h"
#include "observer/Options.h"
#include "observer/AtParse.h"
#include "observer/WcdmaWalk.h"

#include <observer/engine/SurveyDomain.h>
#include <observer/engine/SurveyProjection.h>
#include <observer/model/CellIdentity.h>
#include <observer/model/Types.h>
#include <observer/model/Utils.h>
#include <qcom/io/ScannerEngine.h>
#include <qcom/linux/LinuxSource.h>
#include <qcom/linux/SimcomAtControl.h>
#include <qcom/qmi/Qmi.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace Observer {

/// Shared survey process state. Workers take a reference; Runtime owns the instance.
struct SurveyCtx {
  Options opt;
  SelectedModem modem;

  QCom::RadioScannerEngine* engine = nullptr;
  LiveDashboard* dash = nullptr;
  AtBus* at = nullptr;
  QCom::Engine::SimcomAtControl* simcom = nullptr;
  QCom::Qmi::Session* qmi = nullptr;

  std::atomic<int> updates{0};
  std::atomic<int> cereg_ok{0};
  std::atomic<int> cpsi_ok{0};
  std::atomic<int> cnw_ok{0};
  std::atomic<int> hop_kicks{0};
  std::atomic<int> hop_locks{0};
  std::atomic<int> hop_fulls{0};
  std::atomic<int> wcdma_walk_kicks{0};
  std::atomic<int> wcdma_walk_fulls{0};
  std::atomic<int> cops_between_kicks{0};
  std::atomic<int> ghost_kicks{0};
  std::atomic<int> fplmn_wipes{0};
  std::atomic<int> qmi_hop_snaps{0};
  std::atomic<bool> scan_rat_ok{true};
  std::atomic<bool> rat_recover_busy{false};
  std::atomic<bool> rat_guard_stop{false};
  std::atomic<bool> intentional_search{false};
  std::atomic<bool> intentional_wcdma{false};
  std::atomic<int> rat_guard_trips{0};
  std::atomic<int> observed_rat_code{0};
  std::atomic<int> survey_phase{static_cast<int>(QCom::Engine::SurveyPhase::Init)};
  std::atomic<bool> hop_stop{false};
  std::atomic<bool> qmi_stop{false};
  std::atomic<bool> cereg_stop{false};
  std::atomic<bool> at_only_stop{false};
  std::atomic<int> qmi_polls{0};
  std::atomic<int> qmi_ok{0};
  std::atomic<int> search_kicks{0};
  std::atomic<bool> deep_dereg_done{false};
  std::atomic<bool> diag_alive{true};
  std::atomic<bool> diag_needs_reconnect{false};
  std::atomic<int> diag_reconnects{0};

  std::mutex qmi_status_mu;
  QCom::Qmi::NasRadioStatus qmi_status{};
  std::mutex code_mu;
  std::map<QCom::LogCode, uint64_t> code_hist;
  std::map<int, uint64_t> b0c0_pdu_hist;
  std::map<int, uint64_t> b0c0_ver_hist;
  std::mutex fp_mu;
  std::string last_fp;

  std::optional<std::string> saved_lte_bands;
  std::optional<std::string> saved_cnmp;
  std::chrono::steady_clock::time_point last_fplmn_wipe{};

  std::mutex rg_note_mu;
  std::chrono::steady_clock::time_point last_rg_note{};
  std::string last_rg_note_msg;

  [[nodiscard]] bool want_lte_scan() const noexcept {
    return !opt.wcdma_only() && (opt.surveying() || opt.recipe.pin_lte || opt.recipe.ghost);
  }
  [[nodiscard]] QCom::RatType scan_rat() const noexcept {
    return opt.wcdma_only() ? QCom::RatType::WCDMA : QCom::RatType::LTE;
  }

  [[nodiscard]] std::optional<std::string> at_cmd(const char* command, int timeout_ms = 2000,
                                                  AtSession::TickFn tick = {}) {
    if (!at) return std::nullopt;
    return at->cmd(command, timeout_ms, std::move(tick));
  }

  [[nodiscard]] bool cpsi_is_lte_ok() {
    auto raw = at_cmd("AT+CPSI?", 1500);
    return raw && parse_cpsi_lte(*raw).ok;
  }

  [[nodiscard]] bool qmi_lte_rf() {
    std::lock_guard lock(qmi_status_mu);
    if (qmi_status.lte_rsrp_dbm) return true;
    if (qmi_status.radio.find("lte") != std::string::npos) return true;
    return false;
  }

  [[nodiscard]] bool diag_lte_rf() const {
    if (!engine) return false;
    for (const auto& c : engine->tracker().get_snapshot()) {
      if (c.rat != QCom::RatType::LTE) continue;
      if (c.radio.freq() != 0 && c.radio.pci_bsic() != 0 && c.radio.pci_bsic() <= 503) return true;
    }
    return false;
  }

  [[nodiscard]] bool rf_lte_alive() { return cpsi_is_lte_ok() || qmi_lte_rf() || diag_lte_rf(); }

  [[nodiscard]] bool diag_serving_3g() {
    if (rf_lte_alive()) return false;
    if (!engine) return false;
    for (const auto& c : engine->tracker().get_snapshot()) {
      if (!c.is_serving) continue;
      if (c.rat == QCom::RatType::WCDMA || c.rat == QCom::RatType::GSM) return true;
    }
    return false;
  }

  bool wipe_fplmn(const char* why, bool force = false);
  void maybe_wipe_fplmn(const char* why);
  void rat_guard_note(std::string msg, std::chrono::seconds min_gap = std::chrono::seconds(20));
  bool ensure_scan_rat(const char* why);
};

inline bool SurveyCtx::wipe_fplmn(const char* why, bool force) {
  if ((!opt.recipe.wipe_fplmn && !force) || !at) return false;
  auto rd = at_cmd("AT+CRSM=176,28539,0,0,12", 4000);
  if (!rd || (!crsm_sw_ok(*rd) && !crsm_fplmn_empty(*rd))) {
    dash->note(std::string("[fplmn] read failed (") + why + ")");
    return false;
  }
  if (crsm_fplmn_empty(*rd)) {
    dash->note(std::string("[fplmn] already empty (") + why + ")");
    return true;
  }
  dash->note(std::string("[fplmn] clearing EF_FPLMN (") + why + ")…");
  auto wr = at_cmd("AT+CRSM=214,28539,0,0,12,\"FFFFFFFFFFFFFFFFFFFFFFFF\"", 5000);
  if (!wr || !at_reply_ok(*wr)) {
    (void)at_cmd("AT+CFUN=4", 5000);
    wr = at_cmd("AT+CRSM=214,28539,0,0,12,\"FFFFFFFFFFFFFFFFFFFFFFFF\"", 5000);
    (void)at_cmd("AT+CFUN=1", 5000);
    if (!opt.recipe.ghost) (void)at_cmd("AT+COPS=0", 5000);
  }
  auto rd2 = at_cmd("AT+CRSM=176,28539,0,0,12", 4000);
  const bool ok = rd2 && crsm_fplmn_empty(*rd2);
  if (ok) ++fplmn_wipes;
  dash->note(ok ? std::string("[fplmn] cleared (") + why + ", #" +
                     std::to_string(fplmn_wipes.load()) + ")"
               : std::string("[fplmn] clear verify failed (") + why + ")");
  return ok;
}

inline void SurveyCtx::maybe_wipe_fplmn(const char* why) {
  if (!opt.recipe.wipe_fplmn || !at) return;
  const auto now = std::chrono::steady_clock::now();
  const auto min_gap = std::chrono::seconds(std::max(30, opt.recipe.search_period_sec));
  if (last_fplmn_wipe.time_since_epoch().count() != 0 && now - last_fplmn_wipe < min_gap) return;
  last_fplmn_wipe = now;
  (void)wipe_fplmn(why);
}

inline void SurveyCtx::rat_guard_note(std::string msg, std::chrono::seconds min_gap) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(rg_note_mu);
  if (msg == last_rg_note_msg && last_rg_note.time_since_epoch().count() != 0 &&
      now - last_rg_note < min_gap)
    return;
  last_rg_note = now;
  last_rg_note_msg = msg;
  dash->note(std::move(msg));
}

inline bool SurveyCtx::ensure_scan_rat(const char* why) {
  if (!want_lte_scan() || !at) return true;
  if (rat_recover_busy.exchange(true)) return scan_rat_ok.load(std::memory_order_relaxed);
  struct BusyGuard {
    std::atomic<bool>& f;
    ~BusyGuard() { f.store(false, std::memory_order_relaxed); }
  } busy{rat_recover_busy};

  auto raw = at_cmd("AT+CPSI?", 1500);
  ObservedRat obs = raw ? observe_rat_from_cpsi(*raw) : ObservedRat::Unknown;
  observed_rat_code.store(static_cast<int>(obs), std::memory_order_relaxed);

  if (intentional_wcdma.load(std::memory_order_relaxed)) {
    if (obs == ObservedRat::Lte) {
      (void)at_cmd("AT+CNMP=14", 5000);
      if (qmi) (void)qmi->control().set_mode_preference({QCom::Qmi::Rat::Wcdma});
      rat_guard_note(std::string("[rat-guard] wcdma-walk hold — re-pin CNMP=14 (") + why + ")",
                     std::chrono::seconds(10));
    } else {
      rat_guard_note(std::string("[rat-guard] wcdma-walk hold (") + why + ", " + to_string(obs) + ")",
                     std::chrono::seconds(30));
    }
    scan_rat_ok.store(true, std::memory_order_relaxed);
    return true;
  }

  const auto want = scan_rat();
  if (observed_matches_scan(obs, want) && cpsi_is_lte_ok()) {
    scan_rat_ok.store(true, std::memory_order_relaxed);
    return true;
  }

  const bool sticky_3g =
      (obs == ObservedRat::Wcdma || obs == ObservedRat::Gsm) || diag_serving_3g();
  bool ghosting = intentional_search.load(std::memory_order_relaxed);

  if (ghosting && sticky_3g) {
    intentional_search.store(false, std::memory_order_relaxed);
    ghosting = false;
    rat_guard_note(std::string("[rat-guard] 3G during ghost — yank (") + why +
                       ", DIAG/CPSI=" + to_string(obs) + ")",
                   std::chrono::seconds(5));
  }

  if (ghosting && !sticky_3g) {
    scan_rat_ok.store(true, std::memory_order_relaxed);
    return true;
  }

  if (!sticky_3g) {
    scan_rat_ok.store(true, std::memory_order_relaxed);
    (void)at_cmd("AT+CNMP=38", 5000);
    rat_guard_note(std::string("[rat-guard] ") + to_string(obs) + " — hop/ghost own recover (" + why +
                   ")");
    return false;
  }

  ++rat_guard_trips;
  std::string snip = raw ? *raw : "(no CPSI)";
  for (char& c : snip) {
    if (c == '\r' || c == '\n') c = ' ';
  }
  if (snip.size() > 100) snip.resize(100);
  dash->note(std::string("[rat-guard] MISMATCH want=") + to_string(want) + " have=" + to_string(obs) +
             " (" + why + ", #" + std::to_string(rat_guard_trips.load()) + ") CPSI=" + snip);

  (void)at_cmd("AT+CCELLCFG=0", 2000);
  (void)at_cmd("AT+CLECELL", 2000);
  (void)at_cmd("AT+CLEARFCN", 2000);
  (void)at_cmd("AT+CLUCELL", 2000);
  (void)at_cmd("AT+CLUARFCN", 2000);
  last_fplmn_wipe = {};
  {
    const std::string wipe_why = std::string("rat-guard:") + why;
    (void)wipe_fplmn(wipe_why.c_str(), /*force=*/true);
  }
  if (qmi) (void)qmi->control().set_mode_preference({QCom::Qmi::Rat::Lte});
  (void)at_cmd("AT+CNMP=38", 10000);
  if (!ghosting) (void)at_cmd("AT+COPS=0", 10000);
  if (qmi) (void)qmi->control().force_network_search();

  if (opt.surveying()) {
    scan_rat_ok.store(true, std::memory_order_relaxed);
    rat_guard_note(std::string("[rat-guard] hop owns recover — CNMP pin only (") + why + ")");
    return cpsi_is_lte_ok();
  }

  for (int i = 0; i < 40 && !rat_guard_stop.load(std::memory_order_relaxed); ++i) {
    if (cpsi_is_lte_ok()) {
      scan_rat_ok.store(true, std::memory_order_relaxed);
      observed_rat_code.store(static_cast<int>(ObservedRat::Lte), std::memory_order_relaxed);
      dash->note(std::string("[rat-guard] back on LTE (") + why + ")");
      return true;
    }
    if ((i % 20) == 19) {
      (void)at_cmd("AT+CNMP=38", 5000);
      if (!ghosting) (void)at_cmd("AT+COPS=0", 5000);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  const bool do_cfun =
      (obs == ObservedRat::Wcdma || obs == ObservedRat::Gsm) || (opt.recipe.cfun_recover && sticky_3g);
  if (do_cfun) {
    dash->note(std::string("[rat-guard] airplane CFUN (") + why + ", wrong-RAT)");
    (void)at_cmd("AT+CFUN=4", 10000);
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    (void)at_cmd("AT+CNMP=38", 10000);
    last_fplmn_wipe = {};
    (void)wipe_fplmn("rat-guard-airplane", /*force=*/true);
    (void)at_cmd("AT+CFUN=1", 10000);
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));
    if (at) at->reconnect();
    (void)at_cmd("AT+CNMP=38", 10000);
    if (!ghosting) (void)at_cmd("AT+COPS=0", 10000);
    if (qmi) {
      (void)qmi->control().set_mode_preference({QCom::Qmi::Rat::Lte});
      (void)qmi->control().force_network_search();
    }
    for (int i = 0; i < 80 && !rat_guard_stop.load(std::memory_order_relaxed); ++i) {
      if (cpsi_is_lte_ok()) {
        scan_rat_ok.store(true, std::memory_order_relaxed);
        observed_rat_code.store(static_cast<int>(ObservedRat::Lte), std::memory_order_relaxed);
        dash->note(std::string("[rat-guard] LTE after CFUN (") + why + ")");
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
  }

  auto raw2 = at_cmd("AT+CPSI?", 1500);
  if (raw2)
    observed_rat_code.store(static_cast<int>(observe_rat_from_cpsi(*raw2)),
                            std::memory_order_relaxed);
  const ObservedRat obs2 = static_cast<ObservedRat>(observed_rat_code.load());
  const bool freeze = (obs2 == ObservedRat::Wcdma || obs2 == ObservedRat::Gsm);
  scan_rat_ok.store(!freeze, std::memory_order_relaxed);
  dash->note(std::string("[rat-guard] STILL OFF-RAT (") + why + ") have=" + to_string(obs2) +
             (freeze ? " — hop paused" : " — hop may continue (ML1 listen)"));
  return cpsi_is_lte_ok();
}

}  // namespace Observer
