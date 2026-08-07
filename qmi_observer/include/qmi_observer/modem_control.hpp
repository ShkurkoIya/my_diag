#pragma once

/**
 * @file modem_control.hpp
 * @brief Control-plane helpers (SSP, force search, cautious RF recover).
 */

#include "qmi_observer/error.hpp"
#include "qmi_observer/types.hpp"

#include <vector>

namespace qmi_observer {

class Session;

/**
 * @brief Mutates modem policy. Always go through @ref ScanController::ensure_ready
 *        (or check @ref ModemHealth flags) before calling.
 */
class ModemControl {
 public:
  explicit ModemControl(Session& session);

  /**
   * @brief Set NAS mode preference (UMTS/LTE/…).
   * @note Does not use DMS offline. May require a short settle afterwards.
   */
  [[nodiscard]] Result<void> set_mode_preference(const std::vector<Rat>& modes);

  [[nodiscard]] Result<std::vector<Rat>> get_mode_preference();

  [[nodiscard]] Result<void> force_network_search();

  /**
   * @brief Best-effort return to DMS online (low-power → online).
   *        Never uses offline unless Settings::allow_dms_offline.
   */
  [[nodiscard]] Result<void> try_recover_online();

 private:
  Session& session_;
};

}  // namespace qmi_observer
