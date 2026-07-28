#pragma once

#include <array>
#include <expected>
#include <string_view>
#include <vector>

#include "core/Events.h"
#include "core/ParserInterface.h"
#include "core/Utils.h"

namespace QCom::Wcdma {

using QCom::IRatParser;
using QCom::LogCode;
using QCom::ParserError;
using QCom::QualcommPacketView;

class WcdmaParser : public IRatParser {
public:
  // Qualcomm DIAG log codes for WCDMA/UMTS
  static constexpr LogCode WCDMA_CELL_ID = 0x4027;
  static constexpr LogCode WCDMA_RESEL_RANK = 0x4005;
  static constexpr LogCode WCDMA_RRC_OTA = 0x412F;
  static constexpr LogCode WCDMA_ACTIVE_SET = 0x4111;
  static constexpr LogCode WCDMA_NEIGHBOR_SET = 0x4127;

  struct LogTableEntry {
    LogCode code;
    std::string_view name;
  };

  static constexpr std::array kLogTable = {
      LogTableEntry{WCDMA_CELL_ID, "WCDMA Cell ID (0x4027)"},
      LogTableEntry{WCDMA_RESEL_RANK, "WCDMA Resel Rank (0x4005)"},
      LogTableEntry{WCDMA_RRC_OTA, "WCDMA RRC OTA (0x412F)"},
      LogTableEntry{WCDMA_ACTIVE_SET, "WCDMA Active Set (0x4111)"},
      LogTableEntry{WCDMA_NEIGHBOR_SET, "WCDMA Neighbor Set (0x4127)"},
  };

  WcdmaParser() = default;
  ~WcdmaParser() override = default;

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse(QualcommPacketView pkt) override {
    // TODO: WCDMA Qualcomm DIAG packet parsing
    //
    // Key targets:
    //   0x4027 — Cell ID: 32-byte scat layout
    //     - BCD MCC/MNC, PSC >> 4, 28-bit UTRAN Cell ID, LAC, UARFCN
    //     - Parse per scat's wcdma_cell_id() function
    //
    //   0x4005 — Reselection Rank: per-cell RSCP/EcNo
    //     - Formula from scat: rscp_raw - 21
    //     - Contains serving + up to 6 neighbor cells
    //
    //   0x412F — RRC OTA: raw UMTS RRC PDU
    //     - Needs asn1c with TS 25.331 grammar (BER, not PER)
    //     - Symbol clash with LTE RRC — must be in separate .so or use srsRAN NR
    //
    //   0x4111 — Active Set: list of PSC in active set
    //   0x4127 — Neighbor Set: monitored set with RSCP/EcNo
    return std::unexpected(ParserError::NotImplemented);
  }

  std::vector<LogCode> get_supported_codes() const override {
    std::vector<LogCode> codes;
    codes.reserve(kLogTable.size());
    for (const auto& e : kLogTable) codes.push_back(e.code);
    return codes;
  }

  std::string_view log_to_string(LogCode log_code) const noexcept override {
    for (const auto& e : kLogTable) {
      if (e.code == log_code) return e.name;
    }
    return "Unknown WCDMA code";
  }
};

}  // namespace QCom::Wcdma
