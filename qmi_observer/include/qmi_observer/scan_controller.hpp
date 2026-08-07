#pragma once

/**
 * @file scan_controller.hpp
 * @brief Business orchestration: readiness gate → policy → snapshot → aggregate.
 */

#include "qmi_observer/error.hpp"
#include "qmi_observer/health.hpp"
#include "qmi_observer/settings.hpp"
#include "qmi_observer/types.hpp"

namespace qmi_observer {

class Session;

/**
 * @brief Scanner-facing API. Hides GLib/QMI and enforces the health gate.
 *
 * @par Flow for @ref collect_once
 * 1. @ref ensure_ready — refresh health; fail on NeedsRecover / OfflineRf
 * 2. optional ensure preferred modes (SSP)
 * 3. optional force search + settle
 * 4. wait until Camped or camp_wait expires
 * 5. snapshot (NoNetwork → empty if policy says so)
 * 6. merge rounds → @ref AggregatedCells
 */
class ScanController {
 public:
  explicit ScanController(Session& session, ScanPolicy policy = {});

  [[nodiscard]] ScanPolicy& policy() noexcept { return policy_; }
  [[nodiscard]] const ScanPolicy& policy() const noexcept { return policy_; }

  /**
   * @brief Refresh health and verify control/observe are allowed.
   * @return Current health on success.
   */
  [[nodiscard]] Result<ModemHealth> ensure_ready();

  [[nodiscard]] Result<AggregatedCells> collect_once();

 private:
  Session& session_;
  ScanPolicy policy_;
};

}  // namespace qmi_observer
