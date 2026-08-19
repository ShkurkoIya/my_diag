/// @file Utils.h
/// @brief Binary parsing utilities: LE readers, bitfield extraction, validity checks.
#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace QCom::Utils {

class Converter {
public:
  template <typename T>
  [[nodiscard]] static constexpr T read_le(std::string_view data, size_t offset) noexcept {
    if (offset + sizeof(T) > data.size()) return T{};
    T result{};
    std::memcpy(&result, data.data() + offset, sizeof(T));
    return result;
  }

  template <typename T>
  [[nodiscard]] static constexpr T read_le(std::span<const uint8_t> data, size_t offset) noexcept {
    if (offset + sizeof(T) > data.size()) return T{};
    T result{};
    std::memcpy(&result, data.data() + offset, sizeof(T));
    return result;
  }

  /// @pre offset + sizeof(T) must be within valid readable range.
  template <typename T>
  [[nodiscard]] static T read_le(const uint8_t* data, size_t offset) noexcept {
    T result{};
    std::memcpy(&result, data + offset, sizeof(T));
    return result;
  }

  template <typename Container>
  [[nodiscard]] static constexpr uint16_t digits_to_number(const Container& digits) noexcept {
    uint16_t result = 0;
    for (size_t i = 0; i < digits.size(); ++i) {
      assert(digits[i] <= 9);
      result = result * 10 + digits[i];
    }
    return result;
  }
};

/// Extract `nbits` starting at bit position `lsb` from a 32-bit word.
[[nodiscard]] inline constexpr uint32_t bits(uint32_t word, unsigned lsb, unsigned nbits) noexcept {
  if (nbits == 0) return 0;
  if (nbits >= 32) return word >> lsb;
  return (word >> lsb) & ((1u << nbits) - 1);
}

// ============================================================================
// ML1 conversion formulas (from scat, verified against Qualcomm QXDM)
// ============================================================================

/// LTE/NR RSRP: raw * 0.0625 - 180.0 (dBm)
[[nodiscard]] inline constexpr float ml1_rsrp(uint32_t raw) noexcept {
  return static_cast<float>(raw) * 0.0625f - 180.0f;
}

/// LTE/NR RSRQ: raw * 0.0625 - 30.0 (dB)
[[nodiscard]] inline constexpr float ml1_rsrq(uint32_t raw) noexcept {
  return static_cast<float>(raw) * 0.0625f - 30.0f;
}

/// LTE RSSI: raw * 0.0625 - 110.0 (dBm)
[[nodiscard]] inline constexpr float ml1_rssi(uint32_t raw) noexcept {
  return static_cast<float>(raw) * 0.0625f - 110.0f;
}

/// LTE ML1 FTL SNR (0xB193): raw * 0.1 - 20.0 (dB)
[[nodiscard]] inline constexpr float ml1_ftl_snr(uint32_t raw) noexcept {
  return static_cast<float>(raw) * 0.1f - 20.0f;
}

/// NR SINR: raw * 0.0625 - 20.0 (dB)
[[nodiscard]] inline constexpr float ml1_nr_sinr(uint32_t raw) noexcept {
  return static_cast<float>(raw) * 0.0625f - 20.0f;
}

// ============================================================================
// Validity guards — fail-closed: reject garbage values
// ============================================================================

[[nodiscard]] inline constexpr bool valid_lte_earfcn(uint32_t v) noexcept {
  // 3GPP TS 36.101 EARFCN; reject 0 / 0xFFFFFFFF modem padding.
  return v > 0 && v <= 262143;
}
[[nodiscard]] inline constexpr bool valid_lte_pci(uint16_t v) noexcept { return v <= 503; }
/// 28-bit E-UTRAN Cell Identity (reject 0xFFFFFFFF padding).
[[nodiscard]] inline constexpr bool valid_lte_eci(uint64_t v) noexcept {
  return v > 0 && v <= 0x0FFFFFFFull;
}
/// 16-bit TAC (0xFFFF is reserved / unused).
[[nodiscard]] inline constexpr bool valid_lte_tac(uint32_t v) noexcept {
  return v > 0 && v < 0xFFFFu;
}

/// How PCI sits in an ML1 `pci_serv_layer_prio` / meas word (0xB17F, 0xB197, 0xB193).
/// Decide by **refutation**, not by log version — v5 and v7 on Snapdragon are both LSB,
/// while scat's >>7 is MSB. A wrong PCI mints hop targets that never camp.
enum class LtePciPack { Unknown, Lsb, Msb };

[[nodiscard]] inline constexpr uint16_t lte_pci_lsb(uint16_t w) noexcept {
  return static_cast<uint16_t>(w & 0x1FFu);
}
[[nodiscard]] inline constexpr uint16_t lte_pci_msb(uint16_t w) noexcept {
  return static_cast<uint16_t>((w >> 7) & 0x1FFu);
}
/// LSB packing: layer prio in bits 9..11 (TS 36.304 0..7). Bit 12 is a flag, not prio.
[[nodiscard]] inline constexpr uint8_t lte_pci_lsb_prio(uint16_t w) noexcept {
  return static_cast<uint8_t>((w >> 9) & 0x7u);
}
/// MSB/scat packing: prio in the low bits. 3 bits (0..7), not the old 7-bit mask.
[[nodiscard]] inline constexpr uint8_t lte_pci_msb_prio(uint16_t w) noexcept {
  return static_cast<uint8_t>(w & 0x7u);
}

/// One word that refutes exactly one packing proves the other.
/// When both 9-bit slices look like a PCI, low 7 bits >7 means those bits are PCI
/// (LSB), not scat's layer-prio — same discriminator as Observer-Android.
[[nodiscard]] inline constexpr LtePciPack lte_pci_pack_from_word(uint16_t w) noexcept {
  const uint16_t lo = lte_pci_lsb(w);
  const uint16_t hi = lte_pci_msb(w);
  const bool lsb_pci = lo >= 1 && lo <= 503;
  const bool msb_pci = hi >= 1 && hi <= 503;
  if (lo > 503 && msb_pci) return LtePciPack::Msb;
  if (hi > 503 && lsb_pci) return LtePciPack::Lsb;
  if (lo == 0 && msb_pci) return LtePciPack::Msb;
  if (hi == 0 && lsb_pci) return LtePciPack::Lsb;
  if (lsb_pci && msb_pci && (w & 0x7Fu) > 7) return LtePciPack::Lsb;
  return LtePciPack::Unknown;
}

struct LtePackedPci {
  uint16_t pci{0};
  uint8_t prio{0};
  LtePciPack pack{LtePciPack::Unknown};
};

[[nodiscard]] inline constexpr LtePackedPci lte_unpack_pci_slp(
    uint16_t w, LtePciPack hint = LtePciPack::Unknown) noexcept {
  LtePciPack pack = lte_pci_pack_from_word(w);
  if (pack == LtePciPack::Unknown) pack = hint;
  if (pack == LtePciPack::Unknown) {
    const uint16_t lo = lte_pci_lsb(w);
    pack = (lo != 0 && lo <= 503) ? LtePciPack::Lsb : LtePciPack::Msb;
  }
  LtePackedPci out;
  out.pack = pack;
  if (pack == LtePciPack::Lsb) {
    out.pci = lte_pci_lsb(w);
    out.prio = lte_pci_lsb_prio(w);
  } else {
    out.pci = lte_pci_msb(w);
    out.prio = lte_pci_msb_prio(w);
  }
  return out;
}

/// Session lock: decisive 0xB17F words vote; 0xB197 inherits (no prio field of its own).
struct LtePciPackOrder {
  LtePciPack locked{LtePciPack::Unknown};
  unsigned votes_lsb{0};
  unsigned votes_msb{0};
  static constexpr unsigned k_min_votes{3};

  void observe(uint16_t w) noexcept {
    const auto d = lte_pci_pack_from_word(w);
    if (d == LtePciPack::Unknown) return;
    (d == LtePciPack::Lsb ? votes_lsb : votes_msb)++;
    if (locked != LtePciPack::Unknown) return;
    const unsigned win = (d == LtePciPack::Lsb) ? votes_lsb : votes_msb;
    const unsigned lose = (d == LtePciPack::Lsb) ? votes_msb : votes_lsb;
    if (win >= k_min_votes && win > lose) locked = d;
  }

  [[nodiscard]] LtePackedPci unpack(uint16_t w) const noexcept {
    const auto decided = lte_pci_pack_from_word(w);
    return lte_unpack_pci_slp(w, decided != LtePciPack::Unknown ? decided : locked);
  }
};

/// PCI packed in ML1 meas words (0xB17F / 0xB193 cells).
[[nodiscard]] inline constexpr uint16_t lte_pci_from_meas_word(uint16_t w) noexcept {
  return lte_unpack_pci_slp(w).pci;
}
[[nodiscard]] inline constexpr bool valid_lte_rsrp(float v) noexcept {
  // Survey-grade: reject nonsense extremes (−30 from bad decode, <−150 noise floor).
  return v >= -150.0f && v <= -40.0f;
}
[[nodiscard]] inline constexpr bool valid_nr_arfcn(uint32_t v) noexcept { return v <= 3279165; }
[[nodiscard]] inline constexpr bool valid_nr_pci(uint16_t v) noexcept { return v <= 1007; }

}  // namespace QCom::Utils
