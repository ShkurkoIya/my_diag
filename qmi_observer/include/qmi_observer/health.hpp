#pragma once

/**
 * @file health.hpp
 * @brief Modem readiness model — pure evaluation, no libqmi dependency.
 *
 * @par Business rules (from live SIM8300/SDX55 bring-up)
 * - Limited / registration-denied with a serving cell is still @c Camped
 *   (snapshot allowed) — e.g. Tele2 UMTS while home is MTS.
 * - @c Searching + NoNetworkFound is not fatal; wait or return empty.
 * - DMS @c offline / DeviceNotReady ⇒ @c NeedsRecover; do not force-search.
 * - SIM not ready ⇒ block control that assumes USIM.
 */

#include "qmi_observer/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qmi_observer {

/**
 * @brief Coarse modem lifecycle as seen by the scanner.
 */
enum class ModemPhase : uint8_t {
  Absent = 0,      ///< Device node missing / not opened.
  Unavailable,     ///< Open/permission/proxy failure.
  OfflineRf,       ///< DMS not online (offline / low-power / unknown).
  OnlineIdle,      ///< RF online, NAS not yet classified.
  Searching,       ///< not-registered-searching / radio none mid-scan.
  Camped,          ///< Has usable serving/neighbor context (incl. limited).
  Fault,           ///< Stuck transitions / internal QMI faults.
};

[[nodiscard]] std::string_view to_string(ModemPhase p) noexcept;

enum class RegistrationKind : uint8_t {
  Unknown = 0,
  NotRegistered,
  Searching,
  Registered,
  Denied,
};

[[nodiscard]] std::string_view to_string(RegistrationKind r) noexcept;

enum class OperatingModeKind : uint8_t {
  Unknown = 0,
  Online,
  LowPower,
  Offline,
  Reset,
  Other,
};

[[nodiscard]] std::string_view to_string(OperatingModeKind m) noexcept;

/**
 * @brief Raw inputs collected from DMS/NAS/UIM (filled by HealthMonitor).
 */
struct HealthProbe {
  bool session_open{false};
  bool device_node_present{true};
  OperatingModeKind operating_mode{OperatingModeKind::Unknown};
  bool sim_ready{false};
  bool sim_known{false};  ///< false if UIM probe skipped/failed
  RegistrationKind registration{RegistrationKind::Unknown};
  std::optional<Rat> radio;
  std::optional<Plmn> home_plmn;
  std::optional<Plmn> current_plmn;
  std::vector<Rat> mode_preference;  ///< Current SSP mode preference list
  bool last_cell_location_ok{false};
  bool last_cell_location_no_network{false};
  std::optional<std::string> last_error;
};

/**
 * @brief Evaluated readiness + capability flags for the health gate.
 */
struct ModemHealth {
  ModemPhase phase{ModemPhase::Absent};
  HealthProbe probe{};

  bool can_snapshot{false};      ///< Safe to call NAS Get Cell Location Info.
  bool can_force_search{false};  ///< Safe to force network search.
  bool can_change_ssp{false};    ///< Safe to set system selection preference.
  bool needs_recover{false};     ///< Operator should reset / low-power→online.

  std::string summary;  ///< Short human-readable status line.
};

/**
 * @brief Pure function: map @p probe → @ref ModemHealth.
 *
 * Unit-tested; keep side-effect free.
 */
[[nodiscard]] ModemHealth evaluate_health(HealthProbe probe);

/**
 * @brief True if @p have covers every RAT in @p want (order ignored).
 */
[[nodiscard]] bool mode_preference_covers(const std::vector<Rat>& have,
                                          const std::vector<Rat>& want) noexcept;

}  // namespace qmi_observer
