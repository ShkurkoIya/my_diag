#pragma once

/**
 * @file settings.hpp
 * @brief User-facing configuration for Session / ScanController.
 */

#include "qmi_observer/types.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace qmi_observer {

/**
 * @brief Connection + safety policy for a QMI session.
 *
 * @note @ref allow_dms_offline defaults to false — DMS offline on SDX55/SimCom
 *       has been observed to brick RF until reset/replug (@c InvalidTransition).
 */
struct Settings {
  /// QMI port, e.g. "/dev/cdc-wdm0".
  std::string device_path;

  /// Share the port with ModemManager via qmi-proxy.
  bool use_proxy{true};

  /// Prefer MBIM/QMI auto open when the kernel exposes cdc_mbim.
  bool open_auto{false};

  std::chrono::milliseconds request_timeout{std::chrono::seconds(10)};

  /**
   * @brief If true, HealthMonitor/ModemControl may call DMS offline.
   *        Keep false unless you own a tested recover path.
   */
  bool allow_dms_offline{false};

  /// Preferred RAT mask for ScanController::ensure_modes (empty = do not change).
  std::vector<Rat> preferred_modes{Rat::Lte, Rat::Wcdma};

  /// How long collect waits in Searching before giving up / snapshot anyway.
  std::chrono::milliseconds camp_wait{std::chrono::seconds(45)};

  /// Sleep after force-search before the first snapshot poll.
  std::chrono::milliseconds settle_time{std::chrono::seconds(2)};

  uint32_t collect_rounds{1};
};

/**
 * @brief Per-collect orchestration knobs (overrides a subset of Settings).
 */
struct ScanPolicy {
  bool force_search{true};
  bool ensure_preferred_modes{true};
  bool treat_no_network_as_empty{true};  ///< Searching → empty snapshot, not hard fail
  std::chrono::milliseconds settle_time{std::chrono::seconds(2)};
  std::chrono::milliseconds camp_wait{std::chrono::seconds(45)};
  uint32_t max_rounds{1};
};

}  // namespace qmi_observer
