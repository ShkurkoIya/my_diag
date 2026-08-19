/// @file DiagSourceConfig.h
/// @brief Platform-neutral config for opening a DIAG data source.
#pragma once

#include <cstdint>
#include <string>

// uint8_t used by DiagMaskProfile

namespace QCom {

/// Which DIAG equipment masks to enable at session start.
enum class DiagMaskProfile : uint8_t {
  AllRats = 0,   ///< LTE(+NR) + GSM + WCDMA (default survey)
  WcdmaOnly = 1, ///< WCDMA (+ light GSM IRAT); LTE/NR masks cleared
  LteOnly = 2,   ///< LTE(+NR) only; WCDMA cleared
};

/// LTE item subset inside the equipment mask (never DISABLE_ALL to switch).
/// Search drops PSS/TA/CER flood so RRC OTA can survive the MPSS log ring;
/// SSS (B115) stays — it is the SIM8300 COPS=? cell-mint path.
enum class LteDiagPack : uint8_t {
  Search = 0,   ///< RRC + SSS/ML1/NAS; no B113/B114/B123
  Serving = 1,  ///< search + B114 TA while camped soak
};

[[nodiscard]] inline const char* lte_diag_pack_name(LteDiagPack pack) noexcept {
  switch (pack) {
    case LteDiagPack::Serving: return "serving";
    case LteDiagPack::Search:
    default: return "search";
  }
}

struct DiagSourceConfig {
  std::string device_path{"/dev/ttyUSB0"};  ///< Linux DIAG tty; unused on Android DCI today
  int baud_rate{921600};
  bool init_masks{true};  ///< Linux: run DiagSession::init_modem(); Android: set via libdiag
  DiagMaskProfile mask_profile{DiagMaskProfile::AllRats};
};

}  // namespace QCom
