/// @file LteRrcOta.h
/// @brief Qualcomm 0xB0C0 LTE RRC OTA wrapper layouts (scat diagltelogparser).
///
/// Layouts and pdu_num→channel maps must match fgsect/scat parse_lte_rrc —
/// wrong header_size slices ASN.1 mid-sib_mask and silently kills SIB1 decode.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

#include "core/Types.h"
#include "core/Utils.h"

namespace QCom::Lte {

struct LteRrcOtaInfo {
  uint8_t version{0};
  uint16_t pci{0};
  uint32_t earfcn{0};
  uint8_t pdu_num{0};  ///< Qualcomm channel type (version-dependent numbering)
  uint16_t pdu_len{0}; ///< Declared PDU length (0 = unknown / use remainder)
  /// v30+ only (scat): 0 = not segmented, 1–6 = fragment, 7 = last fragment.
  uint8_t segment_id{0};
  std::span<const uint8_t> asn1;
};

namespace detail {

struct OtaLayout {
  bool valid{false};
  bool earfcn_u32{false};
  uint8_t off_pci{0};
  uint8_t off_earfcn{0};
  uint8_t off_pdu_type{0};
  uint8_t off_pdu_len{0};
  uint8_t header_size{0};
};

/// Byte offsets match scat `struct.unpack` on pkt_body (version at [0]).
inline OtaLayout layout_for_version(uint8_t v) noexcept {
  // v2–v4: <BB BHHH BH> → content @13
  auto v2 = []() { return OtaLayout{true, false, 4, 6, 10, 11, 13}; };
  // v5–v7: <BB BHHH BLH> (+ sib_mask) → content @17
  auto v5 = []() { return OtaLayout{true, false, 4, 6, 10, 15, 17}; };
  // v8–v24: <BB BHLH BLH> (EARFCN u32 + sib_mask) → content @19
  auto v8 = []() { return OtaLayout{true, true, 4, 6, 12, 17, 19}; };
  // v25–v29: <BBBB BHLH BLH> (+ nr_rel) → content @21
  auto v25 = []() { return OtaLayout{true, true, 6, 8, 14, 19, 21}; };
  // v30+: + segment fields → content @24
  auto v30 = []() { return OtaLayout{true, true, 6, 8, 14, 19, 24}; };

  if (v >= 30) return v30();
  if (v >= 25) return v25();  // 0x19..0x1D
  if (v >= 8) return v8();    // 0x08..0x18
  if (v >= 5) return v5();
  if (v >= 2) return v2();
  return {};
}

[[nodiscard]] inline bool looks_like_ota_header(std::span<const uint8_t> payload) noexcept {
  if (payload.empty()) return false;
  return layout_for_version(payload[0]).valid;
}

/// scat rrc_subtype_map: is this pdu BCCH-BCH (MIB)?
[[nodiscard]] inline bool is_mib_pdu(uint8_t version, uint8_t pdu_num) noexcept {
  if (version == 0x09 || version == 0x0C) return pdu_num == 8;
  return pdu_num == 1;
}

[[nodiscard]] inline bool is_plausible_lte_rrc_pdu(uint8_t version, uint8_t pdu_num) noexcept {
  if (pdu_num == 0) return false;
  if (pdu_num <= 15) return true;
  // NB-IoT channel types (scat v19+/v20+)
  if (pdu_num >= 45 && pdu_num <= 61) return true;
  (void)version;
  return false;
}

}  // namespace detail

/// Map Qualcomm pdu_num → ChannelType using scat version-specific tables.
/// MIB (BCCH-BCH) returns UNKNOWN — caller handles via is_mib_pdu().
[[nodiscard]] inline ChannelType pdu_num_to_channel(uint8_t version, uint8_t pdu_num) noexcept {
  using C = ChannelType;

  auto map_early = [&]() -> ChannelType {
    // <v9, v13, v22 — scat (0x02..0x08, 0x0d, 0x16)
    switch (pdu_num) {
      case 2: return C::BCCH_DL_SCH;
      case 5: return C::DL_CCCH;
      case 6: return C::DL_DCCH;
      case 7: return C::UL_CCCH;
      case 8: return C::UL_DCCH;
      default: return C::UNKNOWN;  // 1=MIB, 3=MCCH, 4=PCCH
    }
  };

  auto map_v9 = [&]() -> ChannelType {
    // v9–v12
    switch (pdu_num) {
      case 9: return C::BCCH_DL_SCH;
      case 12: return C::DL_CCCH;
      case 13: return C::DL_DCCH;
      case 14: return C::UL_CCCH;
      case 15: return C::UL_DCCH;
      default: return C::UNKNOWN;  // 8=MIB, 10=MCCH, 11=PCCH
    }
  };

  auto map_v14_16 = [&]() -> ChannelType {
    // v14 / v15 / v16 and v20/v24/v25-style (pdu 2 = BCCH-DL-SCH)
    switch (pdu_num) {
      case 2: return C::BCCH_DL_SCH;
      case 6: return C::DL_CCCH;
      case 7: return C::DL_DCCH;
      case 8: return C::UL_CCCH;
      case 9: return C::UL_DCCH;
      case 55: return C::BCCH_DL_SCH;  // NB
      default: return C::UNKNOWN;
    }
  };

  auto map_v19 = [&]() -> ChannelType {
    // v19, v26, v27, v29, v30 — BCCH-DL-SCH is 3, PCCH is 7
    switch (pdu_num) {
      case 3: return C::BCCH_DL_SCH;
      case 8: return C::DL_CCCH;
      case 9: return C::DL_DCCH;
      case 10: return C::UL_CCCH;
      case 11: return C::UL_DCCH;
      case 46: return C::BCCH_DL_SCH;  // NB
      default: return C::UNKNOWN;
    }
  };

  switch (version) {
    case 0x02:
    case 0x03:
    case 0x04:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x0D:
    case 0x16:
      return map_early();
    case 0x09:
    case 0x0C:
      return map_v9();
    case 0x0E:
    case 0x0F:
    case 0x10:
    case 0x14:
    case 0x18:
    case 0x19:
      return map_v14_16();
    case 0x13:
    case 0x1A:
    case 0x1B:
    case 0x1C:
    case 0x1D:
    case 0x1E:
    case 0x1F:
    case 0x20:
    case 0x21:
    case 0x22:
    case 0x23:
    case 0x24:
      return map_v19();
    default:
      // Unknown version: accept common SIB1 / signalling pdus
      if (pdu_num == 2 || pdu_num == 3 || pdu_num == 9) return C::BCCH_DL_SCH;
      if (pdu_num == 5 || pdu_num == 6 || pdu_num == 8 || pdu_num == 12) return C::DL_CCCH;
      if (pdu_num == 7 || pdu_num == 13) return C::DL_DCCH;
      if (pdu_num == 10 || pdu_num == 14) return C::UL_CCCH;
      if (pdu_num == 11 || pdu_num == 15) return C::UL_DCCH;
      return C::UNKNOWN;
  }
}

[[nodiscard]] inline bool is_mib_pdu(uint8_t version, uint8_t pdu_num) noexcept {
  return detail::is_mib_pdu(version, pdu_num);
}

/// Decode 0xB0C0 payload. Returns nullopt if buffer is too short / unknown version /
/// fields look like ASN.1 misdetected as a versioned header.
[[nodiscard]] inline std::optional<LteRrcOtaInfo> decode_lte_rrc_ota(
    std::span<const uint8_t> payload) noexcept {
  auto try_decode = [](std::span<const uint8_t> p) -> std::optional<LteRrcOtaInfo> {
    if (!detail::looks_like_ota_header(p)) return std::nullopt;

    auto L = detail::layout_for_version(p[0]);
    if (!L.valid || p.size() < L.header_size) return std::nullopt;

    LteRrcOtaInfo out;
    out.version = p[0];
    out.pci = Utils::Converter::read_le<uint16_t>(p, L.off_pci);
    out.earfcn = L.earfcn_u32 ? Utils::Converter::read_le<uint32_t>(p, L.off_earfcn)
                              : Utils::Converter::read_le<uint16_t>(p, L.off_earfcn);
    out.pdu_num = p[L.off_pdu_type];
    uint16_t pdu_len = Utils::Converter::read_le<uint16_t>(p, L.off_pdu_len);
    out.pdu_len = pdu_len;
    // scat v30: ... len@19, unk1@21, unk2@22, segment_id@23, ASN.1@24
    if (out.version >= 30 && p.size() >= 24) out.segment_id = p[23];

    if (!detail::is_plausible_lte_rrc_pdu(out.version, out.pdu_num)) return std::nullopt;
    if (out.pci > 503) return std::nullopt;
    if (out.earfcn > 262143) return std::nullopt;
    // Do not require BandInfo membership for EARFCN≥65536 — incomplete band tables
    // rejected valid TDD wrappers (undecoded B0C0). Range check above is enough.

    size_t avail = p.size() - L.header_size;
    if (avail == 0) return std::nullopt;
    // scat uses the full remainder after the header (warns if len mismatches).
    // Truncating to pdu_len drops SIB1 on some SDX55 builds.
    if (pdu_len != 0 && pdu_len > avail + 64) return std::nullopt;
    out.asn1 = p.subspan(L.header_size, avail);
    return out;
  };

  if (auto ok = try_decode(payload)) return ok;

  // Some SDX55 frames prepend a 1-byte flag (seen: 0x01) before ext_header_ver.
  if (payload.size() > 2 && payload[0] <= 0x04 && detail::layout_for_version(payload[1]).valid) {
    if (auto ok = try_decode(payload.subspan(1))) return ok;
  }
  return std::nullopt;
}

/// Build a minimal 7-byte synthetic header for journal ASN.1-only RAW so the
/// common parse path (channel@6, ASN.1@7) and extract_cell_key keep working.
[[nodiscard]] inline std::vector<uint8_t> synthesize_ota_header(uint32_t earfcn, uint16_t pci,
                                                                ChannelType channel,
                                                                std::span<const uint8_t> asn1) {
  std::vector<uint8_t> out;
  out.resize(7 + asn1.size());
  out[0] = 0x00;
  out[1] = 0x00;
  out[2] = static_cast<uint8_t>(earfcn & 0xFF);
  out[3] = static_cast<uint8_t>((earfcn >> 8) & 0xFF);
  out[4] = static_cast<uint8_t>(pci & 0xFF);
  out[5] = static_cast<uint8_t>((pci >> 8) & 0xFF);
  out[6] = static_cast<uint8_t>(channel);
  std::memcpy(out.data() + 7, asn1.data(), asn1.size());
  return out;
}

}  // namespace QCom::Lte
