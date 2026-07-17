#pragma once

#include <cstdint>
#include <string>

namespace observer_qcom_parser {
enum class LogCode : uint16_t {
  // --- 4G LTE ---
  LTE_RRC_OTA = 0xB0C0,     // Сигналка LTE RRC (SIB-ы, Handover)
  LTE_ML1_METRICS = 0xB17C, // Физика LTE L1 (RSRP, RSRQ, SINR)

  // --- 5G NR ---
  NR_RRC_OTA = 0xB193,     // Сигналка 5G NR RRC
  NR_ML1_METRICS = 0xB1A1, // Физика 5G NR L1 (Лучи Beamforming, BRSRP)

  // --- 3G WCDMA ---
  WCDMA_RRC_OTA = 0x412F,   // Сигналка 3G RRC
  WCDMA_CELL_LIST = 0x4115, // Физика/измерения соседей 3G

  // --- 2G GSM ---
  GSM_RR_SIGNALS = 0x51FC, // Сигналка 2G GSM
  GSM_L1_METRICS = 0x5A2A  // Физика/измерения 2G
};

inline std::string to_string(LogCode code) {
  switch (code) {
  case LogCode::LTE_RRC_OTA:
    return "LTE RRC OTA (0xB0C0)";
  case LogCode::LTE_ML1_METRICS:
    return "LTE ML1 Metrics (0xB17C)";
  case LogCode::NR_RRC_OTA:
    return "NR RRC OTA (0xB193)";
  case LogCode::NR_ML1_METRICS:
    return "NR ML1 Metrics (0xB1A1)";
  case LogCode::WCDMA_RRC_OTA:
    return "WCDMA RRC OTA (0x412F)";
  case LogCode::WCDMA_CELL_LIST:
    return "WCDMA Cell List (0x4115)";
  case LogCode::GSM_RR_SIGNALS:
    return "GSM RR Signals (0x51FC)";
  case LogCode::GSM_L1_METRICS:
    return "GSM L1 Metrics (0x5A2A)";
  }
  return "Unknown Log Code";
}
} // namespace observer_qcom_parser
