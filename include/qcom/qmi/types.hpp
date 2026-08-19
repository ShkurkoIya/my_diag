#pragma once

/**
 * @file types.hpp
 * @brief Domain DTOs for QMI NAS observations (wire-friendly, optional fields).
 *
 * These intentionally stay independent of @c QCom::CellIdentity so the library
 * can be tested without the DIAG stack. Use @ref bridge.hpp to emit
 * @c QCom::Events::RrcEventEnvelope into observer::model's CellTracker.
 */

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace QCom::Qmi {

enum class Rat : uint8_t {
  Unknown = 0,
  Gsm = 2,
  Wcdma = 3,
  Lte = 4,
  Nr = 5,
};

[[nodiscard]] constexpr std::string_view to_string(Rat r) noexcept {
  switch (r) {
    case Rat::Gsm: return "GSM";
    case Rat::Wcdma: return "WCDMA";
    case Rat::Lte: return "LTE";
    case Rat::Nr: return "NR";
    default: return "UNKNOWN";
  }
}

struct Plmn {
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint8_t mnc_digits{2};  ///< 2 or 3

  [[nodiscard]] constexpr bool valid() const noexcept { return mcc != 0; }
  auto operator<=>(const Plmn&) const = default;
};

struct ModemIdentity {
  std::string manufacturer;
  std::string model;
  std::string revision;
};

/**
 * @brief One cell row from NAS Get Cell Location Info (serving or neighbor).
 *
 * Missing optionals mean the modem did not provide the TLV — do not invent values.
 */
struct CellObservation {
  Rat rat{Rat::Unknown};
  std::optional<Plmn> plmn;
  std::optional<uint32_t> lac_or_tac;
  std::optional<uint64_t> cell_id;       ///< CGI / ECI / NCI / 28-bit UMTS when known
  std::optional<uint32_t> rf_channel;    ///< ARFCN / UARFCN / EARFCN / NR-ARFCN
  std::optional<uint16_t> phy_id;        ///< BSIC / PSC / PCI
  std::optional<float> rsrp_dbm;         ///< LTE/NR RSRP or UMTS RSCP (dBm)
  std::optional<float> rsrq_db;          ///< LTE/NR RSRQ or UMTS Ec/Io (dB)
  std::optional<float> rssi_dbm;
  bool serving{false};
  /// From NAS LTE intra-freq / inter-freq TLVs — previously discarded.
  std::optional<bool> idle;
  std::optional<uint8_t> cell_resel_prio;
  std::optional<uint8_t> s_non_intra_search;
  std::optional<uint8_t> thresh_serving_low;
  std::optional<uint8_t> s_intra_search;
  std::optional<uint8_t> thresh_x_low;   ///< inter-freq cell selection low
  std::optional<uint8_t> thresh_x_high;  ///< inter-freq cell selection high
  std::optional<float> snr_db;           ///< LTE/NR SINR when present
  std::optional<uint32_t> timing_advance;
};

struct CellSnapshot {
  std::vector<CellObservation> cells;
};

/**
 * @brief Compact NAS serving-system + signal-info (what qmicli shows beyond cell list).
 */
struct NasRadioStatus {
  std::string registration;   ///< registered / searching / denied / …
  std::string ps_attach;      ///< attached / detached / unknown
  std::string cs_attach;
  std::string radio;          ///< lte / umts / gsm / …
  std::optional<Plmn> plmn;
  std::string plmn_name;      ///< operator description if modem provides it
  std::optional<uint8_t> roaming_indicator;
  std::optional<float> lte_rsrp_dbm;
  std::optional<float> lte_rsrq_db;
  std::optional<float> lte_rssi_dbm;
  std::optional<float> lte_snr_db;
  std::optional<float> wcdma_rssi_dbm;
  std::optional<float> wcdma_ecio_db;
};

struct AggregatedCells {
  std::vector<CellObservation> cells;
};

/**
 * @brief Stable merge key for aggregation across collect rounds.
 * @return rat|rf|phy|cid string (missing fields become "-").
 */
[[nodiscard]] std::string cell_merge_key(const CellObservation& c);

/**
 * @brief Merge @p src into @p dst, skipping duplicate @ref cell_merge_key entries.
 */
void merge_snapshot(AggregatedCells& dst, const CellSnapshot& src);

}  // namespace QCom::Qmi
