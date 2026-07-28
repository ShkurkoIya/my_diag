/// @file WcdmaParser.h
/// @brief WCDMA/UMTS DIAG parser — proprietary Qualcomm binary formats.
///
/// All data comes from Qualcomm-proprietary packed binary logs, not ASN.1.
/// Layouts ported from scat/dia_vldos, verified on SM8550.
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

namespace QCom::Wcdma {

using QCom::IRatParser;
using QCom::LogCode;
using QCom::ParserError;
using QCom::QualcommPacketView;

/// Decode 3-byte Qualcomm BCD field (one digit per byte in low nibble, 0xF = stop).
[[nodiscard]] inline uint16_t bcd3_to_int(const uint8_t* p) noexcept {
  uint16_t r = 0;
  for (int i = 0; i < 3; ++i) {
    uint8_t d = p[i] & 0x0F;
    if (d == 0x0F) break;
    if (d > 9) continue;
    r = r * 10 + d;
  }
  return r;
}

struct WcdmaLogEntry {
  LogCode code;
  std::string_view name;
};

class WcdmaParser : public IRatParser {
public:
  static constexpr LogCode WCDMA_CELL_ID = 0x4027;
  static constexpr LogCode WCDMA_RESEL_RANK = 0x4005;
  static constexpr LogCode WCDMA_ACTIVE_SET = 0x4111;
  static constexpr LogCode WCDMA_SERV_CELL = 0x4127;
  static constexpr LogCode WCDMA_RRC_OTA = 0x412F;

  static constexpr std::array kLogTable = {
      WcdmaLogEntry{WCDMA_CELL_ID, "WCDMA Cell ID (0x4027)"},
      WcdmaLogEntry{WCDMA_RESEL_RANK, "WCDMA Resel Rank (0x4005)"},
      WcdmaLogEntry{WCDMA_ACTIVE_SET, "WCDMA Active Set (0x4111)"},
      WcdmaLogEntry{WCDMA_SERV_CELL, "WCDMA Serving Cell (0x4127)"},
      WcdmaLogEntry{WCDMA_RRC_OTA, "WCDMA RRC OTA (0x412F)"},
  };

  WcdmaParser() = default;
  ~WcdmaParser() override = default;

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
    return "Unknown WCDMA";
  }

private:
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_cell_id(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_resel_rank(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_active_set(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_serv_cell(
      std::span<const uint8_t> payload);
};

}  // namespace QCom::Wcdma
