/// @file LteQcomLayouts.h
/// @brief Single source of truth for LTE Qualcomm proprietary binary layouts.
///
/// Offsets / strides / RevWordBits ranges live here. Parsers in LteQcomBinary.cpp
/// only orchestrate validate → domain structs → Events. Verified against
/// scat/QCSuper and SIM8300 live dumps; unknown versions stay fail-closed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "core/BinaryCursor.h"
#include "core/RevWordBits.h"
#include <observer/model/Utils.h>

namespace QCom::Lte::Wire {

using Utils::BinaryCursor;
using Utils::BitRange;
using Utils::RevWordBits;

// ============================================================================
// 0xB0C2 — Serving Cell Info
// ============================================================================

namespace B0c2 {

struct Decoded {
  uint16_t pci{0};
  uint32_t earfcn{0};
  uint32_t ul_earfcn{0};
  uint8_t dl_bw_raw{0};   ///< NRB (6/15/25/50/75/100) or already-MHz (1..20)
  uint8_t ul_bw_raw{0};
  uint32_t cell_id{0};
  uint16_t tac{0};
  uint32_t band{0};
  uint16_t mcc{0};
  uint8_t mnc_digit{0};
  uint16_t mnc{0};
  /// Wire enum from QXDM "Allowed Access": 0 = Full, nonzero = Limited.
  uint8_t allowed_raw{0};
};

/// Map B0C2/B0C1 BW byte → MHz. Live QXDM uses NRB (6/15/25/50/75/100);
/// unknown values pass through as already-MHz (note: wire 15 is NRB→3, not 15 MHz).
[[nodiscard]] inline uint8_t bw_raw_to_mhz(uint8_t v) noexcept {
  switch (v) {
    case 6: return 1;
    case 15: return 3;
    case 25: return 5;
    case 50: return 10;
    case 75: return 15;
    case 100: return 20;
    default: return v;
  }
}

/// Domain bool for LteRadioParams.allowed_access: 1 = Full, 0 = Limited.
/// QXDM wire 0 = Full (SIM8300 live dumps).
[[nodiscard]] inline int8_t allowed_access_bool(uint8_t wire) noexcept {
  return wire == 0 ? int8_t{1} : int8_t{0};
}

/// v2: EARFCN is u16. Body after version byte.
struct V2 {
  static constexpr uint8_t kVersion = 2;
  static constexpr size_t kMinBody = 24;
  static constexpr size_t pci = 0;         // u16
  static constexpr size_t earfcn = 2;      // u16
  static constexpr size_t ul_earfcn = 4;   // u16
  static constexpr size_t dl_bw = 6;       // u8
  static constexpr size_t ul_bw = 7;       // u8
  static constexpr size_t cell_id = 8;     // u32
  static constexpr size_t tac = 12;        // u16
  static constexpr size_t band = 14;       // u32
  static constexpr size_t mcc = 18;        // u16
  static constexpr size_t mnc_digit = 20;  // u8
  static constexpr size_t mnc = 21;        // u16
  static constexpr size_t allowed = 23;    // u8
};

/// v3: EARFCN is u32.
struct V3 {
  static constexpr uint8_t kVersion = 3;
  static constexpr size_t kMinBody = 28;
  static constexpr size_t pci = 0;         // u16
  static constexpr size_t earfcn = 2;      // u32
  static constexpr size_t ul_earfcn = 6;   // u32
  static constexpr size_t dl_bw = 10;      // u8
  static constexpr size_t ul_bw = 11;      // u8
  static constexpr size_t cell_id = 12;    // u32
  static constexpr size_t tac = 16;        // u16
  static constexpr size_t band = 18;       // u32
  static constexpr size_t mcc = 22;        // u16
  static constexpr size_t mnc_digit = 24;  // u8
  static constexpr size_t mnc = 25;        // u16
  static constexpr size_t allowed = 27;    // u8
};

[[nodiscard]] inline std::optional<Decoded> decode_v2(BinaryCursor body) {
  if (!body.has(0, V2::kMinBody)) return std::nullopt;
  Decoded d;
  d.pci = body.le16(V2::pci);
  d.earfcn = body.le16(V2::earfcn);
  d.ul_earfcn = body.le16(V2::ul_earfcn);
  d.dl_bw_raw = body.u8(V2::dl_bw);
  d.ul_bw_raw = body.u8(V2::ul_bw);
  d.cell_id = body.le32(V2::cell_id);
  d.tac = body.le16(V2::tac);
  d.band = body.le32(V2::band);
  d.mcc = body.le16(V2::mcc);
  d.mnc_digit = body.u8(V2::mnc_digit);
  d.mnc = body.le16(V2::mnc);
  d.allowed_raw = body.u8(V2::allowed);
  return d;
}

[[nodiscard]] inline std::optional<Decoded> decode_v3(BinaryCursor body) {
  if (!body.has(0, V3::kMinBody)) return std::nullopt;
  Decoded d;
  d.pci = body.le16(V3::pci);
  d.earfcn = body.le32(V3::earfcn);
  d.ul_earfcn = body.le32(V3::ul_earfcn);
  d.dl_bw_raw = body.u8(V3::dl_bw);
  d.ul_bw_raw = body.u8(V3::ul_bw);
  d.cell_id = body.le32(V3::cell_id);
  d.tac = body.le16(V3::tac);
  d.band = body.le32(V3::band);
  d.mcc = body.le16(V3::mcc);
  d.mnc_digit = body.u8(V3::mnc_digit);
  d.mnc = body.le16(V3::mnc);
  d.allowed_raw = body.u8(V3::allowed);
  return d;
}

/// Full DIAG payload: version byte + body.
[[nodiscard]] inline std::optional<Decoded> decode(BinaryCursor pkt) {
  if (!pkt.has(0, 1)) return std::nullopt;
  const uint8_t version = pkt.u8(0);
  const BinaryCursor body = pkt.at(1);
  if (version == V2::kVersion) return decode_v2(body);
  if (version == V3::kVersion) return decode_v3(body);
  return std::nullopt;
}

}  // namespace B0c2

// ============================================================================
// 0xB0EE — LTE NAS EMM State (UE registration status)
// ============================================================================
// QXDM: "LTE NAS EMM State". No EARFCN/PCI — bind to serving like B114.
// Oracle SIM8300 v2: REGISTERED / DEREGISTERED_INITIATED (same GUTI body).

namespace B0ee {

/// Qualcomm EMM main state (wire @+0 after version).
enum class EmmState : uint8_t {
  Null = 0,
  Deregistered = 1,
  RegisteredInitiated = 2,
  Registered = 3,
  TauInitiated = 4,
  ServiceRequestInitiated = 5,
  DeregisteredInitiated = 6,
};

struct Plmn {
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint8_t mnc_digits{0};  ///< 2 or 3
};

struct Decoded {
  uint8_t emm_state{0};
  uint8_t emm_substate{0};
  Plmn plmn{};
  bool guti_valid{false};
  uint8_t guti_ue_id{0};
  Plmn guti_plmn{};
  uint16_t mme_group_id{0};  ///< big-endian word (QXDM byte pair {hi,lo} on wire)
  uint8_t mme_code{0};
  uint32_t m_tmsi{0};  ///< LE on wire; PII — decode for tests, do not export by default
};

struct V2 {
  static constexpr uint8_t kVersion = 2;
  static constexpr size_t kMinBody = 18;
  static constexpr size_t emm_state = 0;
  static constexpr size_t emm_substate = 1;
  static constexpr size_t reserved = 2;
  static constexpr size_t plmn = 3;         // 3 BCD
  static constexpr size_t guti_valid = 6;
  static constexpr size_t guti_ue_id = 7;
  static constexpr size_t guti_plmn = 8;    // 3 BCD
  static constexpr size_t mme_group = 11;   // 2 bytes as QXDM {b0,b1}
  static constexpr size_t mme_code = 13;
  static constexpr size_t m_tmsi = 14;      // u32 LE
};

/// 3GPP 24.008 PLMN (same packing as NAS / B0C4).
[[nodiscard]] inline std::optional<Plmn> decode_plmn_24_008(BinaryCursor b, size_t off) noexcept {
  if (!b.has(off, 3)) return std::nullopt;
  const uint8_t d1 = b.u8(off) & 0x0F, d2 = (b.u8(off) >> 4) & 0x0F, d3 = b.u8(off + 1) & 0x0F;
  const uint8_t m3 = (b.u8(off + 1) >> 4) & 0x0F, m1 = b.u8(off + 2) & 0x0F,
                m2 = (b.u8(off + 2) >> 4) & 0x0F;
  if (d1 > 9 || d2 > 9 || d3 > 9) return std::nullopt;
  Plmn p;
  p.mcc = static_cast<uint16_t>(d1 * 100 + d2 * 10 + d3);
  if (m3 == 0x0F) {
    if (m1 > 9 || m2 > 9) return std::nullopt;
    p.mnc = static_cast<uint16_t>(m1 * 10 + m2);
    p.mnc_digits = 2;
  } else {
    if (m1 > 9 || m2 > 9 || m3 > 9) return std::nullopt;
    p.mnc = static_cast<uint16_t>(m1 * 100 + m2 * 10 + m3);
    p.mnc_digits = 3;
  }
  if (p.mcc < 100 || p.mcc > 999) return std::nullopt;
  return p;
}

[[nodiscard]] inline const char* emm_state_name(uint8_t s) noexcept {
  switch (s) {
    case 0: return "NULL";
    case 1: return "DEREGISTERED";
    case 2: return "REGISTERED_INITIATED";
    case 3: return "REGISTERED";
    case 4: return "TAU_INITIATED";
    case 5: return "SERVICE_REQUEST_INITIATED";
    case 6: return "DEREGISTERED_INITIATED";
    default: return "UNKNOWN";
  }
}

[[nodiscard]] inline std::optional<Decoded> decode_v2(BinaryCursor body) {
  if (!body.has(0, V2::kMinBody)) return std::nullopt;
  Decoded d;
  d.emm_state = body.u8(V2::emm_state);
  d.emm_substate = body.u8(V2::emm_substate);
  auto plmn = decode_plmn_24_008(body, V2::plmn);
  if (!plmn) return std::nullopt;
  d.plmn = *plmn;
  d.guti_valid = body.u8(V2::guti_valid) != 0;
  d.guti_ue_id = body.u8(V2::guti_ue_id);
  if (d.guti_valid) {
    if (auto gp = decode_plmn_24_008(body, V2::guti_plmn)) d.guti_plmn = *gp;
    d.mme_group_id = static_cast<uint16_t>((body.u8(V2::mme_group) << 8) | body.u8(V2::mme_group + 1));
    d.mme_code = body.u8(V2::mme_code);
    d.m_tmsi = body.le32(V2::m_tmsi);
  }
  return d;
}

[[nodiscard]] inline std::optional<Decoded> decode(BinaryCursor pkt) {
  if (!pkt.has(0, 1)) return std::nullopt;
  if (pkt.u8(0) != V2::kVersion) return std::nullopt;
  return decode_v2(pkt.at(1));
}

}  // namespace B0ee

// ============================================================================
// DIAG event 1606 — EVENT_LTE_RRC_STATE_CHANGE
// ============================================================================
// Not a 0xB0xx log. QXDM Item View "RRC State = …". Payload = 1 byte state.
// eid wire example 0x2646 → id=1606, pl_ind=1. Oracle SIM8300; Suspend=5 assumed
// (QXDM 8-state set; no live dump yet).

namespace Evt1606 {

inline constexpr uint16_t kEventId = 1606;

enum class State : uint8_t {
  Inactive = 0,
  IdleNotCamped = 1,
  IdleCamped = 2,
  Connecting = 3,
  Connected = 4,
  Suspend = 5,           ///< assumed (not yet seen on SIM8300)
  IratToLteStarted = 6,  ///< QXDM: "IRAT To LTE Started"
  Closing = 7,
};

[[nodiscard]] inline const char* name(uint8_t s) noexcept {
  switch (s) {
    case 0: return "Inactive";
    case 1: return "Idle Not Camped";
    case 2: return "Idle Camped";
    case 3: return "Connecting";
    case 4: return "Connected";
    case 5: return "Suspend";
    case 6: return "IRAT To LTE Started";
    case 7: return "Closing";
    default: return "UNKNOWN";
  }
}

[[nodiscard]] inline bool known(uint8_t s) noexcept { return s <= 7; }

/// Camped on a cell in idle (good hop / SIB wait target).
[[nodiscard]] inline bool is_idle_camped(uint8_t s) noexcept {
  return s == static_cast<uint8_t>(State::IdleCamped);
}
[[nodiscard]] inline bool is_connected(uint8_t s) noexcept {
  return s == static_cast<uint8_t>(State::Connected);
}
[[nodiscard]] inline bool is_connecting_or_connected(uint8_t s) noexcept {
  return s == static_cast<uint8_t>(State::Connecting) ||
         s == static_cast<uint8_t>(State::Connected);
}
/// OOS / stack quiet — not camped on LTE.
[[nodiscard]] inline bool is_not_camped(uint8_t s) noexcept {
  return s == static_cast<uint8_t>(State::Inactive) ||
         s == static_cast<uint8_t>(State::IdleNotCamped);
}

}  // namespace Evt1606

// ============================================================================
// 0xB193 — ML1 Serving Cell Meas Response, subpacket id 0x19
// ============================================================================
// SIM8300 / SDX55 v48 oracle (QXDM ↔ live hex): cell metrics are native LE
// fields, NOT the older RevWordBits windows (those decode garbage on dual-Rx).
// v36 keeps the legacy RevWordBits path (no SIM8300 oracle yet).

namespace B193 {

inline constexpr uint8_t kSubpacketId = 0x19;

/// Body header (after subpacket id/ver/size).
inline constexpr size_t kBodyEarfcn = 0;     // u32
inline constexpr size_t kBodyNumCells = 4;   // u16
inline constexpr size_t kBodyValidRx = 6;    // u16: 1=RX0, 3=RX0_RX1, …

/// Legacy RevWordBits (v36 only).
inline constexpr size_t kMeasWordsOff = 16;
inline constexpr int kMeasWords = 12;
inline constexpr BitRange kRsrp{108, 120};
inline constexpr BitRange kRsrq{224, 234};
inline constexpr BitRange kRssi{320, 331};
inline constexpr std::pair<int, int> kSnrSlices[] = {{0, 9}, {9, 18}, {32, 41}, {42, 50}};

struct Sp19RevLayout {
  size_t cell_start{};
  size_t cell_stride{};
  size_t snr_off{};
  bool le_meas{false};  ///< true → Sp19LeCell (v48/50); false → RevWordBits (v36)
};

[[nodiscard]] constexpr std::optional<Sp19RevLayout> sp19_rev_layout(uint8_t sp_ver) noexcept {
  if (sp_ver == 36) {
    return Sp19RevLayout{.cell_start = 8, .cell_stride = 128, .snr_off = 80, .le_meas = false};
  }
  if (sp_ver == 48 || sp_ver == 50) {
    return Sp19RevLayout{.cell_start = 12, .cell_stride = 140, .snr_off = 92, .le_meas = true};
  }
  return std::nullopt;
}

/// SM8550 v59 — different header/cell shape.
struct Sp19V59 {
  static constexpr uint8_t kVersion = 59;
  static constexpr size_t kMinBody = 8;
  static constexpr size_t earfcn = 0;     // u32
  static constexpr size_t num_cells = 4;  // u32
  static constexpr size_t cell_start = 8;
  static constexpr size_t cell_stride = 148;
  static constexpr size_t pci = 8;         // u16 in cell
  static constexpr size_t rsrp_word = 44;  // u32; RSRP = (word >> 12) & 0xFFF
};

/// Decoded v48/50 cell row (stride 140). Offsets relative to cell start.
struct Sp19LeCell {
  static constexpr size_t kMinSize = 96;

  uint16_t pci{0};
  bool is_serving{false};
  uint8_t serving_cell_index{0};  ///< 0 = PCell
  bool is_restricted{false};
  uint16_t sfn{0};
  uint8_t subframe{0};

  float rsrp_inst{0};   ///< Inst Measured RSRP
  float rsrp_filt{0};   ///< Filtered RSRP
  float rsrp_rx0{0};
  float rsrp_rx1{0};
  float rsrq_inst{0};   ///< best of Rx0/Rx1
  float rsrq_rx0{0};
  float rsrq_rx1{0};
  float rsrq_filt{0};
  float rssi_inst{0};
  float snr_rx0{0};
  float snr_rx1{0};
  float snr_best{0};

  bool has_rsrp{false};
  bool has_rsrp_filt{false};
  bool has_rsrp_rx{false};
  bool has_rsrq{false};
  bool has_rsrq_filt{false};
  bool has_rssi{false};
  bool has_snr{false};
};

/// v48/50 LE meas SSOT (QXDM-validated on SIM8300).
///   u16@0:  PCI low9, serving_cell_index[9:11], is_serving bit12, restricted bit13
///   u16@4:  SFN low10, subframe[10:…]
///   u32@16 bit10..: Inst RSRP Rx0 (12)
///   u32@20 bit12..: Inst RSRP Rx1 (12)
///   u32@36: Inst Measured RSRP [0:12), Filtered RSRP [12:24)
///   u16@40/44 & 0x1FF: Inst RSRQ Rx0 / Rx1 (Inst = max)
///   u32@48 >> 20 & 0x1FF: Filtered RSRQ
///   u16@60: Inst RSSI
///   u32@92: FTL SNR Rx0 [0:9), Rx1 [9:18)
[[nodiscard]] inline std::optional<Sp19LeCell> decode_sp19_le_cell(BinaryCursor cell) noexcept {
  using Utils::ml1_ftl_snr;
  using Utils::ml1_rsrp;
  using Utils::ml1_rsrq;
  using Utils::ml1_rssi;
  using Utils::valid_lte_pci;
  using Utils::valid_lte_rsrp;

  if (!cell.has(0, Sp19LeCell::kMinSize)) return std::nullopt;

  Sp19LeCell out;
  const uint16_t val0 = cell.le16(0);
  out.pci = Utils::lte_pci_from_meas_word(val0);
  out.serving_cell_index = static_cast<uint8_t>((val0 >> 9) & 0x7u);
  out.is_serving = ((val0 >> 12) & 1u) != 0;
  out.is_restricted = ((val0 >> 13) & 1u) != 0;
  if (!valid_lte_pci(out.pci) || out.pci == 0) return std::nullopt;

  const uint16_t sfn_sf = cell.le16(4);
  out.sfn = static_cast<uint16_t>(sfn_sf & 0x3FFu);
  out.subframe = static_cast<uint8_t>(sfn_sf >> 10);

  out.rsrp_inst = ml1_rsrp(cell.le32_bits(36, 0, 12));
  out.rsrp_filt = ml1_rsrp(cell.le32_bits(36, 12, 12));
  out.has_rsrp = valid_lte_rsrp(out.rsrp_inst);
  out.has_rsrp_filt = valid_lte_rsrp(out.rsrp_filt);

  out.rsrp_rx0 = ml1_rsrp(cell.le32_bits(16, 10, 12));
  out.rsrp_rx1 = ml1_rsrp(cell.le32_bits(20, 12, 12));
  out.has_rsrp_rx = valid_lte_rsrp(out.rsrp_rx0) || valid_lte_rsrp(out.rsrp_rx1);

  out.rsrq_rx0 = ml1_rsrq(cell.le16(40) & 0x1FFu);
  out.rsrq_rx1 = ml1_rsrq(cell.le16(44) & 0x1FFu);
  out.rsrq_inst = (out.rsrq_rx0 > out.rsrq_rx1) ? out.rsrq_rx0 : out.rsrq_rx1;
  out.has_rsrq = (out.rsrq_inst > -30.0f && out.rsrq_inst <= -1.0f);

  out.rsrq_filt = ml1_rsrq(cell.le32_bits(48, 20, 9));
  out.has_rsrq_filt = (out.rsrq_filt > -30.0f && out.rsrq_filt <= -1.0f);

  out.rssi_inst = ml1_rssi(cell.le16(60));
  out.has_rssi = (out.rssi_inst > -120.0f && out.rssi_inst < -20.0f);

  if (cell.has(92, 4)) {
    out.snr_rx0 = ml1_ftl_snr(cell.le32_bits(92, 0, 9));
    out.snr_rx1 = ml1_ftl_snr(cell.le32_bits(92, 9, 9));
    out.snr_best = (out.snr_rx0 > out.snr_rx1) ? out.snr_rx0 : out.snr_rx1;
    out.has_snr = (out.snr_best > -20.0f && out.snr_best < 40.0f);
  }

  if (!out.has_rsrp) return std::nullopt;
  return out;
}

[[nodiscard]] inline float snr_best_of(const RevWordBits& snr_rb) noexcept {
  float best = -999.0f;
  for (auto [a, b] : kSnrSlices) {
    const float s = Utils::ml1_ftl_snr(snr_rb.slice(a, b));
    if (s > best) best = s;
  }
  return best;
}

[[nodiscard]] inline RevWordBits cell_meas_bits(BinaryCursor cell) {
  return RevWordBits(cell.data() + kMeasWordsOff, kMeasWords);
}

}  // namespace B193

// ============================================================================
// 0xB114 — LL1 Serving Cell Frame Timing (SIM8300 / SDX55, QXDM v161)
// ============================================================================
// Oracle: QXDM decode ↔ live hex. Survey needs only Starting UL Timing Advance.
//   u16 packed @+2: bit0 = carrier (0=PCC), bits1.. = TA index  →  ta = packed >> 1
// Confirmed: packed=4→TA=2, packed=2→TA=1. Records optional (SFN bitfield etc.).

namespace B114 {

inline constexpr uint8_t kVersion = 161;
inline constexpr size_t kHeaderSize = 16;
inline constexpr size_t kRecordStride = 48;
/// LTE TA index → approx one-way distance (m). Step ≈ 78.125 m.
inline constexpr double kMetersPerTa = 78.125;

struct Header {
  uint8_t version{0};
  uint8_t num_records{0};
  uint16_t packed{0};
  uint16_t timing_advance{0};  ///< LTE TA index (Starting UL Timing Advance)
  bool carrier_pcc{true};
  uint16_t dl_frame_timing_off_ts{0};
  uint16_t ul_frame_timing_off_ts{0};
  uint32_t dl_sf_mstmr{0};
  uint32_t ul_sf_mstmr{0};
};

/// SFN (0..1023) in bits 0..9, subframe (0..9) in bits 10.. — record word0.
[[nodiscard]] constexpr uint16_t record_sfn(uint16_t sfn_sf) noexcept {
  return static_cast<uint16_t>(sfn_sf & 0x3FFu);
}
[[nodiscard]] constexpr uint8_t record_subframe(uint16_t sfn_sf) noexcept {
  return static_cast<uint8_t>(sfn_sf >> 10);
}

[[nodiscard]] inline std::optional<Header> decode_header(BinaryCursor pkt) noexcept {
  if (!pkt.has(0, kHeaderSize)) return std::nullopt;
  Header h;
  h.version = pkt.u8(0);
  if (h.version != kVersion) return std::nullopt;
  h.num_records = pkt.u8(1);
  h.packed = pkt.le16(2);
  h.carrier_pcc = (h.packed & 1u) == 0;
  h.timing_advance = static_cast<uint16_t>(h.packed >> 1);
  // LTE TA index range (36.213); reject garbage.
  if (h.timing_advance > 1282) return std::nullopt;
  h.dl_frame_timing_off_ts = pkt.le16(4);
  h.ul_frame_timing_off_ts = pkt.le16(6);
  h.dl_sf_mstmr = pkt.le32(8);
  h.ul_sf_mstmr = pkt.le32(12);
  return h;
}

[[nodiscard]] constexpr double ta_meters(uint32_t ta_index) noexcept {
  return static_cast<double>(ta_index) * kMetersPerTa;
}

}  // namespace B114

}  // namespace QCom::Lte::Wire
