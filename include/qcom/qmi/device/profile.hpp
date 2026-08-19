#pragma once

/**
 * @file profile.hpp
 * @brief Статические рецепты модемов (не зависят от текущего USB-состояния).
 *
 * Профили живут в коде/конфиге и отвечают на вопрос «как правильно
 * обращаться с этим железом», а не «что сейчас воткнуто».
 */

#include <qcom/qmi/device/port.hpp>
#include <qcom/qmi/types.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace QCom::Qmi::device {

/**
 * @brief Критерий сопоставления USB-устройства со статическим профилем.
 */
struct ProfileMatch {
  std::optional<uint16_t> vid;
  std::optional<uint16_t> pid;
  std::string product_substr;       ///< подстрока product (case-sensitive)
  std::string manufacturer_substr;
};

/**
 * @brief Quirks и политика open для конкретного семейства.
 */
struct ProfileQuirks {
  bool allow_dms_offline{false};   ///< по умолчанию запрещено
  bool prefer_qmi_proxy{false};    ///< true, если обычно рядом ModemManager
  bool open_auto{false};           ///< QMI DEVICE_OPEN_FLAGS_AUTO
  bool at_probe_safe{true};        ///< можно коротко стучать AT на preferred iface
};

/**
 * @brief Статический профиль семейства модемов.
 */
struct ModemProfile {
  std::string id;                  ///< стабильный id: "simcom_83xx_qmi"
  std::string display_name;        ///< человекочитаемое имя
  ProfileMatch match;
  /// USB interface number → ожидаемая роль (SimCom: 0 diag, 1 nmea, 2 at, ...)
  std::vector<std::pair<uint8_t, PortRole>> interface_roles;
  std::optional<uint8_t> preferred_at_interface{2};
  ProfileQuirks quirks{};
  std::vector<Rat> preferred_modes{Rat::Lte, Rat::Wcdma};
  std::string notes;               ///< свободный текст для оператора/дока
};

/**
 * @brief Встроенный реестр профилей + поиск.
 */
class ProfileRegistry {
 public:
  ProfileRegistry();

  [[nodiscard]] const std::vector<ModemProfile>& all() const noexcept { return profiles_; }

  [[nodiscard]] const ModemProfile* find_by_id(std::string_view id) const noexcept;

  /**
   * @brief Подобрать лучший профиль по VID/PID/строкам.
   * @return nullptr, если только generic fallback нежелателен — тогда вызывайте
   *         @ref generic_qmi_profile.
   */
  [[nodiscard]] const ModemProfile* match(uint16_t vid, uint16_t pid,
                                          std::string_view manufacturer,
                                          std::string_view product) const noexcept;

  [[nodiscard]] static const ModemProfile& generic_qmi_profile() noexcept;

 private:
  std::vector<ModemProfile> profiles_;
};

}  // namespace QCom::Qmi::device
