#include <qcom/qmi/scan_controller.hpp>

#include <qcom/qmi/modem_control.hpp>
#include <qcom/qmi/nas_reader.hpp>
#include <qcom/qmi/session.hpp>

#include "detail/session_impl.hpp"

#include <chrono>
#include <thread>

namespace QCom::Qmi {

ScanController::ScanController(Session& session, ScanPolicy policy)
    : session_(session), policy_(std::move(policy)) {}

Result<ModemHealth> ScanController::ensure_ready() {
  auto health = session_.refresh_health();
  if (!health) {
    return health.error();
  }
  const auto& h = health.value();
  if (h.needs_recover || h.phase == ModemPhase::OfflineRf || h.phase == ModemPhase::Fault) {
    return Error::from(Errc::NeedsRecover, h.summary);
  }
  if (h.phase == ModemPhase::Absent || h.phase == ModemPhase::Unavailable) {
    return Error::from(Errc::NotReady, h.summary);
  }
  return h;
}

Result<AggregatedCells> ScanController::collect_once() {
  auto ready = ensure_ready();
  if (!ready) {
    session_.impl().emit_error(ready.error());
    return ready.error();
  }

  ScanPolicy pol = policy_;
  if (pol.settle_time.count() == 0) {
    pol.settle_time = session_.settings().settle_time;
  }
  if (pol.camp_wait.count() == 0) {
    pol.camp_wait = session_.settings().camp_wait;
  }
  const uint32_t rounds = pol.max_rounds == 0 ? 1 : pol.max_rounds;

  if (pol.ensure_preferred_modes && !session_.settings().preferred_modes.empty()) {
    if (!mode_preference_covers(ready.value().probe.mode_preference,
                                session_.settings().preferred_modes)) {
      if (!ready.value().can_change_ssp) {
        return Error::from(Errc::WrongConfig, "cannot change SSP in current health state");
      }
      if (auto set = session_.control().set_mode_preference(session_.settings().preferred_modes);
          !set) {
        session_.impl().emit_error(set.error());
        return set.error();
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
      ready = ensure_ready();
      if (!ready) {
        return ready.error();
      }
    }
  }

  AggregatedCells out;

  for (uint32_t round = 0; round < rounds; ++round) {
    if (pol.force_search) {
      if (!session_.last_health().can_force_search &&
          session_.last_health().phase != ModemPhase::Searching &&
          session_.last_health().phase != ModemPhase::Camped &&
          session_.last_health().phase != ModemPhase::OnlineIdle) {
        return Error::from(Errc::NotReady, "force search not allowed");
      }
      if (auto fs = session_.control().force_network_search(); !fs) {
        // DeviceNotReady mid-search is soft if we can still snapshot later.
        if (fs.error().code != Errc::RequestFailed) {
          session_.impl().emit_error(fs.error());
          return fs.error();
        }
      }
    }

    if (pol.settle_time.count() > 0) {
      std::this_thread::sleep_for(pol.settle_time);
    }

    const auto deadline = std::chrono::steady_clock::now() + pol.camp_wait;
    CellSnapshot snap;
    for (;;) {
      auto got = session_.nas().snapshot_cells();
      if (got) {
        snap = std::move(got.value());
        break;
      }
      if (got.error().code == Errc::NoNetwork && pol.treat_no_network_as_empty) {
        if (std::chrono::steady_clock::now() >= deadline) {
          snap = {};
          break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
        (void)session_.refresh_health();
        continue;
      }
      session_.impl().emit_error(got.error());
      return got.error();
    }

    merge_snapshot(out, snap);
  }

  return out;
}

}  // namespace QCom::Qmi
