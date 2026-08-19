/// @file LteParser.h
/// @brief LTE DIAG parser — ASN.1 (srsRAN) + proprietary Qualcomm binary formats.
#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/BaseRatParser.h"
#include <observer/model/Events.h>
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

/// B0C0 mapped-channel parses that produced no ASN.1 events (unpack fail / unhandled).
[[nodiscard]] uint64_t lte_rrc_ota_asn1_empty_count() noexcept;

class LteParser : public BaseRatParser<LteParser> {
  friend class BaseRatParser<LteParser>;

public:
  /// @name Qualcomm DIAG Log Codes for LTE
  /// @{
  static constexpr LogCode LTE_RRC_OTA = 0xB0C0;
  static constexpr LogCode LTE_RRC_MIB = 0xB0C1;
  static constexpr LogCode LTE_SERV_CELL_INFO = 0xB0C2;
  static constexpr LogCode LTE_PLMN_SEARCH_REQ = 0xB0C3;
  static constexpr LogCode LTE_PLMN_SEARCH_RSP = 0xB0C4;
  static constexpr LogCode LTE_RRC_PAGING = 0xB0CB;
  static constexpr LogCode LTE_RRC_CA_COMBOS = 0xB0CD;
  static constexpr LogCode LTE_CELL_INFO = 0xB175;
  static constexpr LogCode LTE_INITIAL_ACQ = 0xB176;
  static constexpr LogCode LTE_ML1_CONN_INTRA = 0xB179;
  static constexpr LogCode LTE_ML1_SERV_MEAS = 0xB17F;
  static constexpr LogCode LTE_ML1_NEIGH_MEAS = 0xB180;
  static constexpr LogCode LTE_ML1_INTRA_RESEL = 0xB181;
  static constexpr LogCode LTE_ML1_NEIGH_REQ = 0xB192;
  static constexpr LogCode LTE_ML1_CONN_NEIGH = 0xB195;
  static constexpr LogCode LTE_ML1_MEAS_RESP = 0xB193;
  static constexpr LogCode LTE_ML1_SEARCH_RR = 0xB194;
  static constexpr LogCode LTE_ML1_SERV_INFO = 0xB197;
  static constexpr LogCode LTE_LL1_PSS = 0xB113;
  static constexpr LogCode LTE_LL1_FRAME_TIMING = 0xB114;
  static constexpr LogCode LTE_LL1_SSS = 0xB115;
  static constexpr LogCode LTE_LL1_NCELL_CER = 0xB123;
  static constexpr LogCode LTE_NAS_EMM_DL = 0xB0EC;
  static constexpr LogCode LTE_NAS_EMM_SEC_IN = 0xB0EA;
  static constexpr LogCode LTE_NAS_EMM_SEC_OUT = 0xB0EB;
  static constexpr LogCode LTE_NAS_EMM_PLAIN_OUT = 0xB0ED;
  static constexpr LogCode LTE_NAS_EMM_STATE = 0xB0EE;
  static constexpr LogCode LTE_NAS_ESM_PLAIN_IN = 0xB0E2;
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

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_rrc_ota(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_serv_cell_info(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_rrc_mib(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_mib_metrics(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_plmn_search_req(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_plmn_search_rsp(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_initial_acq(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_serving(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_neighbors(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_meas_resp(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_search_rr(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_serv_info(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_conn_intra(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_intra_resel(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_neigh_req(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_conn_neigh(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ll1_pss(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ll1_frame_timing(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ll1_sss(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ll1_ncell_cer(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_rrc_paging(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_rrc_ca_combos(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_lte_nas(
      std::span<const uint8_t> payload);
  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_emm_state(
      std::span<const uint8_t> payload);

  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(BcchMsg& msg);
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(DlCcchMsg& msg);
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(DlDcchMsg& msg);
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(UlCcchMsg& msg);
  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(UlDcchMsg& msg);

  static constexpr std::array kLogTable = {
      LogEntry<LteParser>{LTE_RRC_OTA, &LteParser::parse_rrc_ota, "LTE RRC OTA (0xB0C0)"},
      LogEntry<LteParser>{LTE_RRC_MIB, &LteParser::parse_rrc_mib, "LTE RRC MIB (0xB0C1)"},
      LogEntry<LteParser>{LTE_SERV_CELL_INFO, &LteParser::parse_serv_cell_info,
                          "LTE Serving Cell Info (0xB0C2)"},
      LogEntry<LteParser>{LTE_PLMN_SEARCH_REQ, &LteParser::parse_plmn_search_req,
                          "LTE PLMN Search Request (0xB0C3)"},
      LogEntry<LteParser>{LTE_PLMN_SEARCH_RSP, &LteParser::parse_plmn_search_rsp,
                          "LTE PLMN Search Response (0xB0C4)"},
      LogEntry<LteParser>{LTE_RRC_PAGING, &LteParser::parse_rrc_paging, "LTE RRC Paging (0xB0CB)"},
      LogEntry<LteParser>{LTE_RRC_CA_COMBOS, &LteParser::parse_rrc_ca_combos,
                          "LTE RRC CA Combos (0xB0CD)"},
      LogEntry<LteParser>{LTE_CELL_INFO, &LteParser::parse_mib_metrics,
                          "LTE LL1/ML1 Cell Metrics (0xB175)"},
      LogEntry<LteParser>{LTE_INITIAL_ACQ, &LteParser::parse_initial_acq,
                          "LTE Initial Acquisition (0xB176)"},
      LogEntry<LteParser>{LTE_ML1_CONN_INTRA, &LteParser::parse_ml1_conn_intra,
                          "LTE ML1 Conn Intra Meas (0xB179)"},
      LogEntry<LteParser>{LTE_ML1_SERV_MEAS, &LteParser::parse_ml1_serving,
                          "LTE ML1 Serving Meas (0xB17F)"},
      LogEntry<LteParser>{LTE_ML1_NEIGH_MEAS, &LteParser::parse_ml1_neighbors,
                          "LTE ML1 Neighbor Meas (0xB180)"},
      LogEntry<LteParser>{LTE_ML1_INTRA_RESEL, &LteParser::parse_ml1_intra_resel,
                          "LTE ML1 Intra Resel (0xB181)"},
      LogEntry<LteParser>{LTE_ML1_NEIGH_REQ, &LteParser::parse_ml1_neigh_req,
                          "LTE PHY Idle Neighbor Meas (0xB192)"},
      LogEntry<LteParser>{LTE_ML1_CONN_NEIGH, &LteParser::parse_ml1_conn_neigh,
                          "LTE PHY Conn Neighbor Meas (0xB195)"},
      LogEntry<LteParser>{LTE_ML1_MEAS_RESP, &LteParser::parse_ml1_meas_resp,
                          "LTE ML1 Meas Response (0xB193)"},
      LogEntry<LteParser>{LTE_ML1_SEARCH_RR, &LteParser::parse_ml1_search_rr,
                          "LTE ML1 Search Req/Rsp (0xB194)"},
      LogEntry<LteParser>{LTE_ML1_SERV_INFO, &LteParser::parse_ml1_serv_info,
                          "LTE ML1 Serving Info (0xB197)"},
      LogEntry<LteParser>{LTE_LL1_PSS, &LteParser::parse_ll1_pss, "LTE LL1 PSS Results (0xB113)"},
      LogEntry<LteParser>{LTE_LL1_FRAME_TIMING, &LteParser::parse_ll1_frame_timing,
                          "LTE LL1 Serving Cell Frame Timing (0xB114)"},
      LogEntry<LteParser>{LTE_LL1_SSS, &LteParser::parse_ll1_sss, "LTE LL1 SSS Results (0xB115)"},
      LogEntry<LteParser>{LTE_LL1_NCELL_CER, &LteParser::parse_ll1_ncell_cer,
                          "LTE LL1 Neighbor CER (0xB123)"},
      LogEntry<LteParser>{LTE_NAS_EMM_DL, &LteParser::parse_lte_nas, "LTE NAS EMM Plain In (0xB0EC)"},
      LogEntry<LteParser>{LTE_NAS_EMM_SEC_IN, &LteParser::parse_lte_nas,
                          "LTE NAS EMM Sec In (0xB0EA)"},
      LogEntry<LteParser>{LTE_NAS_EMM_SEC_OUT, &LteParser::parse_lte_nas,
                          "LTE NAS EMM Sec Out (0xB0EB)"},
      LogEntry<LteParser>{LTE_NAS_EMM_PLAIN_OUT, &LteParser::parse_lte_nas,
                          "LTE NAS EMM Plain Out (0xB0ED)"},
      LogEntry<LteParser>{LTE_NAS_ESM_PLAIN_IN, &LteParser::parse_lte_nas,
                          "LTE NAS ESM Plain In (0xB0E2)"},
      LogEntry<LteParser>{LTE_NAS_EMM_STATE, &LteParser::parse_emm_state,
                          "LTE NAS EMM State (0xB0EE)"},
  };

private:
  [[nodiscard]] std::vector<Events::RrcEvent> extract_sib1(const asn1::rrc::sib_type1_s& sib1);
  [[nodiscard]] std::vector<Events::RrcEvent> extract_sys_info(
      const asn1::rrc::sys_info_s& sys_info);
  [[nodiscard]] static std::vector<Events::RrcEvent> decode_tai_list(const uint8_t* tai,
                                                                     size_t len);
  [[nodiscard]] static std::vector<Events::RrcEvent> decode_tai(const uint8_t* tai, size_t len);

  /// B0C0 v30+ segmented RRC reassembly (scat: segment_id 1–6 cache, 7 join).
  /// Keyed by EARFCN|PCI so concurrent cells do not corrupt each other's segments.
  struct RrcSegKey {
    uint32_t earfcn{0};
    uint16_t pci{0};
    friend bool operator==(const RrcSegKey&, const RrcSegKey&) = default;
  };
  struct RrcSegKeyHash {
    size_t operator()(const RrcSegKey& k) const noexcept {
      return (static_cast<size_t>(k.earfcn) << 16) ^ k.pci;
    }
  };
  std::unordered_map<RrcSegKey, std::array<std::vector<uint8_t>, 7>, RrcSegKeyHash> m_rrc_segments{};
};

}  // namespace QCom::Lte
