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
  uint32_t tac{0};      ///< TAC (LTE/NR) or LAC (GSM/WCDMA) — same semantic role
  uint64_t cell_id{0};  ///< Cell Identity (16-bit GSM CID / 28-bit ECI / 36-bit NCI)
  uint16_t mcc{0};      ///< Mobile Country Code (e.g. 250 = Russia)
  uint16_t mnc{0};      ///< Mobile Network Code (e.g. 01 = MTS)
  uint16_t rac{0};      ///< Routing Area Code (GPRS/UMTS NAS; 0 = unset)

  /// Alias for GSM/WCDMA compatibility
  [[nodiscard]] constexpr uint32_t lac() const noexcept { return tac; }

  bool cell_barred{false};  ///< SIB1: cell is barred for access
  int8_t q_rx_lev_min{0};   ///< SIB1: minimum required RSRP (dBm * 2)
  uint8_t q_rx_lev_min_offset{0};
  bool intra_freq_reselection_allowed{true};  ///< SIB1: intra-freq cell reselection
  bool cell_reserved_for_operator{false};     ///< SIB1 per-PLMN cellReservedForOperatorUse

  bool csg_ind{false};  ///< SIB1: Closed Subscriber Group indicator
  uint32_t csg_id{0};   ///< SIB1: CSG ID (27-bit) — private cell group

  uint8_t freq_band_ind{0};  ///< SIB1: frequency band indicator (e.g. 7 = 2600 MHz)
  uint8_t mnc_digits{0};     ///< 2 or 3 when known (B0C2/B823); 0 = infer
  /// PLMN copied from another PCI on the same EARFCN (fan-out) — not a hard SIB1/B0C2 bind.
  /// GUI must not treat soft-PLMN RADIO rows as operator-confirmed identity.
  bool plmn_soft{false};

  /// LTE: eNodeB ID / local cell from 28-bit ECI (common 20+8 split).
  [[nodiscard]] constexpr uint32_t enb_id() const noexcept {
    return static_cast<uint32_t>((cell_id >> 8) & 0xFFFFFu);
  }
  [[nodiscard]] constexpr uint8_t local_cell_id() const noexcept {
    return static_cast<uint8_t>(cell_id & 0xFFu);
  }
  /// UMTS: RNC-ID / C-ID from 28-bit UTRAN Cell Identity.
  [[nodiscard]] constexpr uint16_t rnc_id() const noexcept {
    return static_cast<uint16_t>((cell_id >> 16) & 0x0FFFu);
  }
  [[nodiscard]] constexpr uint16_t umts_cid16() const noexcept {
    return static_cast<uint16_t>(cell_id & 0xFFFFu);
  }

  [[nodiscard]] constexpr bool has_identity() const noexcept { return cell_id > 0; }
  bool operator==(const CellPassport&) const = default;
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
  uint16_t pci{0};     ///< Physical Cell ID (0-503) — SDR "dl_code"

  // MIB
  uint8_t dl_bw{0};           ///< Downlink bandwidth in MHz (1,3,5,10,15,20) — SDR "bandwidth"
  uint8_t phich_duration{0};  ///< PHICH duration: 0=normal, 1=extended
  uint8_t phich_resource{0};  ///< PHICH resource: 0=1/6, 1=1/2, 2=1, 3=2
  uint16_t sfn{0};            ///< System Frame Number (0-1023)
  uint8_t subframe{0};        ///< Subframe 0-9 (ML1 B193); 0 also means unset with sfn==0
  bool has_sfn_sf{false};     ///< SFN/subframe from ML1 meas (not MIB)

  // SIB1
  uint8_t freq_band_ind{0};  ///< Frequency band indicator
  bool p_max_present{false}; ///< SIB1 p-Max present
  int8_t p_max{0};           ///< SIB1 UE max TX power (dBm), valid if p_max_present

  // SIB2
  uint8_t ul_bw{0};                  ///< Uplink bandwidth in MHz (same units as dl_bw)
  uint32_t ul_earfcn{0};             ///< UL EARFCN (SDR "ul_freq" channel) — 0 = same as DL
  bool ac_barr_emergency{false};     ///< Access barring for emergency
  bool ac_barr_mo_signaling{false};  ///< Access barring for MO signaling
  bool ac_barr_mo_data{false};       ///< Access barring for MO data

  // SIB3
  uint8_t q_hyst{0};              ///< Cell reselection hysteresis (dB)
  uint8_t t_resel_eutra{0};       ///< Cell reselection timer (seconds)
  int8_t s_intra_search{0};       ///< Threshold for intra-freq meas (dB)
  int8_t s_non_intra_search{0};   ///< Threshold for inter-freq meas (dB)
  uint8_t thresh_serving_low{0};  ///< Threshold for serving cell (dB)
  uint8_t cell_resel_prio{0};     ///< Serving / inter-freq reselection priority
  uint8_t thresh_x_high{0};       ///< Inter-freq high threshold (QMI / SIB5)
  uint8_t thresh_x_low{0};        ///< Inter-freq low threshold (QMI / SIB5)
  uint32_t timing_advance{0};  ///< LTE TA index (B114/CMGRMI/QMI); 0 = unset; ≈78.125 m/step
  int8_t nas_idle{-1};            ///< QMI NAS idle flag: -1 unset, 0/1 value
  int8_t allowed_access{-1};      ///< B0C2: -1 unset, 1=Full, 0=Limited (wire 0→Full)
  int16_t emm_state{-1};          ///< B0EE EMM state (-1 unset); see Wire::B0ee::EmmState
  int16_t emm_substate{-1};       ///< B0EE substate (-1 unset); 0=NORMAL_SERVICE when REGISTERED
  uint16_t emm_mcc{0};            ///< B0EE registered PLMN MCC (NAS, not cell SIB)
  uint16_t emm_mnc{0};
  uint8_t emm_mnc_digits{0};      ///< 2 or 3
  uint16_t mme_group_id{0};       ///< B0EE GUTI MME Group Id (BE); 0=unset / no GUTI
  uint8_t mme_code{0};            ///< B0EE GUTI MME Code; 0 may be valid — pair with mme_group_id
  bool mme_present{false};        ///< GUTI MME fields valid (M-TMSI never stored here)
  int16_t rrc_state{-1};          ///< Evt1606 LTE RRC state (-1 unset); see Wire::Evt1606::State
  uint8_t serving_cell_index{0};  ///< B193: 0=PCell, 1+=SCell
  uint8_t valid_rx{0};            ///< B193 Valid Rx bitmask (1=RX0, 3=RX0_RX1); 0=unset
  bool is_restricted{false};      ///< B193 Is Restricted

  bool operator==(const LteRadioParams&) const = default;
};

/// @brief NR radio parameters from SIB1/SIB2/SIB3 / DIAG B822/B823.
struct NrRadioParams {
  uint32_t nrarfcn{0};  ///< NR-ARFCN
  uint32_t ul_nrarfcn{0};
  uint16_t pci{0};      ///< NR Physical Cell ID (0-1007)
  uint8_t dl_bw{0};
  uint8_t ul_bw{0};
  uint16_t band{0};
  uint16_t sfn{0};
  uint8_t scs_khz{0};   ///< 15/30/60/120 from MIB
  int8_t q_rx_lev_min{0};  ///< Minimum RSRP for cell selection (dBm * 2)
  int8_t q_qual_min{0};    ///< Minimum RSRQ for cell selection
  uint8_t q_hyst{0};
  uint16_t ranac{0};  ///< RAN Area Code (NR specific)
  int8_t allowed_access{-1};

  bool operator==(const NrRadioParams&) const = default;
};

/// @brief WCDMA/UMTS radio parameters from 0x4027 / 0x4127 / 0x4005.
struct WcdmaRadioParams {
  uint32_t dl_uarfcn{0};  ///< DL UARFCN — SDR "dl_freq" (channel)
  uint32_t ul_uarfcn{0};  ///< UL UARFCN — SDR "ul_freq"
  uint16_t psc{0};        ///< Primary Scrambling Code (0-511) — SDR "dl_code" / "psc"
  uint16_t ura_id{0};     ///< UTRAN Registration Area (0x4027)
  uint8_t access{0};      ///< Access class / restriction nibble from Cell ID pkt
  uint8_t flags{0};       ///< Qualcomm Cell ID flags byte
  int8_t q_rx_lev_min_rscp{0};
  int8_t q_qual_min_ecno{0};
  /// Last seen RRC OTA channel type from 0x412F (0xFF = unset).
  uint8_t last_rrc_channel{0xFF};
  uint16_t last_rrc_len{0};

  bool operator==(const WcdmaRadioParams&) const = default;
};

/// @brief GSM radio parameters from SI-3/SI-4 and Cell Info.
struct GsmRadioParams {
  uint32_t arfcn{0};  ///< Absolute Radio Freq Channel Number
  uint16_t bsic{0};   ///< Base Station Identity Code (NCC<<3 | BCC)
  uint8_t ncc{0};     ///< Network Colour Code (3 bits)
  uint8_t bcc{0};     ///< Base Station Colour Code (3 bits)
  uint8_t band_class{0xFF};  ///< Qualcomm arfcn_band>>12 nibble (0xFF = unknown)

  // SI-3 Cell Selection Parameters (TS 44.018 §10.5.2.4)
  uint8_t rxlev_access_min{0};  ///< Minimum RxLev for cell access (0-63)
  uint8_t ms_txpwr_max_cch{0};  ///< Max TX power for RACH (power class)
  uint8_t ncc_permitted{0xFF};  ///< Bitmask of allowed NCC values

  // SI-3 Rest Octets — Cell Reselection Parameters
  uint8_t cell_reselect_offset{0};  ///< CRO in 2 dB steps (0-63)
  uint8_t temporary_offset{0};      ///< 10 dB steps, 7 = infinity
  uint8_t penalty_time{0};          ///< 0-31, 31 = infinity
  bool reselect_params_present{false};
  uint32_t timing_advance{0};  ///< QMI GERAN TA when known (0 = unset)

  bool operator==(const GsmRadioParams&) const = default;
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
  /// Optional PCIs from SIB5 interFreqNeighCellList (RADIO rows, no invented RSRP).
  std::vector<uint16_t> neigh_pcis;
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

/// @brief Neighbor cell measurement result from MeasurementReport / ML1.
struct NeighborMeasResult {
  uint16_t pci{0};
  float rsrp_dbm{0};  ///< Inst Measured RSRP (dBm)
  float rsrp_filt{0}; ///< Filtered RSRP when available (B193)
  float rsrq_db{0};   ///< Inst RSRQ (best of Rx) in dB
  float rsrq_filt{0}; ///< Filtered RSRQ when available (B193)
  float sinr_db{0};   ///< Best FTL SNR / SS-SINR in dB
  float sinr_rx0{0};  ///< Per-Rx FTL SNR
  float sinr_rx1{0};
  float rssi_dbm{0};  ///< Inst RSSI in dBm (LTE ML1 B193)
  bool has_rsrp{false};
  bool has_rsrp_filt{false};
  bool has_rsrq{false};
  bool has_rsrq_filt{false};
  bool has_sinr{false};
  bool has_sinr_per_rx{false};
  bool has_rssi{false};
  /// MeasReport cgi-Info (PLMN+TAC+ECI) when reportCGI is active.
  bool has_cgi{false};
  CellPassport cgi{};
  bool operator==(const NeighborMeasResult&) const = default;
};

/// @brief GSM neighbor from BA list / surround measurements.
struct GsmNeighborCell {
  uint16_t arfcn{0};
  uint8_t bsic{0};
  bool bsic_valid{false};
  int16_t rxlev{0};  ///< RxLev in dBm (0.0625 resolution from raw)
  auto operator<=>(const GsmNeighborCell&) const = default;
};

/// @brief WCDMA neighbor from 0x4005 reselection rank or 0x4111 monitored set.
struct WcdmaNeighborCell {
  uint16_t uarfcn{0};
  uint16_t psc{0};
  int16_t rscp{0};  ///< RSCP in dBm
  int16_t ecio{0};  ///< Ec/Io in 0.5 dB steps
  auto operator<=>(const WcdmaNeighborCell&) const = default;
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

  // GSM/WCDMA neighbor cells (from proprietary Qualcomm binary logs)
  std::vector<GsmNeighborCell> gsm_neighbors;
  std::vector<WcdmaNeighborCell> wcdma_neighbors;

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
          else if constexpr (requires { arg.dl_uarfcn; })
            return arg.dl_uarfcn;
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
};

// ============================================================================
// Signal quality measurements
// ============================================================================

struct GsmSignalParams {
  int8_t rxlev{0};     ///< Received signal level (dBm) — SDR "rxl"
  uint8_t rxqual{0};   ///< BER quality indicator (0-7)
  int16_t snr{0};      ///< SNR estimate from L1 burst (raw units; 0 = unset)
  bool has_snr{false};
  int16_t c1{0};       ///< Cell selection criterion C1 (TS 45.008)
  int16_t c2{0};       ///< Cell reselection criterion C2
  bool has_c1c2{false};
  bool operator==(const GsmSignalParams&) const = default;
};

struct WcdmaSignalParams {
  float rscp{0.0f};  ///< Received Signal Code Power (dBm) — SDR "rxl"
  float ecio{0.0f};  ///< Ec/Io ratio (dB) — SDR "snr" proxy for UMTS
  bool has_ecio{false};
};

struct LteSignalParams {
  float rsrp{0.0f};       ///< Inst Measured RSRP (dBm) — SDR "rxl"
  float rsrp_filt{0.0f};  ///< Filtered RSRP (B193); prefer for stable ranking when set
  float rsrq{0.0f};       ///< Inst RSRQ best-of-Rx (dB)
  float rsrq_filt{0.0f};  ///< Filtered RSRQ (B193)
  float sinr{0.0f};       ///< Best FTL SNR (dB) — SDR "snr"
  float sinr_rx0{0.0f};   ///< FTL SNR Rx0
  float sinr_rx1{0.0f};   ///< FTL SNR Rx1
  float rssi{0.0f};       ///< Inst RSSI (dBm)
  bool has_rsrp_filt{false};
  bool has_rsrq_filt{false};
  bool has_sinr{false};
  bool has_sinr_per_rx{false};
  bool has_rssi{false};
};

struct NrSignalParams {
  float ss_rsrp{0.0f};  ///< SS Reference Signal Received Power (dBm)
  float ss_rsrq{0.0f};  ///< SS Reference Signal Received Quality (dB)
  float ss_sinr{0.0f};  ///< SS Signal to Interference + Noise Ratio (dB)
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
  /// Sticky: once this cell was serving in the session, keep for export (Vlad parity).
  bool ever_serving{false};
  CellPassport passport{};
  CellRadio radio{};
  CellSignal signal;

  /// App-level bookkeeping (Vlad CSV parity) — not modem SI fields.
  uint64_t seen{0};
  std::string first_seen;
  std::string last_seen;

  CellIdentity() noexcept = default;
  CellIdentity(RatType r, bool serving, CellPassport p, CellRadio rad, CellSignal s = {}) noexcept
      : rat(r)
      , is_serving(serving)
      , ever_serving(serving)
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
