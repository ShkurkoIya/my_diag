#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>
#include <vector>

#include "core/BaseRatParser.h"
#include "core/Events.h"
#include "srsran/asn1/rrc_nr.h"

namespace QCom::Nr {

using BcchMsg = asn1::rrc_nr::bcch_dl_sch_msg_s;
using DlCcchMsg = asn1::rrc_nr::dl_ccch_msg_s;
using DlDcchMsg = asn1::rrc_nr::dl_dcch_msg_s;
using UlCcchMsg = asn1::rrc_nr::ul_ccch_msg_s;
using UlDcchMsg = asn1::rrc_nr::ul_dcch_msg_s;

using BcchMsgTypes = asn1::rrc_nr::bcch_dl_sch_msg_type_c::c1_c_::types;
using DlCcchMsgTypes = asn1::rrc_nr::dl_ccch_msg_type_c::c1_c_::types;
using DlDcchMsgTypes = asn1::rrc_nr::dl_dcch_msg_type_c::c1_c_::types;
using UlDcchMsgTypes = asn1::rrc_nr::ul_dcch_msg_type_c::c1_c_::types;

class NrParser;

}  // namespace QCom::Nr

namespace QCom {
template <>
struct ParserTraits<Nr::NrParser> {
  using MessageTuple = std::tuple<std::nullptr_t,  // index 0 — unused
                                  Nr::BcchMsg,     // index 1 — BCCH_DL_SCH
                                  Nr::DlCcchMsg,   // index 2 — DL_CCCH
                                  Nr::DlDcchMsg,   // index 3 — DL_DCCH
                                  Nr::UlCcchMsg,   // index 4 — UL_CCCH
                                  Nr::UlDcchMsg    // index 5 — UL_DCCH
                                  >;
};
}  // namespace QCom

namespace QCom::Nr {

using QCom::BaseRatParser;
using QCom::ChannelType;
using QCom::IRatParser;
using QCom::LocalCellKey;
using QCom::LogCode;
using QCom::LogEntry;
using QCom::ParserError;
using QCom::ParserTraits;
using QCom::QualcommPacketView;

class NrParser : public BaseRatParser<NrParser> {
  friend class BaseRatParser<NrParser>;

public:
  // Qualcomm NR DIAG log codes
  static constexpr LogCode NR_RRC_OTA = 0xB821;
  static constexpr LogCode NR_ML1_MEAS = 0xB97F;
  static constexpr LogCode NR_ML1_SERV = 0xB992;

  static constexpr size_t OFF_FREQ = 2;
  static constexpr size_t OFF_PCI = 4;

  NrParser() = default;
  ~NrParser() override = default;

  [[nodiscard]] std::optional<LocalCellKey> parse_metadata(std::string_view metadata) noexcept {
    if (metadata.size() < QCOM_RRC_METADATA_SIZE) return std::nullopt;
    uint32_t nrarfcn = Utils::Converter::read_le<uint16_t>(metadata, OFF_FREQ);
    uint16_t pci = Utils::Converter::read_le<uint16_t>(metadata, OFF_PCI);
    return LocalCellKey{.freq = nrarfcn, .pci_bsic = pci};
  }

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_rrc_ota(
      std::string_view payload) {
    return parse_rrc_ota_base(payload);
  }

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_metrics(
      std::string_view payload);

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_serving(
      std::string_view payload);

  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(BcchMsg& msg);
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(DlCcchMsg& msg);
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(DlDcchMsg& msg);
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(UlCcchMsg& msg);
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(UlDcchMsg& msg);

  static constexpr std::array kLogTable = {
      LogEntry<NrParser>{NR_RRC_OTA, &NrParser::parse_rrc_ota, "NR RRC OTA (0xB821)"},
      LogEntry<NrParser>{NR_ML1_MEAS, &NrParser::parse_ml1_metrics, "NR ML1 Meas DB (0xB97F)"},
      LogEntry<NrParser>{NR_ML1_SERV, &NrParser::parse_ml1_serving, "NR ML1 Serving (0xB992)"},
  };

private:
  [[nodiscard]] std::vector<Events::RrcEvent> extract_sib1(const asn1::rrc_nr::sib1_s& sib1);
};

}  // namespace QCom::Nr
