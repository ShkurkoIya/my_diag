/// @file CellIdentity.h
/// @brief Core data model for multi-RAT cell identity, radio parameters, and signal quality.
///
/// Designed for forensics, radio monitoring, and GUI visualization.
/// All RAT-specific parameters live in std::variant discriminated unions
/// so a single CellIdentity can represent GSM/WCDMA/LTE/NR.
///
/// Key concepts:
///   - LocalCellKey (freq + PCI/BSIC) — physical layer identity, used for merging
///   - CellPassport — logical identity from SIB1 (MCC/MNC/TAC/CellID)
///   - CellRadio — RF parameters from SIBs (reselection thresholds, bandwidth, neighbors)
///   - CellSignal — live measurements (RSRP/RSRQ/SINR from ML1 and MeasurementReport)
#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace QCom {

/// Radio Access Technology discriminator
enum class RatType : uint8_t { GSM = 2, WCDMA = 3, LTE = 4, NR = 5, UNKNOWN = 0 };

inline std::string to_string(RatType r) {
  switch (r) {
    case RatType::GSM: return "GSM";
    case RatType::WCDMA: return "WCDMA";
    case RatType::LTE: return "LTE";
    case RatType::NR: return "NR";
    default: return "UNKNOWN";
  }
}

/// Global cell identity — unique worldwide (from SIB1 / NAS)
struct GlobalCellKey {
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint32_t tac{0};
  uint64_t cell_id{0};
  auto operator<=>(const GlobalCellKey&) const = default;
};

/// Physical-layer cell identity — unique per frequency (from ML1 / RRC OTA header)
struct LocalCellKey {
  uint32_t freq{0};      ///< EARFCN / NR-ARFCN / UARFCN / ARFCN
  uint16_t pci_bsic{0};  ///< PCI (LTE/NR) / PSC (WCDMA) / BSIC (GSM)
  auto operator<=>(const LocalCellKey&) const = default;
};

// ============================================================================
// Cell Passport — logical identity from SIB1 / NAS Attach Accept
// ============================================================================

/// @brief Full cell identity extracted from SIB1 (LTE/NR) or NAS TAI.
///
/// Fields relevant for:
///   - Forensics: cell_id (28-bit ECI / 36-bit NCI), MCC/MNC, TAC
///   - IMSI catcher detection: cell_barred, csg_ind, csg_id
///   - Network analysis: q_rx_lev_min, intra_freq_reselection_allowed
struct CellPassport {
  uint32_t tac{0};      ///< Tracking Area Code (16-bit LTE / 24-bit NR)
  uint64_t cell_id{0};  ///< Cell Identity (28-bit ECI for LTE, 36-bit NCI for NR)
  uint16_t mcc{0};      ///< Mobile Country Code (e.g. 250 = Russia)
  uint16_t mnc{0};      ///< Mobile Network Code (e.g. 01 = MTS)

  bool cell_barred{false};  ///< SIB1: cell is barred for access
  int8_t q_rx_lev_min{0};   ///< SIB1: minimum required RSRP (dBm * 2)
  uint8_t q_rx_lev_min_offset{0};
  bool intra_freq_reselection_allowed{true};  ///< SIB1: intra-freq cell reselection

  bool csg_ind{false};  ///< SIB1: Closed Subscriber Group indicator
  uint32_t csg_id{0};   ///< SIB1: CSG ID (27-bit) — private cell group

  uint8_t freq_band_ind{0};  ///< SIB1: frequency band indicator (e.g. 7 = 2600 MHz)

  [[nodiscard]] constexpr bool has_identity() const noexcept { return cell_id > 0; }
  auto operator<=>(const CellPassport&) const = default;
};

// ============================================================================
// RAT-specific radio parameters from SIBs
// ============================================================================

/// @brief LTE radio parameters aggregated from SIB1/SIB2/SIB3/MIB.
///
/// Used for:
///   - Band identification (freq_band_ind + earfcn -> band number)
///   - Bandwidth display (dl_bw, ul_bw in MHz)
///   - Cell reselection analysis (q_hyst, t_resel, s_intra_search)
///   - Access barring detection (ac_barr_*)
struct LteRadioParams {
  uint32_t earfcn{0};  ///< E-UTRA Absolute Radio Freq Channel Number
  uint16_t pci{0};     ///< Physical Cell ID (0-503)

  // MIB
  uint8_t dl_bw{0};           ///< Downlink bandwidth in MHz (1,3,5,10,15,20)
  uint8_t phich_duration{0};  ///< PHICH duration: 0=normal, 1=extended
  uint8_t phich_resource{0};  ///< PHICH resource: 0=1/6, 1=1/2, 2=1, 3=2
  uint16_t sfn{0};            ///< System Frame Number (0-1023)

  // SIB1
  uint8_t freq_band_ind{0};  ///< Frequency band indicator

  // SIB2
  uint8_t ul_bw{0};                  ///< Uplink bandwidth (resource blocks)
  uint32_t ul_earfcn{0};             ///< UL carrier frequency (if different from DL)
  bool ac_barr_emergency{false};     ///< Access barring for emergency
  bool ac_barr_mo_signaling{false};  ///< Access barring for MO signaling
  bool ac_barr_mo_data{false};       ///< Access barring for MO data

  // SIB3
  uint8_t q_hyst{0};              ///< Cell reselection hysteresis (dB)
  uint8_t t_resel_eutra{0};       ///< Cell reselection timer (seconds)
  int8_t s_intra_search{0};       ///< Threshold for intra-freq meas (dB)
  int8_t s_non_intra_search{0};   ///< Threshold for inter-freq meas (dB)
  uint8_t thresh_serving_low{0};  ///< Threshold for serving cell (dB)

  auto operator<=>(const LteRadioParams&) const = default;
};

/// @brief NR radio parameters from SIB1/SIB2/SIB3.
struct NrRadioParams {
  uint32_t nrarfcn{0};  ///< NR-ARFCN
  uint16_t pci{0};      ///< NR Physical Cell ID (0-1007)
  uint8_t dl_bw{0};
  uint8_t ul_bw{0};
  int8_t q_rx_lev_min{0};  ///< Minimum RSRP for cell selection (dBm * 2)
  int8_t q_qual_min{0};    ///< Minimum RSRQ for cell selection
  uint8_t q_hyst{0};
  uint16_t ranac{0};  ///< RAN Area Code (NR specific)

  auto operator<=>(const NrRadioParams&) const = default;
};

/// @brief WCDMA/UMTS radio parameters.
struct WcdmaRadioParams {
  uint32_t uarfcn{0};  ///< UTRA Absolute Radio Freq Channel Number
  uint16_t psc{0};     ///< Primary Scrambling Code
  int8_t q_rx_lev_min_rscp{0};
  int8_t q_qual_min_ecno{0};

  auto operator<=>(const WcdmaRadioParams&) const = default;
};

/// @brief GSM radio parameters.
struct GsmRadioParams {
  uint32_t arfcn{0};  ///< Absolute Radio Freq Channel Number
  uint16_t bsic{0};   ///< Base Station Identity Code
  int8_t rxlev_access_min{0};
  uint8_t cell_reselect_hysteresis{0};

  auto operator<=>(const GsmRadioParams&) const = default;
};

// ============================================================================
// Neighbor cell structures (from SIB4/SIB5/SIB6/SIB7)
// ============================================================================

/// @brief LTE intra-frequency neighbor from SIB4.
struct IntraFreqNeighbor {
  uint16_t pci{0};     ///< Neighbor Physical Cell ID
  int8_t q_offset{0};  ///< Individual cell offset (dB)
  auto operator<=>(const IntraFreqNeighbor&) const = default;
};

/// @brief LTE inter-frequency carrier from SIB5.
struct InterFreqCarrier {
  uint32_t earfcn{0};        ///< DL carrier frequency
  int8_t q_rx_lev_min{0};    ///< Minimum RSRP on this frequency
  uint8_t thresh_x_high{0};  ///< Upper reselection threshold
  uint8_t thresh_x_low{0};   ///< Lower reselection threshold
  uint8_t cell_resel_prio{0};
  uint8_t allowed_meas_bw{0};  ///< Allowed measurement bandwidth (RB)
  auto operator<=>(const InterFreqCarrier&) const = default;
};

/// @brief UTRA neighbor from SIB6 (for inter-RAT reselection LTE -> WCDMA).
struct UtraNeighborFreq {
  uint16_t uarfcn{0};
  int8_t q_rx_lev_min{0};
  int8_t p_max_utra{0};
  int8_t q_qual_min{0};
  uint8_t thresh_x_high{0};
  uint8_t thresh_x_low{0};
  auto operator<=>(const UtraNeighborFreq&) const = default;
};

/// @brief GERAN neighbor from SIB7 (for inter-RAT reselection LTE -> GSM).
struct GeranNeighborFreq {
  uint16_t arfcn_start{0};
  uint8_t ncc_permitted{0xFF};
  uint8_t q_rx_lev_min{0};
  uint8_t thresh_x_high{0};
  uint8_t thresh_x_low{0};
  auto operator<=>(const GeranNeighborFreq&) const = default;
};

/// @brief Neighbor cell measurement result from MeasurementReport.
struct NeighborMeasResult {
  uint16_t pci{0};
  uint8_t rsrp{0};  ///< Raw RSRP index (0-97), actual = index - 140 dBm
  uint8_t rsrq{0};  ///< Raw RSRQ index (0-34), actual = (index - 40) * 0.5 dB
  auto operator<=>(const NeighborMeasResult&) const = default;
};

// ============================================================================
// CellRadio — variant container for RAT-specific radio + neighbor lists
// ============================================================================

struct CellRadio {
  std::variant<std::monostate, GsmRadioParams, WcdmaRadioParams, LteRadioParams, NrRadioParams>
      radio_data;

  // Neighbor info from SIBs
  std::vector<IntraFreqNeighbor> intra_freq_neighbors;  ///< SIB4
  std::vector<InterFreqCarrier> inter_freq_carriers;    ///< SIB5
  std::vector<UtraNeighborFreq> utra_neighbors;         ///< SIB6
  std::vector<GeranNeighborFreq> geran_neighbors;       ///< SIB7

  // Live neighbor measurements from MeasurementReport (UL-DCCH)
  std::vector<NeighborMeasResult> meas_neighbors;

  template <typename T>
  [[nodiscard]] auto& get(this auto&& self) {
    return std::get<T>(std::forward<decltype(self)>(self).radio_data);
  }

  template <typename T>
  [[nodiscard]] auto* get_if(this auto&& self) noexcept {
    return std::get_if<T>(&std::forward<decltype(self)>(self).radio_data);
  }

  template <typename T>
  [[nodiscard]] bool is() const noexcept {
    return std::holds_alternative<T>(radio_data);
  }

  [[nodiscard]] uint32_t freq() const noexcept {
    return std::visit(
        [](const auto& arg) -> uint32_t {
          if constexpr (requires { arg.earfcn; })
            return arg.earfcn;
          else if constexpr (requires { arg.nrarfcn; })
            return arg.nrarfcn;
          else if constexpr (requires { arg.uarfcn; })
            return arg.uarfcn;
          else if constexpr (requires { arg.arfcn; })
            return arg.arfcn;
          else
            return 0;
        },
        radio_data);
  }

  [[nodiscard]] uint16_t pci_bsic() const noexcept {
    return std::visit(
        [](const auto& arg) -> uint16_t {
          if constexpr (requires { arg.pci; })
            return arg.pci;
          else if constexpr (requires { arg.psc; })
            return arg.psc;
          else if constexpr (requires { arg.bsic; })
            return arg.bsic;
          else
            return 0;
        },
        radio_data);
  }

  auto operator<=>(const CellRadio&) const = default;
};

// ============================================================================
// Signal quality measurements
// ============================================================================

struct GsmSignalParams {
  int8_t rxlev{0};    ///< Received signal level (dBm + 110)
  uint8_t rxqual{0};  ///< BER quality indicator (0-7)
  auto operator<=>(const GsmSignalParams&) const = default;
};

struct WcdmaSignalParams {
  float rscp{0.0f};  ///< Received Signal Code Power (dBm)
  float ecio{0.0f};  ///< Ec/Io ratio (dB)
  auto operator<=>(const WcdmaSignalParams&) const = default;
};

struct LteSignalParams {
  float rsrp{0.0f};  ///< Reference Signal Received Power (dBm)
  float rsrq{0.0f};  ///< Reference Signal Received Quality (dB)
  float sinr{0.0f};  ///< Signal to Interference + Noise Ratio (dB)
  float rssi{0.0f};  ///< Received Signal Strength Indicator (dBm)
  auto operator<=>(const LteSignalParams&) const = default;
};

struct NrSignalParams {
  float ss_rsrp{0.0f};  ///< SS Reference Signal Received Power (dBm)
  float ss_rsrq{0.0f};  ///< SS Reference Signal Received Quality (dB)
  float ss_sinr{0.0f};  ///< SS Signal to Interference + Noise Ratio (dB)
  auto operator<=>(const NrSignalParams&) const = default;
};

struct CellSignal {
  std::variant<std::monostate, GsmSignalParams, WcdmaSignalParams, LteSignalParams, NrSignalParams>
      signal_data;

  template <typename T>
  [[nodiscard]] auto& get(this auto&& self) {
    return std::get<T>(std::forward<decltype(self)>(self).signal_data);
  }

  template <typename T>
  [[nodiscard]] auto* get_if(this auto&& self) noexcept {
    return std::get_if<T>(&std::forward<decltype(self)>(self).signal_data);
  }

  [[nodiscard]] float main_level() const noexcept {
    return std::visit(
        [](const auto& arg) -> float {
          if constexpr (requires { arg.rsrp; })
            return arg.rsrp;
          else if constexpr (requires { arg.ss_rsrp; })
            return arg.ss_rsrp;
          else if constexpr (requires { arg.rscp; })
            return arg.rscp;
          else if constexpr (requires { arg.rxlev; })
            return static_cast<float>(arg.rxlev);
          else
            return 0.0f;
        },
        signal_data);
  }

  auto operator<=>(const CellSignal&) const = default;
};

// ============================================================================
// CellIdentity — top-level aggregate
// ============================================================================

/// @brief Complete cell representation combining identity, radio params, and signal quality.
///
/// One CellIdentity per unique (freq, PCI) pair in the CellTracker registry.
/// Fields are progressively filled as different packet types arrive:
///   1. ML1 packets (0xB193) fill signal (RSRP/RSRQ) and serving status
///   2. RRC OTA SIB1 (0xB0C0) fills passport (MCC/MNC/TAC/CID)
///   3. RRC OTA SIB2-7 fill radio params (bandwidth, neighbors, reselection)
///   4. MeasurementReport fills meas_neighbors
class CellIdentity {
public:
  RatType rat{RatType::UNKNOWN};
  bool is_serving{false};
  CellPassport passport{};
  CellRadio radio{};
  CellSignal signal;

  CellIdentity() noexcept = default;
  CellIdentity(RatType r, bool serving, CellPassport p, CellRadio rad, CellSignal s = {}) noexcept
      : rat(r)
      , is_serving(serving)
      , passport(std::move(p))
      , radio(std::move(rad))
      , signal(std::move(s)) {}

  template <typename T>
  [[nodiscard]] auto& radio_as(this auto&& self) {
    return std::forward<decltype(self)>(self).radio.template get<T>();
  }

  template <typename T>
  [[nodiscard]] auto* radio_as_if(this auto&& self) noexcept {
    return std::forward<decltype(self)>(self).radio.template get_if<T>();
  }

  template <typename T>
  [[nodiscard]] bool is_rat() const noexcept {
    return radio.is<T>();
  }

  template <typename T>
  [[nodiscard]] auto& signal_as(this auto&& self) {
    return std::forward<decltype(self)>(self).signal.template get<T>();
  }

  template <typename T>
  [[nodiscard]] auto* signal_as_if(this auto&& self) noexcept {
    return std::forward<decltype(self)>(self).signal.template get_if<T>();
  }

  auto operator<=>(const CellIdentity&) const = default;
};

// ============================================================================
// RatTraits — compile-time type mapping
// ============================================================================

template <RatType R>
struct RatTraits;

template <>
struct RatTraits<RatType::GSM> {
  using radio_type = GsmRadioParams;
  using signal_type = GsmSignalParams;
};
template <>
struct RatTraits<RatType::WCDMA> {
  using radio_type = WcdmaRadioParams;
  using signal_type = WcdmaSignalParams;
};
template <>
struct RatTraits<RatType::LTE> {
  using radio_type = LteRadioParams;
  using signal_type = LteSignalParams;
};
template <>
struct RatTraits<RatType::NR> {
  using radio_type = NrRadioParams;
  using signal_type = NrSignalParams;
};

template <RatType R>
using RatRadio_t = typename RatTraits<R>::radio_type;
template <RatType R>
using RatSignal_t = typename RatTraits<R>::signal_type;

}  // namespace QCom
