#pragma once

/**
 * @file port.hpp
 * @brief Описание USB/символьных портов модема.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace QCom::Qmi::device {

/**
 * @brief Роль порта в композитном USB-модеме.
 */
enum class PortRole : uint8_t {
  Unknown = 0,
  Qmi,    ///< /dev/cdc-wdm* (qmi_wwan / qmi_wwan_simcom)
  At,     ///< AT-команды (обычно ttyUSB с interface hint из профиля)
  Nmea,   ///< GNSS NMEA
  Diag,   ///< DIAG / отладка baseband
  Modem,  ///< PPP/modem data
  Audio,  ///< голосовой/PCM USB (часто iface 4 у SimCom)
  Mbim,   ///< MBIM control
  Net,    ///< wwan сетевой интерфейс (имя, не /dev)
};

[[nodiscard]] constexpr std::string_view to_string(PortRole r) noexcept {
  switch (r) {
    case PortRole::Qmi: return "qmi";
    case PortRole::At: return "at";
    case PortRole::Nmea: return "nmea";
    case PortRole::Diag: return "diag";
    case PortRole::Modem: return "modem";
    case PortRole::Audio: return "audio";
    case PortRole::Mbim: return "mbim";
    case PortRole::Net: return "net";
    default: return "unknown";
  }
}

/**
 * @brief Один обнаруженный порт.
 */
struct PortDesc {
  std::string path;                 ///< /dev/cdc-wdm0, /dev/ttyUSB2
  PortRole role{PortRole::Unknown};
  std::optional<uint8_t> usb_interface;  ///< bInterfaceNumber, если известен
  std::string driver;               ///< option, qmi_wwan, ...
  std::string sysfs_iface;          ///< путь к USB interface в sysfs (для отладки)
};

}  // namespace QCom::Qmi::device
