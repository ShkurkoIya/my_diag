/// @file LteParser.h
/// @brief LTE DIAG parser — ASN.1 (srsRAN) + proprietary Qualcomm binary formats.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/BaseRatParser.h"
#include "core/Events.h"
#include "srsran/asn1/rrc.h"
#include "srsran/asn1/rrc/bcch_msg.h"
#include "srsran/asn1/rrc/dl_ccch_msg.h"
#include "srsran/asn1/rrc/dl_dcch_msg.h"
#include "srsran/asn1/rrc/si.h"
#include "srsran/asn1/rrc/ul_ccch_msg.h"
#include "srsran/asn1/rrc/ul_dcch_msg.h"

namespace QCom::Lte {

using BcchMsg = asn1::rrc::bcch_dl_sch_msg_s;
using DlCcchMsg = asn1::rrc::dl_ccch_msg_s;
using DlDcchMsg = asn1::rrc::dl_dcch_msg_s;
using UlCcchMsg = asn1::rrc::ul_ccch_msg_s;
using UlDcchMsg = asn1::rrc::ul_dcch_msg_s;

using BcchMsgTypes = asn1::rrc::bcch_dl_sch_msg_type_c::c1_c_::types;
using DlCcchMsgTypes = asn1::rrc::dl_ccch_msg_type_c::c1_c_::types;
using DlDcchMsgTypes = asn1::rrc::dl_dcch_msg_type_c::c1_c_::types;
using UlDcchMsgTypes = asn1::rrc::ul_dcch_msg_type_c::c1_c_::types;

using SibItemTypes = asn1::rrc::sys_info_r8_ies_s::sib_type_and_info_item_c_::types;
using LteMibBandwidthEnum = asn1::rrc::mib_s::dl_bw_e_;

[[nodiscard]] inline uint8_t rb_to_mhz(LteMibBandwidthEnum bw) noexcept {
  switch (bw.to_number()) {
    case 6: return 1;
    case 15: return 3;
    case 25: return 5;
    case 50: return 10;
    case 75: return 15;
    case 100: return 20;
    default: return 0;
  }
}

class LteParser;

}  // namespace QCom::Lte

namespace QCom {
template <>
struct ParserTraits<Lte::LteParser> {
  using MessageTuple = std::tuple<std::nullptr_t, Lte::BcchMsg, Lte::DlCcchMsg, Lte::DlDcchMsg,
                                  Lte::UlCcchMsg, Lte::UlDcchMsg>;
};
}  // namespace QCom

namespace QCom::Lte {

using QCom::BaseRatParser;
using QCom::ChannelType;
using QCom::IRatParser;
using QCom::LocalCellKey;
using QCom::LogCode;
using QCom::LogEntry;
using QCom::ParserError;
using QCom::ParserTraits;
using QCom::QualcommPacketView;

class LteParser : public BaseRatParser<LteParser> {
  friend class BaseRatParser<LteParser>;

public:
  /// @name Qualcomm DIAG Log Codes for LTE
  /// @{
  static constexpr LogCode LTE_RRC_OTA = 0xB0C0;         ///< RRC Over-The-Air (ASN.1 PER)
  static constexpr LogCode LTE_SERV_CELL_INFO = 0xB0C2;  ///< Proprietary serving cell identity
  static constexpr LogCode LTE_CELL_INFO = 0xB175;       ///< MIB / Cell Info
  static constexpr LogCode LTE_ML1_SERV_MEAS = 0xB17F;   ///< ML1 Serving Cell Meas & Eval
  static constexpr LogCode LTE_ML1_NEIGH_MEAS = 0xB180;  ///< ML1 Neighbor Measurements
  static constexpr LogCode LTE_ML1_MEAS_RESP = 0xB193;   ///< ML1 Serving Cell Meas Response
  static constexpr LogCode LTE_ML1_SERV_INFO = 0xB197;   ///< ML1 Serving Cell Information
  /// @}

  static constexpr size_t OFF_FREQ = 2;
  static constexpr size_t OFF_PCI = 4;
  static constexpr size_t QCOM_MIB_MIN_SIZE = 10;

  LteParser() = default;
  ~LteParser() override = default;

  [[nodiscard]] std::optional<LocalCellKey> parse_metadata(
      std::span<const uint8_t> metadata) noexcept {
    if (metadata.size() < QCOM_RRC_METADATA_SIZE) return std::nullopt;
    uint16_t earfcn = Utils::Converter::read_le<uint16_t>(metadata, OFF_FREQ);
    uint16_t pci = Utils::Converter::read_le<uint16_t>(metadata, OFF_PCI);
    return LocalCellKey{.freq = earfcn, .pci_bsic = pci};
  }

  // --- ASN.1 layer handlers ---
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_rrc_ota(
      std::span<const uint8_t> payload) {
    return parse_rrc_ota_base(payload);
  }

  // --- Proprietary binary layer handlers ---
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_serv_cell_info(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_mib_metrics(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_serving(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_neighbors(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_meas_resp(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_serv_info(
      std::span<const uint8_t> payload);

  // --- ASN.1 channel message handlers ---
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(BcchMsg& msg);
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(DlCcchMsg& msg);
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(DlDcchMsg& msg);
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(UlCcchMsg& msg);
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(UlDcchMsg& msg);

  static constexpr std::array kLogTable = {
      LogEntry<LteParser>{LTE_RRC_OTA, &LteParser::parse_rrc_ota, "LTE RRC OTA (0xB0C0)"},
      LogEntry<LteParser>{LTE_SERV_CELL_INFO, &LteParser::parse_serv_cell_info,
                          "LTE Serving Cell Info (0xB0C2)"},
      LogEntry<LteParser>{LTE_CELL_INFO, &LteParser::parse_mib_metrics,
                          "LTE Cell Info / MIB (0xB175)"},
      LogEntry<LteParser>{LTE_ML1_SERV_MEAS, &LteParser::parse_ml1_serving,
                          "LTE ML1 Serving Meas (0xB17F)"},
      LogEntry<LteParser>{LTE_ML1_NEIGH_MEAS, &LteParser::parse_ml1_neighbors,
                          "LTE ML1 Neighbor Meas (0xB180)"},
      LogEntry<LteParser>{LTE_ML1_MEAS_RESP, &LteParser::parse_ml1_meas_resp,
                          "LTE ML1 Meas Response (0xB193)"},
      LogEntry<LteParser>{LTE_ML1_SERV_INFO, &LteParser::parse_ml1_serv_info,
                          "LTE ML1 Serving Info (0xB197)"},
  };

private:
  [[nodiscard]] std::vector<Events::RrcEvent> extract_sib1(const asn1::rrc::sib_type1_s& sib1);
  [[nodiscard]] std::vector<Events::RrcEvent> extract_sys_info(
      const asn1::rrc::sys_info_s& sys_info);
};

}  // namespace QCom::Lte
