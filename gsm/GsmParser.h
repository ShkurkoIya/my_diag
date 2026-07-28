#pragma once

#include <array>
#include <expected>
#include <string_view>
#include <vector>

#include "../Events.h"
#include "../ParserInterface.h"
#include "../Utils.h"

namespace QCom::Gsm {

using QCom::IRatParser;
using QCom::LogCode;
using QCom::ParserError;
using QCom::QualcommPacketView;

class GsmParser : public IRatParser {
public:
  // Qualcomm DIAG log codes for GSM
  static constexpr LogCode GSM_RR_SIGNALING = 0x512F;
  static constexpr LogCode GSM_CELL_INFO = 0x5071;
  static constexpr LogCode GSM_SURROUND_DB = 0x506C;
  static constexpr LogCode GSM_SURROUND_MEAS = 0x5065;
  static constexpr LogCode GSM_BURST_METRICS = 0x5066;
  static constexpr LogCode GSM_BA_LIST = 0x507A;
  static constexpr LogCode GSM_RR_CELL_RESEL = 0x5134;

  struct LogTableEntry {
    LogCode code;
    std::string_view name;
  };

  static constexpr std::array kLogTable = {
      LogTableEntry{GSM_RR_SIGNALING, "GSM RR Signaling (0x512F)"},
      LogTableEntry{GSM_CELL_INFO, "GSM Cell Info (0x5071)"},
      LogTableEntry{GSM_SURROUND_DB, "GSM Surround DB (0x506C)"},
      LogTableEntry{GSM_SURROUND_MEAS, "GSM Surround Meas (0x5065)"},
      LogTableEntry{GSM_BURST_METRICS, "GSM Burst Metrics (0x5066)"},
      LogTableEntry{GSM_BA_LIST, "GSM BA List (0x507A)"},
      LogTableEntry{GSM_RR_CELL_RESEL, "GSM Cell Reselection (0x5134)"},
  };

  GsmParser() = default;
  ~GsmParser() override = default;

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse(QualcommPacketView pkt) override {
    // TODO: GSM SI-3/SI-4/SI-6 manual bitfield parsing (no srsRAN codec available)
    // GSM RR messages use CSN.1 encoding, must be hand-parsed per 3GPP TS 44.018
    //
    // Key targets:
    //   0x512F — RR signaling: contains SI-3 (LAC, CID, PLMN) and SI-4/SI-6
    //   0x5071 — Cell Info: ARFCN, BSIC, RxLev of serving cell
    //   0x506C — Surround DB: neighbor ARFCN list with RxLev
    //   0x5065 — Surround Meas: live neighbor signal levels
    //   0x5066 — Burst Metrics: serving cell RxLev/RxQual
    //   0x507A — BA List: broadcast allocation list (BCCH frequencies)
    //   0x5134 — Cell Reselection: C1/C2 criteria per TS 45.008
    //
    // GSM SI-3 layout (from scat):
    //   Byte 0-1: Cell Identity (16-bit)
    //   Byte 2-6: LAI (MCC-MNC-LAC in BCD)
    //   Byte 7-8: Cell Selection Parameters
    //   Byte 9+:  Rest Octets (CSN.1 bitfield for CELL_RESELECT_OFFSET etc.)
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
    return "Unknown GSM code";
  }
};

}  // namespace QCom::Gsm
