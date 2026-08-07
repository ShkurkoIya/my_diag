/// @file BandInfo.h
/// @brief Band / duplex / RF helpers (3GPP TS 36.101, 25.101, 45.005, 45.008).
///
/// Derived fields for SDR-style export — no DIAG dependency.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace QCom::BandInfo {

enum class Duplex : uint8_t { Unknown = 0, FDD = 1, TDD = 2 };

[[nodiscard]] inline const char* to_string(Duplex d) noexcept {
  switch (d) {
    case Duplex::FDD: return "FDD";
    case Duplex::TDD: return "TDD";
    default: return "";
  }
}

struct LteBandInfo {
  uint16_t band{0};       ///< E-UTRA band number
  Duplex duplex{Duplex::Unknown};
  double dl_mhz{0};       ///< DL carrier centre (approx)
  double ul_mhz{0};       ///< UL carrier centre (0 for TDD / unknown)
  const char* name{""};   ///< e.g. "B20"
};

struct UmtsBandInfo {
  uint16_t band{0};
  Duplex duplex{Duplex::FDD};
  double dl_mhz{0};
  double ul_mhz{0};
  const char* name{""};
};

struct GsmBandInfo {
  enum class Band : uint8_t {
    Unknown = 0,
    GSM450,
    GSM480,
    GSM850,
    GSM900,
    DCS1800,
    PCS1900
  };
  Band band{Band::Unknown};
  const char* name{""};
};

// ── LTE EARFCN → band / MHz (TS 36.101 Table 5.7.3-1, common bands) ─────────

namespace detail {

struct LteBandRow {
  uint16_t band;
  Duplex duplex;
  uint32_t n_dl_min;
  uint32_t n_dl_max;
  double f_dl_low;
  uint32_t n_offs_dl;
  double f_ul_low;   ///< 0 if TDD
  uint32_t n_offs_ul;
  const char* name;
};

// Subset covering Russian / EU commercial bands + common TDD.
inline constexpr LteBandRow kLteBands[] = {
    {1, Duplex::FDD, 0, 599, 2110.0, 0, 1920.0, 18000, "B1"},
    {3, Duplex::FDD, 1200, 1949, 1805.0, 1200, 1710.0, 19200, "B3"},
    {7, Duplex::FDD, 2750, 3449, 2620.0, 2750, 2500.0, 20750, "B7"},
    {8, Duplex::FDD, 3450, 3799, 925.0, 3450, 880.0, 21450, "B8"},
    {20, Duplex::FDD, 6150, 6449, 791.0, 6150, 832.0, 24150, "B20"},
    {28, Duplex::FDD, 9210, 9659, 758.0, 9210, 703.0, 27210, "B28"},
    {38, Duplex::TDD, 37750, 38249, 2570.0, 37750, 0, 0, "B38"},
    {40, Duplex::TDD, 38650, 39649, 2300.0, 38650, 0, 0, "B40"},
    {41, Duplex::TDD, 39650, 41589, 2496.0, 39650, 0, 0, "B41"},
};

}  // namespace detail

[[nodiscard]] inline LteBandInfo lte_from_earfcn(uint32_t earfcn,
                                                 uint8_t band_hint = 0) noexcept {
  LteBandInfo out;
  for (const auto& row : detail::kLteBands) {
    if (band_hint && row.band != band_hint) continue;
    if (earfcn < row.n_dl_min || earfcn > row.n_dl_max) continue;
    out.band = row.band;
    out.duplex = row.duplex;
    out.name = row.name;
    out.dl_mhz = row.f_dl_low + 0.1 * static_cast<double>(earfcn - row.n_offs_dl);
    if (row.duplex == Duplex::FDD && row.f_ul_low > 0) {
      // UL EARFCN = N_offs_ul + (N_dl - N_offs_dl) for paired spectrum
      uint32_t ul = row.n_offs_ul + (earfcn - row.n_offs_dl);
      out.ul_mhz = row.f_ul_low + 0.1 * static_cast<double>(ul - row.n_offs_ul);
    } else {
      out.ul_mhz = out.dl_mhz;  // TDD
    }
    return out;
  }
  // hint-only fallback
  if (band_hint) {
    for (const auto& row : detail::kLteBands) {
      if (row.band != band_hint) continue;
      out.band = row.band;
      out.duplex = row.duplex;
      out.name = row.name;
      return out;
    }
  }
  return out;
}

/// True if EARFCN maps to a known commercial band in @ref kLteBands (filters DIAG garbage).
[[nodiscard]] inline bool is_known_lte_earfcn(uint32_t earfcn) noexcept {
  return lte_from_earfcn(earfcn).band != 0;
}

[[nodiscard]] inline Duplex lte_duplex(uint8_t band) noexcept {
  if (band >= 33 && band <= 51) return Duplex::TDD;
  if (band == 0) return Duplex::Unknown;
  return Duplex::FDD;
}

/// LTE 28-bit ECI → eNodeB ID (20-bit) + local cell (8-bit). Common macro layout.
[[nodiscard]] inline uint32_t lte_enb_id(uint64_t eci) noexcept {
  return static_cast<uint32_t>((eci >> 8) & 0xFFFFFu);
}
[[nodiscard]] inline uint8_t lte_local_cell_id(uint64_t eci) noexcept {
  return static_cast<uint8_t>(eci & 0xFFu);
}

// ── UMTS UARFCN (TS 25.101, common FDD bands) ───────────────────────────────

[[nodiscard]] inline UmtsBandInfo umts_from_uarfcn(uint32_t uarfcn) noexcept {
  UmtsBandInfo out;
  // Band I (2100): DL 10562-10838
  if (uarfcn >= 10562 && uarfcn <= 10838) {
    out = {1, Duplex::FDD, uarfcn / 5.0, (uarfcn - 950) / 5.0, "B1"};
    return out;
  }
  // Band VIII (900): DL 2937-3088
  if (uarfcn >= 2937 && uarfcn <= 3088) {
    out = {8, Duplex::FDD, uarfcn / 5.0, (uarfcn - 225) / 5.0, "B8"};
    return out;
  }
  // Band V (850)
  if (uarfcn >= 4357 && uarfcn <= 4458) {
    out = {5, Duplex::FDD, uarfcn / 5.0, (uarfcn - 225) / 5.0, "B5"};
    return out;
  }
  return out;
}

/// UTRAN Cell Identity (28-bit) = RNC-ID (12) || C-ID (16) — TS 25.401
[[nodiscard]] inline uint16_t umts_rnc_id(uint64_t cid28) noexcept {
  return static_cast<uint16_t>((cid28 >> 16) & 0x0FFFu);
}
[[nodiscard]] inline uint16_t umts_cid16(uint64_t cid28) noexcept {
  return static_cast<uint16_t>(cid28 & 0xFFFFu);
}

// ── GSM band + C1/C2 (TS 45.005 / 45.008) ───────────────────────────────────

[[nodiscard]] inline GsmBandInfo gsm_from_arfcn(uint16_t arfcn, int band_hint = -1) noexcept {
  GsmBandInfo out;
  if (arfcn == 0 || (arfcn >= 1 && arfcn <= 124) || (arfcn >= 975 && arfcn <= 1023)) {
    out = {GsmBandInfo::Band::GSM900, "GSM900"};
  } else if (arfcn >= 128 && arfcn <= 251) {
    out = {GsmBandInfo::Band::GSM850, "GSM850"};
  } else if (arfcn >= 259 && arfcn <= 293) {
    out = {GsmBandInfo::Band::GSM450, "GSM450"};
  } else if (arfcn >= 306 && arfcn <= 340) {
    out = {GsmBandInfo::Band::GSM480, "GSM480"};
  } else if (arfcn >= 512 && arfcn <= 885) {
    if (band_hint == 1)
      out = {GsmBandInfo::Band::PCS1900, "PCS1900"};
    else
      out = {GsmBandInfo::Band::DCS1800, "DCS1800"};
  } else {
    out = {GsmBandInfo::Band::Unknown, ""};
  }
  return out;
}

[[nodiscard]] inline int gsm_ms_max_power_dbm(GsmBandInfo::Band b) noexcept {
  switch (b) {
    case GsmBandInfo::Band::DCS1800:
    case GsmBandInfo::Band::PCS1900: return 30;
    default: return 33;
  }
}

[[nodiscard]] inline int gsm_pwr_ctrl_to_dbm(uint8_t level, GsmBandInfo::Band b) noexcept {
  if (b == GsmBandInfo::Band::DCS1800 || b == GsmBandInfo::Band::PCS1900) {
    if (level == 29) return 36;
    if (level == 30) return 34;
    if (level == 31) return 32;
    if (level <= 28) return 30 - 2 * static_cast<int>(level);
    return 30;
  }
  if (level <= 2) return 39;
  if (level <= 19) return 39 - 2 * (static_cast<int>(level) - 2);
  return 5;
}

/// C1 = A − max(B,0); A = RXLEV_units − RXLEV_ACCESS_MIN (TS 45.008 §6.4)
[[nodiscard]] inline int gsm_compute_c1(int rxlev_dbm, uint8_t rxlev_access_min,
                                        uint8_t ms_txpwr_max_cch,
                                        GsmBandInfo::Band band) noexcept {
  int rxlev_units = rxlev_dbm + 110;
  if (rxlev_units < 0) rxlev_units = 0;
  if (rxlev_units > 63) rxlev_units = 63;
  int A = rxlev_units - static_cast<int>(rxlev_access_min);
  int B = gsm_pwr_ctrl_to_dbm(ms_txpwr_max_cch, band) - gsm_ms_max_power_dbm(band);
  if (B < 0) B = 0;
  return A - B;
}

/// Steady-state C2 (T > PENALTY_TIME ⇒ H=0): C2 = C1 + CRO, or C1−CRO if PT=31
[[nodiscard]] inline int gsm_compute_c2(int c1, bool params_present, uint8_t cro,
                                        uint8_t /*temporary_offset*/,
                                        uint8_t penalty_time) noexcept {
  if (!params_present) return c1;
  int cro_db = static_cast<int>(cro) * 2;
  if (penalty_time == 31) return c1 - cro_db;
  return c1 + cro_db;
}

}  // namespace QCom::BandInfo
