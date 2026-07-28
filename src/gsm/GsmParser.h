/// @file GsmParser.h
/// @brief GSM DIAG parser — hand-rolled binary parsing for SI messages and ML1.
///
/// GSM System Information uses CSN.1/bitfield encoding (not ASN.1),
/// parsed manually per 3GPP TS 44.018.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <vector>

#include "core/Events.h"
#include "core/ParserInterface.h"
#include "core/Types.h"
#include "core/Utils.h"

namespace QCom::Gsm {

using QCom::IRatParser;
using QCom::LogCode;
using QCom::ParserError;
using QCom::QualcommPacketView;

// ============================================================================
// BCD LAI decoder (3GPP TS 24.008 §10.5.1.3)
// ============================================================================

struct DecodedLai {
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint16_t lac{0};
};

/// Decode 5-byte LAI from GSM System Information.
[[nodiscard]] inline DecodedLai decode_lai(const uint8_t* lai) noexcept {
  DecodedLai r;
  uint8_t d1 = lai[0] & 0x0F, d2 = (lai[0] >> 4) & 0x0F, d3 = lai[1] & 0x0F;
  uint8_t m3 = (lai[1] >> 4) & 0x0F, m1 = lai[2] & 0x0F, m2 = (lai[2] >> 4) & 0x0F;

  if (d1 > 9 || d2 > 9 || d3 > 9) return r;
  r.mcc = d1 * 100 + d2 * 10 + d3;

  if (m3 == 0x0F)
    r.mnc = m1 * 10 + m2;
  else if (m1 <= 9 && m2 <= 9 && m3 <= 9)
    r.mnc = m1 * 100 + m2 * 10 + m3;

  r.lac = static_cast<uint16_t>((lai[3] << 8) | lai[4]);
  return r;
}

// ============================================================================
// CSN.1 MSB-first bit reader (for SI-3 Rest Octets)
// ============================================================================

class BitReader {
public:
  BitReader(const uint8_t* data, size_t nbytes) : m_data(data), m_nbits(nbytes * 8) {}

  [[nodiscard]] bool eof(size_t need = 1) const noexcept { return m_pos + need > m_nbits; }

  [[nodiscard]] uint32_t get(size_t n) noexcept {
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) {
      uint32_t bit = 0;
      if (m_pos < m_nbits) bit = (m_data[m_pos >> 3] >> (7 - (m_pos & 7))) & 1u;
      v = (v << 1) | bit;
      ++m_pos;
    }
    return v;
  }

private:
  const uint8_t* m_data;
  size_t m_nbits;
  size_t m_pos{0};
};

// ============================================================================
// Log table entry
// ============================================================================

struct GsmLogEntry {
  LogCode code;
  std::string_view name;
};

// ============================================================================
// GsmParser
// ============================================================================

class GsmParser : public IRatParser {
public:
  static constexpr LogCode GSM_RR_SIGNALING = 0x512F;
  static constexpr LogCode GSM_CELL_INFO = 0x5134;
  static constexpr LogCode GSM_SURROUND_DB = 0x5071;
  static constexpr LogCode GSM_BURST_METRICS = 0x506C;
  static constexpr LogCode GSM_SERVING_AUX = 0x507A;
  static constexpr LogCode GSM_NEIGHBOR_AUX = 0x507B;

  static constexpr std::array kLogTable = {
      GsmLogEntry{GSM_RR_SIGNALING, "GSM RR Signaling (0x512F)"},
      GsmLogEntry{GSM_CELL_INFO, "GSM Cell Info (0x5134)"},
      GsmLogEntry{GSM_SURROUND_DB, "GSM Surround DB (0x5071)"},
      GsmLogEntry{GSM_BURST_METRICS, "GSM Burst Metrics (0x506C)"},
      GsmLogEntry{GSM_SERVING_AUX, "GSM Serving Aux (0x507A)"},
      GsmLogEntry{GSM_NEIGHBOR_AUX, "GSM Neighbor Aux (0x507B)"},
  };

  GsmParser() = default;
  ~GsmParser() override = default;

  [[nodiscard]] std::expected<std::vector<Events::RrcEvent>, ParserError> parse(
      QualcommPacketView pkt) override;

  [[nodiscard]] std::vector<LogCode> get_supported_codes() const override {
    std::vector<LogCode> codes;
    codes.reserve(kLogTable.size());
    for (const auto& e : kLogTable) codes.push_back(e.code);
    return codes;
  }

  [[nodiscard]] std::string_view log_to_string(LogCode code) const noexcept override {
    for (const auto& e : kLogTable) {
      if (e.code == code) return e.name;
    }
    return "Unknown GSM";
  }

private:
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_rr_signaling(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_cell_info(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_surround_db(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_burst_metrics(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_serving_aux(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_neighbor_aux(
      std::span<const uint8_t> payload);

  std::vector<Events::RrcEvent> parse_si3(const uint8_t* l3, size_t len);
};

}  // namespace QCom::Gsm
