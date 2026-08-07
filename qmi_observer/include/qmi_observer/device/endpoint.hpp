#pragma once

/**
 * @file endpoint.hpp
 * @brief Живой экземпляр модема на хосте (результат enumerate).
 */

#include "qmi_observer/device/port.hpp"
#include "qmi_observer/device/profile.hpp"
#include "qmi_observer/settings.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace qmi_observer::device {

/**
 * @brief Обнаруженный модем (один USB device = один endpoint).
 *
 * @par Стабильный id
 * Предпочтительно @c vid:pid:iSerial. Если serial пустой/фейковый — fallback
 * на USB bus path (@c 1-4), который меняется при перетыкании в другой порт.
 */
struct ModemEndpoint {
  std::string id;
  uint16_t vid{0};
  uint16_t pid{0};
  std::string usb_path;          ///< sysfs имя: "1-4"
  std::string serial;
  std::string manufacturer;
  std::string product;
  std::string matched_profile_id;
  std::vector<PortDesc> ports;
  bool serial_is_stable{true};   ///< false, если id построен только из bus path

  [[nodiscard]] std::optional<std::string> qmi_path() const;
  [[nodiscard]] std::optional<std::string> preferred_at_path() const;
  [[nodiscard]] std::optional<std::string> preferred_diag_path() const;
  [[nodiscard]] std::vector<std::string> paths_with_role(PortRole role) const;

  /**
   * @brief Собрать Settings для @ref Session из endpoint + профиля.
   */
  [[nodiscard]] Settings to_qmi_settings(const ModemProfile* profile = nullptr) const;
};

/**
 * @brief Построить стабильный id.
 */
[[nodiscard]] std::string make_endpoint_id(uint16_t vid, uint16_t pid,
                                           std::string_view serial,
                                           std::string_view usb_path,
                                           bool& serial_stable);

}  // namespace qmi_observer::device
