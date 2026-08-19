#include "observer/SurveyProc.h"

#include "observer/AtParse.h"
#include "observer/ModemSelect.h"
#include "observer/ServingInject.h"
#include "observer/Signals.h"

#include <observer/model/Events.h>
#include <observer/model/Utils.h>
#include <qcom/linux/LinuxSource.h>
#include <qcom/linux/SourceFactory.h>
#include <qcom/lte/LteRrcOta.h>
#include <qcom/parser/QualcomParser.h>
#include <qcom/protocol/DiagSourceConfig.h>

#include <algorithm>
#include <csignal>
#include <iostream>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace Observer {

namespace {

QCom::DiagMaskProfile mask_profile(const Options& opt) {
  if (opt.wcdma_only()) return QCom::DiagMaskProfile::WcdmaOnly;
  if (opt.rats == Rats::Lte) return QCom::DiagMaskProfile::LteOnly;
  return QCom::DiagMaskProfile::AllRats;
}

}  // namespace

SurveyProc::SurveyProc(Options opt) : dash_(/*enable_inplace=*/true) {
  ctx_.opt = std::move(opt);
  ctx_.dash = &dash_;
}

SurveyProc::~SurveyProc() { stop_feeds(); }

int SurveyProc::run() {
  if (const int st = boot(); st != 0) return st;
  pin_radio();
  start_feeds();
  hop_.emplace(ctx_);
  hop_->start();
  set_banner();
  pump();
  stop_feeds();
  restore_radio();
  report();
  return 0;
}

int SurveyProc::boot() {
  auto resolved = resolve_modem(ctx_.opt.io.device, ctx_.opt.io.qmi_path);
  if (!resolved) return 1;
  ctx_.modem = *resolved;

  const auto& rec = ctx_.opt.recipe;
  const auto& io = ctx_.opt.io;
  QCom::DiagSourceConfig cfg{
      .device_path = ctx_.modem.diag,
      .baud_rate = io.baud,
      .init_masks = rec.init_masks,
      .mask_profile = mask_profile(ctx_.opt),
  };
  engine_ = std::make_unique<QCom::RadioScannerEngine>(QCom::make_diag_source(std::move(cfg)));
  ctx_.engine = engine_.get();

  if (io.scanner_log_path) {
    std::ostringstream hdr;
    hdr << ctx_.opt.cmdline;
    if (!dash_.open_scanner_log(*io.scanner_log_path, hdr.str())) {
      std::cerr << "scanner-log open failed: " << *io.scanner_log_path << "\n";
    } else {
      std::cout << "Scanner log → " << dash_.log_path() << "\n"
                << "             → " << dash_.log_session_path() << " (session)\n";
    }
  }

  live_ = std::make_unique<LiveJson>(ctx_);
  if (live_->acquire_lock() != 0) return 1;
  engine_->set_callback([this](const std::vector<QCom::CellIdentity>& cells) { live_->on_cells(cells); });
  engine_->set_packet_observer([this](QCom::QualcommPacketView pkt) { live_->on_packet(pkt); });
  engine_->set_disconnect_callback([this] {
    ctx_.diag_alive.store(false, std::memory_order_relaxed);
    ctx_.diag_needs_reconnect.store(true, std::memory_order_relaxed);
    dash_.note("[diag] link dead after revive attempts — full reconnect…");
  });

  std::signal(SIGINT, live_scanner_on_signal);
  std::signal(SIGTERM, live_scanner_on_signal);

  std::cout << "Starting " << (engine_->source() ? engine_->source()->name() : "?") << " on "
            << ctx_.modem.diag << " …\n";
  if (!engine_->start()) {
    std::cerr << "Failed to start DIAG source on " << ctx_.modem.diag << "\n";
    if (auto* linux = dynamic_cast<QCom::LinuxSource*>(engine_->source())) {
      if (!linux->last_error().empty()) std::cerr << "  reason: " << linux->last_error() << "\n";
    }
    std::cerr << "  tips: ls -l " << ctx_.modem.diag << " ; groups | grep dialout ; fuser "
              << ctx_.modem.diag << "\n  after USB re-plug wait 1–2s, or: sudo chmod a+rw "
              << ctx_.modem.diag << "\n";
    if (ctx_.modem.qmi) {
      std::cerr << "  QMI perms (often root:root after re-enum): sudo chmod a+rw " << *ctx_.modem.qmi
                << "\n";
    }
    return 1;
  }
  if (auto* linux = dynamic_cast<QCom::LinuxSource*>(engine_->source())) {
    if (!linux->init_ok()) {
      std::cerr << "Warning: mask init failed (" << linux->last_error() << ") — listening anyway\n";
    } else if (rec.init_masks) {
      const char* mp = ctx_.opt.wcdma_only()     ? "WCDMA-only (LTE muted)"
                       : (ctx_.opt.rats == Rats::Lte) ? "LTE-only"
                                                      : "all-RAT";
      std::cout << "Diag masks init OK (" << mp << ")\n";
    }
  }

  if (ctx_.modem.at) {
    at_ = std::make_unique<AtBus>(*ctx_.modem.at);
    ctx_.at = at_.get();
  } else if (ctx_.opt.surveying()) {
    std::cerr << "survey walk needs AT path (catalog has DIAG but no AT tty)\n";
    return 1;
  }
  simcom_.emplace([&](std::string_view cmd, int timeout_ms) {
    return ctx_.at_cmd(std::string(cmd).c_str(), timeout_ms);
  });
  ctx_.simcom = &*simcom_;

  g_qmi_stop_ptr = &ctx_.qmi_stop;
  g_rat_guard_stop_ptr = &ctx_.rat_guard_stop;
  g_cereg_stop_ptr = &ctx_.cereg_stop;
  g_at_only_stop_ptr = &ctx_.at_only_stop;

  if (rec.use_qmi && ctx_.modem.qmi) {
    std::string qmi_open_err;
    auto try_open_qmi = [&](bool proxy) -> bool {
      QCom::Qmi::Settings qs;
      qs.device_path = *ctx_.modem.qmi;
      qs.use_proxy = proxy;
      qs.allow_dms_offline = false;
      qs.request_timeout = std::chrono::seconds(8);
      qmi_ = std::make_unique<QCom::Qmi::Session>(std::move(qs));
      if (auto opened = qmi_->open(); !opened) {
        qmi_open_err = opened.error().message;
        qmi_.reset();
        return false;
      }
      std::cout << "QMI " << (proxy ? "qmi-proxy" : "direct") << " on " << *ctx_.modem.qmi << "\n";
      return true;
    };
    // cdc-wdm is often root:root 0600. qmi-proxy under the same uid also gets EACCES.
    // chmod (root) or one polkit `chmod a+rw`, then exclusive open.
    (void)try_make_qmi_rw(*ctx_.modem.qmi);
    const bool can_rw = device_user_rw(*ctx_.modem.qmi);
    bool qmi_ok = try_open_qmi(/*proxy=*/!can_rw);
    if (!qmi_ok) qmi_ok = try_open_qmi(/*proxy=*/can_rw);
    if (!qmi_ok) {
      std::cerr << "QMI open failed: " << qmi_open_err << " — continuing DIAG+AT only\n";
      std::cerr << "  Neighbours still come from DIAG SIB/ML1; QMI adds NAS GCI when the node is rw.\n";
      std::cerr << "  This session: sudo chmod a+rw " << *ctx_.modem.qmi << "\n";
      std::cerr << "  Persist: sudo cp tools/udev/99-observer-cdc-wdm.rules /etc/udev/rules.d/ "
                   "&& sudo udevadm control --reload-rules && sudo udevadm trigger -s usbmisc\n";
      ctx_.qmi = nullptr;
    } else {
      ctx_.qmi = qmi_.get();
      const auto& r = ctx_.opt.recipe;
      if (r.pin_lte || ctx_.opt.hop_lock() || r.ghost) {
        auto set = qmi_->control().set_mode_preference({QCom::Qmi::Rat::Lte});
        if (!set) {
          std::cerr << "QMI mode preference failed: " << set.error().message << "\n";
        } else {
          std::cout << "QMI mode preference set to LTE-only"
                    << (ctx_.opt.hop_lock() ? " (hop)" : r.ghost ? " (ghost)" : " (pin-lte)")
                    << "\n";
        }
      }
      if (r.qmi_plmn_search) {
        std::cout << "PLMN search every " << r.search_period_sec
                  << "s (QMI force-search → B0C2/SIB1 during sweep)\n";
      } else {
        std::cout << "QMI PLMN search OFF (default)\n";
      }
      if (r.periodic_cops && ctx_.modem.at) {
        std::cout << "AT+COPS=? + QMI force-search every " << r.search_period_sec << "s on "
                  << *ctx_.modem.at << " (first kick ~5s)\n";
      } else if (r.periodic_cops) {
        std::cout << "AT+COPS=? requested but no AT path in catalog\n";
      }
      if (r.ghost && !ctx_.opt.surveying() && ctx_.modem.at) {
        std::cout << "ghost-plmn periodic every " << r.search_period_sec << "s on " << *ctx_.modem.at
                  << " (dwell " << r.ghost_dwell_sec << "s)\n";
      }
      std::cout << "QMI NAS poll every " << io.qmi_period_ms << "ms on " << *ctx_.modem.qmi << "\n";
    }
  } else if (rec.use_qmi) {
    std::cerr << "No QMI path — DIAG only (--qmi /dev/cdc-wdm0 to force)\n";
  }
  return 0;
}

void SurveyProc::pin_radio() {
  const auto& rec = ctx_.opt.recipe;
  auto at_do = [&](const char* cmd, int timeout_ms = 2000) { return ctx_.at_cmd(cmd, timeout_ms); };
  const char* at_path = ctx_.at ? ctx_.at->path().c_str() : "?";

  if (ctx_.at && ctx_.opt.wcdma_only()) {
    ctx_.intentional_wcdma.store(true, std::memory_order_relaxed);
    if (auto rsp = at_do("AT+CNMP=14", 10000); rsp && at_reply_ok(*rsp)) {
      std::cout << "AT+CNMP=14 (WCDMA-only) on " << at_path << "\n";
    } else {
      std::cerr << "AT+CNMP=14 failed on " << at_path << "\n";
    }
    if (qmi_) (void)qmi_->control().set_mode_preference({QCom::Qmi::Rat::Wcdma});
  } else if (ctx_.at && (rec.pin_lte || ctx_.opt.hop_lock() || rec.ghost)) {
    if (auto rsp = at_do("AT+CNMP=38", 10000); rsp && at_reply_ok(*rsp)) {
      std::cout << "AT+CNMP=38 (LTE-only) on " << at_path << "\n";
    } else {
      std::cerr << "AT+CNMP=38 failed on " << at_path << "\n";
    }
  }
  if (ctx_.at && !rec.ghost && (rec.pin_lte || ctx_.opt.hop_lock() || qmi_)) {
    if (auto rsp = at_do("AT+COPS=0", 4000); rsp && at_reply_ok(*rsp)) {
      std::cout << "AT+COPS=0 (auto select) on " << at_path << "\n";
    } else {
      std::cerr << "AT+COPS=0 failed on " << at_path << "\n";
    }
  } else if (ctx_.at && rec.ghost) {
    std::cout << "skip AT+COPS=0 at start (ghost will select "
              << format_plmn_numeric(rec.ghost_plmn.first, rec.ghost_plmn.second) << ")\n";
  }
  if (rec.wipe_fplmn && ctx_.at && !ctx_.opt.surveying()) {
    (void)ctx_.wipe_fplmn("startup");
    ctx_.last_fplmn_wipe = std::chrono::steady_clock::now();
  }
}

void SurveyProc::start_feeds() {
  const auto& rec = ctx_.opt.recipe;
  if (ctx_.want_lte_scan() && ctx_.at) {
    std::cout << "rat-guard ON: enforce " << to_string(ctx_.scan_rat())
              << " (poll 2s; mismatch → FPLMN wipe + CNMP=38 + auto-CFUN on 3G)\n";
    rat_guard_th_ = std::thread([this] { rat_guard_loop(); });
  }
  if (qmi_) qmi_th_ = std::thread([this] { qmi_loop(); });

  const bool want_search =
      (rec.periodic_cops || (rec.ghost && !ctx_.opt.surveying())) && ctx_.at && !qmi_;
  if (want_search) {
    if (rec.deep_search && !ctx_.deep_dereg_done.exchange(true)) {
      if (auto rsp = ctx_.at_cmd("AT+COPS=2", 8000); rsp && at_reply_ok(*rsp)) {
        dash_.note("[deep-search] AT+COPS=2 once (deregister, no QMI)");
      } else {
        dash_.note("[deep-search] AT+COPS=2 failed");
      }
      std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    search_th_ = std::thread([this] { search_loop(); });
  }

  if (rec.enrich_serving && ctx_.at) {
    (void)ctx_.at_cmd("AT+CEREG=2", 2000);
    (void)ctx_.at_cmd("AT+CREG=2", 2000);
    (void)ctx_.at_cmd("AT+COPS=3,2", 2000);
    std::cout << "AT+CPSI? serving FULL poll on " << ctx_.at->path()
              << " (CEREG CID stamp disabled — needs EARFCN|PCI)\n";
    serving_th_ = std::thread([this] { serving_loop(); });
  }
}

void SurveyProc::rat_guard_loop() {
  std::this_thread::sleep_for(std::chrono::seconds(3));
  while (!ctx_.rat_guard_stop.load(std::memory_order_relaxed)) {
    (void)ctx_.ensure_scan_rat("watchdog");
    for (int i = 0; i < 20 && !ctx_.rat_guard_stop.load(std::memory_order_relaxed); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void SurveyProc::qmi_loop() {
  const auto& rec = ctx_.opt.recipe;
  auto last_search = std::chrono::steady_clock::now();
  const auto started = std::chrono::steady_clock::now();
  bool first_cops_gate = false;
  while (!ctx_.qmi_stop.load(std::memory_order_relaxed)) {
    ++ctx_.qmi_polls;
    auto snap = qmi_->nas().snapshot_cells();
    if (snap) {
      ++ctx_.qmi_ok;
      auto envs = QCom::Qmi::to_rrc_envelopes(snap.value());
      if (ctx_.opt.wcdma_only()) {
        envs.erase(std::remove_if(envs.begin(), envs.end(),
                                  [](const QCom::Events::RrcEventEnvelope& e) {
                                    return e.rat == QCom::RatType::LTE || e.rat == QCom::RatType::NR;
                                  }),
                   envs.end());
      }
      if (!envs.empty()) engine_->inject_envelopes(std::move(envs));
    }
    if (auto st = qmi_->nas().snapshot_status(); st) {
      std::lock_guard qlock(ctx_.qmi_status_mu);
      ctx_.qmi_status = std::move(st.value());
    }

    const auto now = std::chrono::steady_clock::now();
    const bool want_force =
        rec.qmi_plmn_search || rec.periodic_cops || (rec.ghost && !ctx_.opt.surveying());
    const bool warmed = now - started >= std::chrono::seconds(5);
    const bool period_ok =
        warmed && now - last_search >= std::chrono::seconds(std::max(5, rec.search_period_sec));
    const bool first_ok = warmed && !first_cops_gate;
    if (want_force && (period_ok || first_ok)) {
      last_search = now;
      first_cops_gate = true;

      if (auto fs = qmi_->control().force_network_search(); fs) {
        ++ctx_.search_kicks;
        dash_.note("[ota-kick] QMI force-search ok (#" + std::to_string(ctx_.search_kicks.load()) +
                   ")");
      } else {
        dash_.note("[ota-kick] QMI force-search failed: " + fs.error().message);
      }

      if (rec.periodic_cops && ctx_.modem.at) {
        if (rec.deep_search && !ctx_.deep_dereg_done.exchange(true)) {
          if (auto rsp = ctx_.at_cmd("AT+COPS=2", 8000); rsp && at_reply_ok(*rsp)) {
            dash_.note("[deep-search] AT+COPS=2 once (deregister)");
          } else {
            dash_.note("[deep-search] AT+COPS=2 failed");
          }
          std::this_thread::sleep_for(std::chrono::seconds(3));
        }
        if (ctx_.at_cmd("AT+COPS=?", 180000)) {
          dash_.note(std::string("[at-cops] AT+COPS=? done on ") +
                     (ctx_.at ? ctx_.at->path() : "?"));
        } else {
          dash_.note("[at-cops] AT+COPS=? timeout/fail");
        }
      }

      if (rec.ghost && !ctx_.opt.surveying() && ctx_.modem.at) {
        ctx_.maybe_wipe_fplmn("pre-ghost");
        FlagGuard ghost_flag{ctx_.intentional_search};
        const std::string plmn = format_plmn_numeric(rec.ghost_plmn.first, rec.ghost_plmn.second);
        const std::string cmd = format_cops_manual_select(plmn, rec.cops_act);
        (void)ctx_.at_cmd("AT+COPS=3,2", 2000);
        ++ctx_.ghost_kicks;
        dash_.note("[ghost] " + cmd + " (#" + std::to_string(ctx_.ghost_kicks.load()) + ")…");
        auto gr = ctx_.at_cmd(cmd.c_str(), 60000);
        const bool ok = gr && at_reply_ok(*gr);
        const int linger = ghost_linger_sec(ok, rec.ghost_dwell_sec);
        if (ok)
          dash_.note("[ghost] select accepted — lingering " + std::to_string(linger) + "s");
        else
          dash_.note("[ghost] select ERROR/timeout (expected) — linger " + std::to_string(linger) +
                     "s (not full dwell)");
        for (int g = 0; g < linger * 10 && !ctx_.qmi_stop.load(std::memory_order_relaxed);
             ++g)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        (void)ctx_.at_cmd("AT+COPS=0", 10000);
        dash_.note("[ghost] AT+COPS=0 restore");
      }
    }

    for (int i = 0; i < ctx_.opt.io.qmi_period_ms / 50 && !ctx_.qmi_stop.load(std::memory_order_relaxed);
         ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}

void SurveyProc::search_loop() {
  const auto& rec = ctx_.opt.recipe;
  const auto started = std::chrono::steady_clock::now();
  auto last = started;
  bool first = false;
  while (!ctx_.at_only_stop.load(std::memory_order_relaxed)) {
    const auto now = std::chrono::steady_clock::now();
    const bool warmed = now - started >= std::chrono::seconds(5);
    const bool due =
        warmed && now - last >= std::chrono::seconds(std::max(5, rec.search_period_sec));
    const bool gate = warmed && !first;
    if (due || gate) {
      last = now;
      first = true;
      if (rec.periodic_cops) {
        if (ctx_.at_cmd("AT+COPS=?", 180000)) {
          ++ctx_.search_kicks;
          dash_.note("[at-cops] AT+COPS=? done (no QMI, #" +
                     std::to_string(ctx_.search_kicks.load()) + ")");
        } else {
          dash_.note("[at-cops] AT+COPS=? timeout/fail");
        }
      }
      if (rec.ghost && !ctx_.opt.surveying()) {
        ctx_.maybe_wipe_fplmn("pre-ghost-noqmi");
        FlagGuard ghost_flag{ctx_.intentional_search};
        const std::string plmn = format_plmn_numeric(rec.ghost_plmn.first, rec.ghost_plmn.second);
        const std::string cmd = format_cops_manual_select(plmn, rec.cops_act);
        (void)ctx_.at_cmd("AT+COPS=3,2", 2000);
        ++ctx_.ghost_kicks;
        dash_.note("[ghost] " + cmd + " (no QMI, #" + std::to_string(ctx_.ghost_kicks.load()) + ")…");
        auto gr = ctx_.at_cmd(cmd.c_str(), 60000);
        const bool ok = gr && at_reply_ok(*gr);
        const int linger = ghost_linger_sec(ok, rec.ghost_dwell_sec);
        if (ok)
          dash_.note("[ghost] select accepted — lingering " + std::to_string(linger) + "s");
        else
          dash_.note("[ghost] select ERROR/timeout (expected) — linger " + std::to_string(linger) +
                     "s (not full dwell)");
        for (int g = 0; g < linger * 10 && !ctx_.at_only_stop.load(std::memory_order_relaxed);
             ++g)
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        (void)ctx_.at_cmd("AT+COPS=0", 10000);
        dash_.note("[ghost] AT+COPS=0 restore (no QMI)");
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

void SurveyProc::serving_loop() {
  while (!ctx_.cereg_stop.load(std::memory_order_relaxed)) {
    (void)inject_serving_identity(ctx_);
    for (int i = 0; i < 20 && !ctx_.cereg_stop.load(std::memory_order_relaxed); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void SurveyProc::set_banner() {
  const auto& rec = ctx_.opt.recipe;
  const auto& m = ctx_.modem;
  std::ostringstream ban;
  ban << "live_scanner  diag=" << m.diag;
  if (m.at) ban << "  at=" << *m.at;
  if (m.qmi) ban << "  qmi=" << *m.qmi;
  ban << "\njob=" << to_string(ctx_.opt.job) << "  rats=" << to_string(ctx_.opt.rats)
      << "\nTree: eNB site → FULL cells → n=neighbors  |  *=serving  |  Fill=FULL/PLMN/RADIO\n";
  if (ctx_.opt.surveying())
    ban << "survey walk (" << (ctx_.opt.hop_lock() ? "sticky CCELLCFG grind" : "CFUN bounce")
        << (rec.band_clip ? ", band-clip" : "")
        << (rec.scan_between_hops ? ", between-hop COPS=?" : "")
        << (rec.ghost ? (", ghost-plmn " + format_plmn_numeric(rec.ghost_plmn.first, rec.ghost_plmn.second))
                      : "")
        << (rec.foreign_plmn ? ", full-walk" : "") << (rec.irat_wcdma ? ", wcdma-walk" : "")
        << (rec.wipe_fplmn ? ", clear-fplmn" : "") << ")\n";
  if (rec.enrich_serving) ban << "AT+CPSI?/CNWINFO serving FULL poll ON (no CEREG CID stamp)\n";
  ban << "(in-place refresh; Ctrl+C / duration end → summary)\n\n";
  dash_.set_banner(ban.str());
}

bool SurveyProc::reconnect_diag() {
  auto* linux = dynamic_cast<QCom::LinuxSource*>(engine_->source());
  if (!linux) return false;

  const auto now = std::chrono::steady_clock::now();
  if (last_reconnect_.time_since_epoch().count() != 0 &&
      now - last_reconnect_ < std::chrono::seconds(8)) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    return engine_->source() && engine_->source()->is_running();
  }
  last_reconnect_ = now;

  dash_.note("[diag] USB/DIAG down — waiting for device…");
  std::string diag_path = ctx_.modem.diag;
  for (int i = 0; i < 90 && !g_user_stop.load(std::memory_order_relaxed); ++i) {
    if (i > 0 && (i % 5) == 0) {
      if (auto again = resolve_modem(ctx_.opt.io.device, ctx_.opt.io.qmi_path); again) {
        ctx_.modem = *again;
        diag_path = ctx_.modem.diag;
        if (ctx_.modem.at) {
          if (ctx_.at && ctx_.at->path() != *ctx_.modem.at)
            ctx_.at->reopen(*ctx_.modem.at);
          else if (!ctx_.at) {
            at_ = std::make_unique<AtBus>(*ctx_.modem.at);
            ctx_.at = at_.get();
          }
        }
      }
      dash_.note("[diag] still waiting… (" + std::to_string(i) + "s) path=" + diag_path);
    }
    if (::access(diag_path.c_str(), R_OK | W_OK) == 0) break;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (g_user_stop.load(std::memory_order_relaxed)) return false;
  if (::access(diag_path.c_str(), R_OK | W_OK) != 0) {
    dash_.note("[diag] device still missing: " + diag_path);
    return false;
  }

  linux->set_device_path(diag_path);
  if (!linux->reconnect()) {
    dash_.note(std::string("[diag] reconnect failed: ") + std::string(linux->last_error()));
    return false;
  }
  if (ctx_.at) {
    if (ctx_.modem.at && ctx_.at->path() != *ctx_.modem.at)
      ctx_.at->reopen(*ctx_.modem.at);
    else
      ctx_.at->reconnect();
  } else if (ctx_.modem.at) {
    at_ = std::make_unique<AtBus>(*ctx_.modem.at);
    ctx_.at = at_.get();
  }
  ++ctx_.diag_reconnects;
  ctx_.diag_alive.store(true, std::memory_order_relaxed);
  ctx_.diag_needs_reconnect.store(false, std::memory_order_relaxed);
  dash_.note("[diag] reconnected OK on " + diag_path + " (#" +
             std::to_string(ctx_.diag_reconnects.load()) + ")");
  return true;
}

void SurveyProc::pump() {
  const int duration_sec = ctx_.opt.io.duration_sec;
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

    const bool down = ctx_.diag_needs_reconnect.load(std::memory_order_relaxed) ||
                      !engine_->source() || !engine_->source()->is_running();
    if (down) {
      ctx_.diag_alive.store(false, std::memory_order_relaxed);
      if (!reconnect_diag()) {
        for (int i = 0; i < 3 && !g_user_stop.load(std::memory_order_relaxed); ++i)
          std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
    }
    live_->write(true);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  live_->write(true);
}

void SurveyProc::stop_feeds() {
  if (stopped_) return;
  stopped_ = true;
  ctx_.qmi_stop = true;
  ctx_.at_only_stop = true;
  ctx_.cereg_stop = true;
  ctx_.hop_stop = true;
  ctx_.rat_guard_stop.store(true, std::memory_order_relaxed);
  g_user_stop = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  if (qmi_th_.joinable()) qmi_th_.join();
  if (search_th_.joinable()) search_th_.join();
  if (rat_guard_th_.joinable()) rat_guard_th_.join();
  if (serving_th_.joinable()) serving_th_.join();
  if (hop_) hop_->join();
  if (qmi_) {
    qmi_->close();
    ctx_.qmi = nullptr;
  }
  if (engine_) engine_->stop();
  dash_.leave_inplace();
}

void SurveyProc::restore_radio() {
  const auto& rec = ctx_.opt.recipe;
  if (!ctx_.at) return;
  if (!(rec.periodic_cops || ctx_.opt.surveying() || rec.enrich_serving || rec.scan_between_hops))
    return;
  if (ctx_.opt.surveying()) {
    (void)ctx_.at_cmd("AT+CCELLCFG=0", 1500);
    (void)ctx_.at_cmd("AT+CLECELL", 1500);
    (void)ctx_.at_cmd("AT+CLEARFCN", 1500);
    (void)ctx_.at_cmd("AT+CLUCELL", 1500);
    (void)ctx_.at_cmd("AT+CLUARFCN", 1500);
    if (ctx_.saved_lte_bands && !ctx_.saved_lte_bands->empty()) {
      const std::string restore = "AT+CSYSSEL=\"lte_band\"," + *ctx_.saved_lte_bands;
      (void)ctx_.at_cmd(restore.c_str(), 1500);
    }
    if (ctx_.saved_cnmp && !ctx_.saved_cnmp->empty()) {
      const std::string restore = "AT+CNMP=" + *ctx_.saved_cnmp;
      (void)ctx_.at_cmd(restore.c_str(), 3000);
    } else if (rec.pin_lte) {
      (void)ctx_.at_cmd("AT+CNMP=54", 3000);
    }
  }
  (void)ctx_.at_cmd("AT+CFUN=1", 2000);
  (void)ctx_.at_cmd("AT+COPS=0", 3000);
}

void SurveyProc::report() {
  const auto& rec = ctx_.opt.recipe;
  std::cout << "\nDone. updates=" << ctx_.updates.load()
            << " cells=" << engine_->tracker().cell_count();
  if (auto* linux = dynamic_cast<QCom::LinuxSource*>(engine_->source())) {
    std::cout << " raw_bytes=" << linux->bytes_raw() << " msgs=" << linux->frames_ok()
              << " logs=" << linux->logs_delivered() << " hdlc_bad_crc=" << linux->frames_bad_crc()
              << " diag_revives=" << linux->silent_revives();
    if (linux->bytes_raw() == 0) {
      std::cerr << "\nNo bytes from DIAG. Modem silent or wrong port — re-plug USB / "
                   "qmi_recover / check masks were not left disabled.\n";
    }
  }
  if (rec.use_qmi || rec.periodic_cops || rec.enrich_serving || ctx_.opt.surveying() ||
      rec.scan_between_hops) {
    std::cout << " qmi_polls=" << ctx_.qmi_polls.load() << " qmi_ok=" << ctx_.qmi_ok.load()
              << " ota_kicks=" << ctx_.search_kicks.load() << " cereg_ok=" << ctx_.cereg_ok.load()
              << " cpsi_ok=" << ctx_.cpsi_ok.load() << " cnw_ok=" << ctx_.cnw_ok.load()
              << " hop_kicks=" << ctx_.hop_kicks.load() << " hop_locks=" << ctx_.hop_locks.load()
              << " hop_fulls=" << ctx_.hop_fulls.load()
              << " hop_cops=" << ctx_.cops_between_kicks.load()
              << " qmi_hop_snaps=" << ctx_.qmi_hop_snaps.load()
              << " diag_reconnects=" << ctx_.diag_reconnects.load();
  }
  std::cout << "\n";

  {
    std::lock_guard lock(ctx_.code_mu);
    print_log_code_table(ctx_.code_hist, engine_->parser());
    if (!ctx_.b0c0_pdu_hist.empty()) {
      std::cout << "B0C0 OTA wrapper:\n";
      for (const auto& [ver, n] : ctx_.b0c0_ver_hist)
        std::cout << "  ver=0x" << std::hex << ver << std::dec << "  x" << n << "\n";
      std::cout << "B0C0 pdu_num (SIB1≈2/3/9 by version; MIB≈1/8):\n";
      for (const auto& [pdu, n] : ctx_.b0c0_pdu_hist) {
        if (pdu < 0)
          std::cout << "  (undecoded wrapper) x" << n << "\n";
        else
          std::cout << "  pdu=" << pdu << "  x" << n << "\n";
      }
      const auto asn1_empty = QCom::Lte::lte_rrc_ota_asn1_empty_count();
      if (asn1_empty) std::cout << "  ASN.1 empty/fail (BCCH-DL-SCH only) x" << asn1_empty << "\n";
    }
  }

  const auto snap = engine_->tracker().get_snapshot();
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
    const bool full = c.rat == QCom::RatType::LTE && freq != 0 && pci != 0 && c.passport.mcc != 0 &&
                      QCom::Utils::valid_lte_tac(c.passport.tac) &&
                      QCom::Utils::valid_lte_eci(c.passport.cell_id);
    if (full)
      ++complete;
    else if (c.rat == QCom::RatType::LTE && freq != 0 && pci != 0 && c.passport.mcc == 0)
      ++radio_only;
    else if (c.rat == QCom::RatType::LTE && freq != 0 && pci == 0 && c.passport.mcc != 0)
      ++plmn_only_weak;
  }
  std::cout << "Identity coverage: " << with_id << "/" << snap.size()
            << " cells have passport (CID); " << with_plmn << "/" << snap.size() << " have PLMN\n";
  std::cout << "LTE towers: complete=" << complete << " radio_only=" << radio_only
            << " plmn_weak(EARFCN|0)=" << plmn_only_weak << "\n";
  print_cells_table(snap, "Final snapshot");
}

}  // namespace Observer
