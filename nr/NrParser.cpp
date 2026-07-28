/// @file NrParser.cpp
/// @brief NR DIAG parser — SIB1 (srsRAN ASN.1) + proprietary Qualcomm ML1 binary.
#include "NrParser.h"

#include <cstdint>

namespace QCom::Nr {

using Utils::bits;
using Utils::Converter;
using Utils::ml1_nr_sinr;
using Utils::ml1_rsrp;
using Utils::ml1_rsrq;
using Utils::valid_nr_arfcn;
using Utils::valid_nr_pci;

// ============================================================================
// 0xB97F — NR ML1 Searcher Meas Database Update
// ============================================================================
// Container: {minor_ver, major_ver, num_subpkts, reserved}
// Per subpacket: {id, ver, size(u16)} + data
// Per CC: nrarfcn(24 bits), num_cells(4 bits)
// Per cell: PCI(10 bits), num_beams(4 bits), then beams of 8 bytes each.

std::expected<std::vector<Events::RrcEvent>, ParserError> NrParser::parse_ml1_metrics(
    std::string_view payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);

  auto p = reinterpret_cast<const uint8_t*>(payload.data());
  uint8_t num_subpkts = p[2];
  size_t pos = 4;

  std::vector<Events::RrcEvent> events;

  for (uint8_t sp = 0; sp < num_subpkts && pos + 4 <= payload.size(); ++sp) {
    uint16_t sp_size = Converter::read_le<uint16_t>(p, pos + 2);
    if (sp_size < 4 || pos + sp_size > payload.size()) break;

    const uint8_t* sp_data = p + pos + 4;
    size_t sp_data_sz = sp_size - 4;

    if (sp_data_sz < 8) {
      pos += sp_size;
      continue;
    }

    uint32_t nrarfcn = bits(Converter::read_le<uint32_t>(sp_data, 0), 0, 24);
    uint32_t num_cells = bits(Converter::read_le<uint32_t>(sp_data, 4), 0, 4);

    if (!valid_nr_arfcn(nrarfcn)) {
      pos += sp_size;
      continue;
    }
    if (num_cells > 16) num_cells = 16;

    Events::NeighborMeasEvent nev;
    size_t cell_off = 8;

    for (uint32_t c = 0; c < num_cells && cell_off + 4 <= sp_data_sz; ++c) {
      uint32_t cw0 = Converter::read_le<uint32_t>(sp_data, cell_off);
      uint16_t pci = static_cast<uint16_t>(bits(cw0, 0, 10));
      uint32_t num_beams = bits(cw0, 10, 4);
      cell_off += 4;

      if (!valid_nr_pci(pci)) {
        cell_off += num_beams * 8;
        continue;
      }

      float best_rsrp = -999.0f;
      float best_rsrq = 0.0f;
      float best_sinr = 0.0f;

      for (uint32_t b = 0; b < num_beams && cell_off + 8 <= sp_data_sz; ++b) {
        uint32_t bw0 = Converter::read_le<uint32_t>(sp_data, cell_off);
        uint32_t bw1 = Converter::read_le<uint32_t>(sp_data, cell_off + 4);

        float rsrp = ml1_rsrp(bits(bw0, 10, 12));
        float rsrq = ml1_rsrq(bits(bw1, 0, 12));
        float sinr = ml1_nr_sinr(bits(bw1, 12, 10));

        if (rsrp > best_rsrp) {
          best_rsrp = rsrp;
          best_rsrq = rsrq;
          best_sinr = sinr;
        }
        cell_off += 8;
      }

      if (best_rsrp > -180.0f) {
        nev.neighbors.push_back(NeighborMeasResult{
            .pci = pci,
            .rsrp = static_cast<uint8_t>(static_cast<int>(best_rsrp + 180.0f) & 0xFF),
            .rsrq = static_cast<uint8_t>(static_cast<int>(best_rsrq + 30.0f) & 0xFF),
        });
      }
    }

    if (!nev.neighbors.empty()) events.push_back(std::move(nev));
    pos += sp_size;
  }

  return events;
}

// ============================================================================
// 0xB992 — NR ML1 Serving Cell Measurement
// ============================================================================
// Same container as 0xB97F. Per subpacket: NRARFCN, PCI, RSRP/RSRQ/SINR.

std::expected<std::vector<Events::RrcEvent>, ParserError> NrParser::parse_ml1_serving(
    std::string_view payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);

  auto p = reinterpret_cast<const uint8_t*>(payload.data());
  uint8_t num_subpkts = p[2];
  size_t pos = 4;

  std::vector<Events::RrcEvent> events;

  for (uint8_t sp = 0; sp < num_subpkts && pos + 4 <= payload.size(); ++sp) {
    uint16_t sp_size = Converter::read_le<uint16_t>(p, pos + 2);
    if (sp_size < 4 || pos + sp_size > payload.size()) break;

    const uint8_t* sp_data = p + pos + 4;
    size_t sp_data_sz = sp_size - 4;

    if (sp_data_sz < 20) {
      pos += sp_size;
      continue;
    }

    uint32_t nrarfcn = bits(Converter::read_le<uint32_t>(sp_data, 0), 0, 24);
    uint16_t pci = static_cast<uint16_t>(bits(Converter::read_le<uint32_t>(sp_data, 4), 0, 10));

    if (!valid_nr_arfcn(nrarfcn) || !valid_nr_pci(pci)) {
      pos += sp_size;
      continue;
    }

    uint32_t rsrp_raw = bits(Converter::read_le<uint32_t>(sp_data, 12), 0, 12);
    float rsrp = ml1_rsrp(rsrp_raw);

    NrSignalParams sig_params{.ss_rsrp = rsrp};

    if (sp_data_sz >= 24) {
      uint32_t w4 = Converter::read_le<uint32_t>(sp_data, 16);
      sig_params.ss_rsrq = ml1_rsrq(bits(w4, 0, 12));
      sig_params.ss_sinr = ml1_nr_sinr(bits(w4, 12, 10));
    }

    CellSignal sig;
    sig.signal_data = sig_params;

    events.push_back(Events::SignalUpdateEvent{.signal = std::move(sig)});
    events.push_back(Events::ServingChangedEvent{.is_serving = true});

    pos += sp_size;
  }

  return events;
}

// ============================================================================
// ASN.1 channel handlers
// ============================================================================

std::vector<Events::RrcEvent> NrParser::on_message_unpacked(BcchMsg& msg) {
  const auto msg_type = msg.msg.c1().type().value;
  if (msg_type == BcchMsgTypes::sib_type1) return extract_sib1(msg.msg.c1().sib_type1());
  return {};
}

std::vector<Events::RrcEvent> NrParser::on_message_unpacked(DlCcchMsg&) { return {}; }
std::vector<Events::RrcEvent> NrParser::on_message_unpacked(DlDcchMsg&) { return {}; }
std::vector<Events::RrcEvent> NrParser::on_message_unpacked(UlCcchMsg&) { return {}; }
std::vector<Events::RrcEvent> NrParser::on_message_unpacked(UlDcchMsg&) { return {}; }

// ============================================================================
// NR SIB1 — Cell identity (PLMN/TAC/NCI/RANAC)
// ============================================================================

std::vector<Events::RrcEvent> NrParser::extract_sib1(const asn1::rrc_nr::sib1_s& sib1) {
  std::vector<Events::RrcEvent> events;

  const auto& plmn_list = sib1.cell_access_related_info.plmn_id_list;
  if (plmn_list.size() == 0) return events;

  const auto& first = plmn_list[0];
  CellPassport passport;

  if (first.tac_present) passport.tac = static_cast<uint32_t>(first.tac.to_number());
  passport.cell_id = static_cast<uint32_t>(first.cell_id.to_number());

  if (first.plmn_id_list.size() > 0) {
    const auto& plmn = first.plmn_id_list[0];
    if (plmn.mcc_present) passport.mcc = Utils::Converter::digits_to_number(plmn.mcc);
    passport.mnc = Utils::Converter::digits_to_number(plmn.mnc);
  }

  events.push_back(Events::PassportEvent{.passport = std::move(passport)});

  Events::RadioParamsEvent<NrRadioParams> rev;
  if (first.ranac_present) rev.data.ranac = first.ranac;
  if (sib1.cell_sel_info_present) {
    rev.data.q_rx_lev_min = static_cast<int8_t>(sib1.cell_sel_info.q_rx_lev_min * 2);
    if (sib1.cell_sel_info.q_qual_min_present) {
      rev.data.q_qual_min = static_cast<int8_t>(sib1.cell_sel_info.q_qual_min);
    }
  }
  events.push_back(Events::RrcEvent{std::move(rev)});

  return events;
}

}  // namespace QCom::Nr
