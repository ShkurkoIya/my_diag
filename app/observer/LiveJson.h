#pragma once

#include "observer/Dashboard.h"
#include "observer/Fmt.h"
#include "observer/SurveyCtx.h"
#include "observer/WcdmaWalk.h"

#include "TowerExport.h"

#include <observer/engine/SurveyDomain.h>
#include <observer/engine/SurveyProjection.h>
#include <observer/model/Utils.h>
#include <qcom/lte/LteRrcOta.h>

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#include <vector>

namespace Observer {

class LiveJson {
public:
  explicit LiveJson(SurveyCtx& ctx) : ctx_(ctx) {}
  LiveJson(const LiveJson&) = delete;
  LiveJson& operator=(const LiveJson&) = delete;
  ~LiveJson() { close(); }

  /// 0 = ok (or disabled), 1 = lock/open failed.
  [[nodiscard]] int acquire_lock() {
    const auto& path = ctx_.opt.io.live_json_path;
    if (!path) return 0;
    const std::string lock_path = *path + ".lock";
    lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
    if (lock_fd_ < 0) {
      std::cerr << "live-json lock open failed: " << lock_path << "\n";
      return 1;
    }
    if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
      std::cerr << "Another live_scanner already owns " << *path
                << "\n  stop it first: sudo pkill -f live_scanner\n";
      ::close(lock_fd_);
      lock_fd_ = -1;
      return 1;
    }
    const std::string pid_s = std::to_string(::getpid()) + "\n";
    (void)::ftruncate(lock_fd_, 0);
    (void)::lseek(lock_fd_, 0, SEEK_SET);
    (void)::write(lock_fd_, pid_s.data(), pid_s.size());
    std::cout << "Live JSON → " << *path << " (tower_gui Live Scan, exclusive)\n";
    return 0;
  }

  void write(bool force = false) {
    const auto& live_json_path = ctx_.opt.io.live_json_path;
    if (!live_json_path) return;
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock(mu_);
    if (!force && now - last_write_ < std::chrono::milliseconds(500)) return;
    last_write_ = now;

    auto& opt = ctx_.opt;
    std::map<std::string, std::string> extra;
    extra["updates"] = sfmt("{}", ctx_.updates.load());
    extra["hop_kicks"] = sfmt("{}", ctx_.hop_kicks.load());
    extra["hop_locks"] = sfmt("{}", ctx_.hop_locks.load());
    extra["hop_fulls"] = sfmt("{}", ctx_.hop_fulls.load());
    extra["wcdma_walk_kicks"] = sfmt("{}", ctx_.wcdma_walk_kicks.load());
    extra["wcdma_walk_fulls"] = sfmt("{}", ctx_.wcdma_walk_fulls.load());
    extra["survey_mode"] = std::string(opt.rats == Rats::Auto ? "lte" : to_string(opt.rats));
    extra["job"] = std::string(to_string(opt.job));
    {
      const int kicks = ctx_.hop_kicks.load();
      const int fulls = ctx_.hop_fulls.load();
      if (kicks > 0) extra["hop_hit_rate"] = sfmt("{:.0f}%", 100.0 * fulls / kicks);
    }
    extra["hop_cops"] = sfmt("{}", ctx_.cops_between_kicks.load());
    extra["hop_ghosts"] = sfmt("{}", ctx_.ghost_kicks.load());
    extra["fplmn_wipes"] = sfmt("{}", ctx_.fplmn_wipes.load());
    extra["rat_guard_trips"] = sfmt("{}", ctx_.rat_guard_trips.load());
    extra["scan_rat_ok"] = ctx_.scan_rat_ok.load() ? "1" : "0";
    extra["observed_rat"] = to_string(static_cast<ObservedRat>(ctx_.observed_rat_code.load()));
    extra["survey_phase"] = std::string(QCom::Engine::to_string(
        static_cast<QCom::Engine::SurveyPhase>(ctx_.survey_phase.load(std::memory_order_relaxed))));
    {
      const auto snap = ctx_.engine->tracker().get_snapshot();
      const auto lte = QCom::Engine::project_lte(snap);
      extra["rf_unique"] = sfmt("{}", lte.stats.lte_rf_unique);
      extra["full_passport"] = sfmt("{}", lte.stats.lte_full);
      extra["lte_sites"] = sfmt("{}", lte.stats.lte_sites);
      extra["lte_serving"] = sfmt("{}", lte.stats.lte_serving);
      int wrf = 0, wfull = 0;
      for (const auto& c : snap) {
        if (c.rat != QCom::RatType::WCDMA) continue;
        if (c.radio.freq() == 0 || c.radio.pci_bsic() > 511) continue;
        ++wrf;
        if (cell_is_full_wcdma(c)) ++wfull;
      }
      extra["wcdma_rf"] = sfmt("{}", wrf);
      extra["wcdma_full"] = sfmt("{}", wfull);
    }
    {
      std::vector<std::pair<QCom::LogCode, uint64_t>> ranked;
      {
        std::lock_guard clock(ctx_.code_mu);
        ranked.assign(ctx_.code_hist.begin(), ctx_.code_hist.end());
      }
      std::sort(ranked.begin(), ranked.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });
      const auto& stats = ctx_.engine->parser().code_stats();
      std::string top;
      for (size_t i = 0; i < ranked.size() && i < 8; ++i) {
        const auto code = ranked[i].first;
        uint64_t ev = 0;
        if (auto it = stats.find(code); it != stats.end()) ev = it->second.with_events;
        if (i) top += ',';
        top += sfmt("{:X}:{}/{}", code, ranked[i].second, ev);
      }
      if (!top.empty()) extra["diag_top"] = std::move(top);
    }
    if (auto* linux = dynamic_cast<QCom::LinuxSource*>(ctx_.engine->source())) {
      extra["diag_lte_pack"] = QCom::lte_diag_pack_name(linux->applied_lte_diag_pack());
    }
    if (opt.recipe.ghost)
      extra["ghost_plmn"] =
          format_plmn_numeric(opt.recipe.ghost_plmn.first, opt.recipe.ghost_plmn.second);
    extra["cpsi_ok"] = sfmt("{}", ctx_.cpsi_ok.load());
    extra["qmi_hop_snaps"] = sfmt("{}", ctx_.qmi_hop_snaps.load());
    {
      std::lock_guard qlock(ctx_.qmi_status_mu);
      if (!ctx_.qmi_status.registration.empty()) extra["qmi_reg"] = ctx_.qmi_status.registration;
      if (!ctx_.qmi_status.cs_attach.empty()) extra["qmi_cs"] = ctx_.qmi_status.cs_attach;
      if (!ctx_.qmi_status.ps_attach.empty()) extra["qmi_ps"] = ctx_.qmi_status.ps_attach;
      if (!ctx_.qmi_status.radio.empty()) extra["qmi_radio"] = ctx_.qmi_status.radio;
      if (ctx_.qmi_status.plmn) {
        extra["qmi_plmn"] =
            sfmt("{:03}-{:02}", ctx_.qmi_status.plmn->mcc, ctx_.qmi_status.plmn->mnc);
      }
      if (!ctx_.qmi_status.plmn_name.empty()) extra["qmi_plmn_name"] = ctx_.qmi_status.plmn_name;
      if (ctx_.qmi_status.roaming_indicator)
        extra["qmi_roam"] = std::to_string(*ctx_.qmi_status.roaming_indicator);
      if (ctx_.qmi_status.lte_rsrp_dbm)
        extra["qmi_rsrp"] = std::to_string(static_cast<int>(*ctx_.qmi_status.lte_rsrp_dbm));
      if (ctx_.qmi_status.lte_rsrq_db)
        extra["qmi_rsrq"] = std::to_string(static_cast<int>(*ctx_.qmi_status.lte_rsrq_db));
      if (ctx_.qmi_status.lte_rssi_dbm)
        extra["qmi_rssi"] = std::to_string(static_cast<int>(*ctx_.qmi_status.lte_rssi_dbm));
      if (ctx_.qmi_status.lte_snr_db)
        extra["qmi_snr"] = sfmt("{:.1f}", *ctx_.qmi_status.lte_snr_db);
      if (ctx_.qmi_status.wcdma_rssi_dbm)
        extra["qmi_wcdma_rssi"] = std::to_string(static_cast<int>(*ctx_.qmi_status.wcdma_rssi_dbm));
      if (ctx_.qmi_status.wcdma_ecio_db)
        extra["qmi_wcdma_ecio"] = sfmt("{:.1f}", *ctx_.qmi_status.wcdma_ecio_db);
    }
    if (auto log = ctx_.dash->notes_joined(); !log.empty()) extra["scanner_log"] = std::move(log);
    if (!ctx_.dash->log_path().empty()) extra["scanner_log_file"] = ctx_.dash->log_path();
    if (!ctx_.dash->log_session_path().empty())
      extra["scanner_log_session"] = ctx_.dash->log_session_path();

    auto snap = ctx_.engine->tracker().get_snapshot();
    if (opt.wcdma_only()) {
      snap.erase(std::remove_if(snap.begin(), snap.end(),
                                [](const QCom::CellIdentity& c) {
                                  return c.rat == QCom::RatType::LTE || c.rat == QCom::RatType::NR;
                                }),
                 snap.end());
    } else if (opt.rats == Rats::Lte) {
      snap.erase(std::remove_if(snap.begin(), snap.end(),
                                [](const QCom::CellIdentity& c) {
                                  return c.rat == QCom::RatType::WCDMA;
                                }),
                 snap.end());
    }
    snap.erase(std::remove_if(snap.begin(), snap.end(),
                              [](const QCom::CellIdentity& c) {
                                if (c.rat != QCom::RatType::LTE) return false;
                                if (!QCom::Utils::valid_lte_earfcn(c.radio.freq())) return true;
                                if (c.radio.pci_bsic() == 0 || c.radio.pci_bsic() > 503) return true;
                                if (c.passport.mcc == 0xFFFF || c.passport.mnc == 0xFFFF ||
                                    c.passport.mcc > 999)
                                  return true;
                                return false;
                              }),
               snap.end());
    (void)QCom::Tools::write_towers_json_survey(*live_json_path, snap, "live_scanner", extra);
  }

  void on_cells(const std::vector<QCom::CellIdentity>& cells) {
    if (cells.empty()) return;
    std::vector<QCom::CellIdentity> filtered;
    const std::vector<QCom::CellIdentity>* view = &cells;
    if (ctx_.opt.wcdma_only()) {
      filtered = cells;
      filtered.erase(std::remove_if(filtered.begin(), filtered.end(),
                                    [](const QCom::CellIdentity& c) {
                                      return c.rat == QCom::RatType::LTE ||
                                             c.rat == QCom::RatType::NR;
                                    }),
                     filtered.end());
      view = &filtered;
    }
    if (view->empty()) return;
    const std::string fp = snapshot_fingerprint(*view);
    std::lock_guard lock(ctx_.fp_mu);
    if (fp == ctx_.last_fp) return;
    ctx_.last_fp = fp;
    const int n = ++ctx_.updates;
    ctx_.dash->show_cells(*view, n);
    write(false);
  }

  void on_packet(QCom::QualcommPacketView pkt) {
    std::lock_guard lock(ctx_.code_mu);
    ++ctx_.code_hist[pkt.log_code];
    if (pkt.log_code == 0xB0C0) {
      if (auto ota = QCom::Lte::decode_lte_rrc_ota(pkt.payload)) {
        ++ctx_.b0c0_pdu_hist[static_cast<int>(ota->pdu_num)];
        ++ctx_.b0c0_ver_hist[static_cast<int>(ota->version)];
      } else {
        ++ctx_.b0c0_pdu_hist[-1];
      }
    }
  }

  void close() {
    if (lock_fd_ >= 0) {
      ::close(lock_fd_);
      lock_fd_ = -1;
    }
  }

private:
  SurveyCtx& ctx_;
  std::mutex mu_;
  std::chrono::steady_clock::time_point last_write_{
      std::chrono::steady_clock::now() - std::chrono::seconds(10)};
  int lock_fd_{-1};
};

}  // namespace Observer
