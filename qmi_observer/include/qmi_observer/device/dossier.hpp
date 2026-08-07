#pragma once

/**
 * @file dossier.hpp
 * @brief Runtime-досье: результат простукивания конкретного endpoint.
 *
 * Хранится на диске и отдаётся по запросу вместе со static profile.
 */

#include "qmi_observer/device/endpoint.hpp"
#include "qmi_observer/health.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qmi_observer::device {

/**
 * @brief Уровень простукивания (дороже = глубже).
 */
enum class ProbeLevel : uint8_t {
  Presence = 0,   ///< только sysfs /dev
  Transport = 1,  ///< open QMI (+ опционально AT)
  Identity = 2,   ///< DMS / ATI
  Radio = 3,      ///< NAS health / короткий snapshot
};

[[nodiscard]] constexpr std::string_view to_string(ProbeLevel l) noexcept {
  switch (l) {
    case ProbeLevel::Presence: return "presence";
    case ProbeLevel::Transport: return "transport";
    case ProbeLevel::Identity: return "identity";
    case ProbeLevel::Radio: return "radio";
  }
  return "unknown";
}

/**
 * @brief Кэш результатов probe для одного endpoint_id.
 */
struct ModemDossier {
  std::string endpoint_id;
  std::string matched_profile_id;

  std::optional<std::string> qmi_path;
  std::vector<std::string> at_paths;

  bool qmi_open_ok{false};
  bool at_ok{false};

  std::optional<std::string> dms_manufacturer;
  std::optional<std::string> dms_model;
  std::optional<std::string> dms_revision;
  std::optional<std::string> at_identity;

  std::optional<ModemPhase> last_phase;
  std::optional<std::string> last_health_summary;
  bool last_snapshot_ok{false};

  ProbeLevel deepest_probe{ProbeLevel::Presence};
  int64_t probed_at_unix{0};   ///< time_t
  std::string last_error;
};

/**
 * @brief Merge endpoint + profile + dossier для ответа API/GUI.
 */
struct ModemReport {
  ModemEndpoint endpoint;
  const ModemProfile* profile{nullptr};
  std::optional<ModemDossier> dossier;
};

}  // namespace qmi_observer::device
