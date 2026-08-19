/// @file NrParser.cpp
/// @brief NR DIAG parser — SIB1 (srsRAN ASN.1) + proprietary Qualcomm ML1/RRC binary.
#include "nr/NrParser.h"

#include <cstdint>

namespace QCom::Nr {

using Utils::bits;
using Utils::Converter;
using Utils::ml1_nr_sinr;
using Utils::ml1_rsrp;
using Utils::ml1_rsrq;
using Utils::valid_nr_arfcn;
using Utils::valid_nr_pci;

namespace {

[[nodiscard]] uint8_t nr_scs_khz(uint32_t scs_code) noexcept {
  switch (scs_code) {
    case 0: return 15;
    case 1: return 30;
    case 2: return 60;
    case 3: return 120;
    default: return 0;
  }
}

/// Props bitfield: bit index 0 = LSB of byte0 (SFN = bits[0:10] == u16&0x3FF).
[[nodiscard]] uint32_t le_bits_slice(const uint8_t* p, size_t nbytes, int a, int b) noexcept {
  uint32_t v = 0;
  for (int i = a; i < b; ++i) {
    const int by = i >> 3;
    const int bit = i & 7;
    if (static_cast<size_t>(by) >= nbytes) break;
    if ((p[by] >> bit) & 1u) v |= (1u << (i - a));
  }
  return v;
}

}  // namespace

// ============================================================================
// 0xB822 — NR RRC MIB Info (scat)
// ============================================================================
// Header: rel_min u16, rel_maj u16. Then PCI u16, NRARFCN u32, props bits.

std::expected<std::vector<Events::RrcEvent>, ParserError> NrParser::parse_rrc_mib(
    std::span<const uint8_t> payload) {
  if (payload.size() < 14) return std::unexpected(ParserError::PacketTooShort);

  const uint16_t rel_min = Converter::read_le<uint16_t>(payload.data(), 0);
  const uint16_t rel_maj = Converter::read_le<uint16_t>(payload.data(), 2);
  const uint16_t pci = Converter::read_le<uint16_t>(payload.data(), 4);
  const uint32_t nrarfcn = Converter::read_le<uint32_t>(payload.data(), 6);
  if (!valid_nr_pci(pci) || !valid_nr_arfcn(nrarfcn) || nrarfcn == 0) {
    return std::vector<Events::RrcEvent>{};
  }

  Events::RadioParamsEvent<NrRadioParams> rev;
  rev.data.pci = pci;
  rev.data.nrarfcn = nrarfcn;

  if (rel_maj == 0x00 && rel_min == 0x03 && payload.size() >= 14) {
    rev.data.sfn = static_cast<uint16_t>(le_bits_slice(payload.data() + 10, 4, 0, 10));
    rev.data.scs_khz = nr_scs_khz(le_bits_slice(payload.data() + 10, 4, 30, 32));
  } else if (rel_maj == 0x02 && rel_min == 0x00 && payload.size() >= 15) {
    rev.data.sfn = static_cast<uint16_t>(le_bits_slice(payload.data() + 10, 5, 0, 10));
    rev.data.scs_khz = nr_scs_khz(le_bits_slice(payload.data() + 10, 5, 31, 33));
  } else {
    // Unknown version — still mint RADIO with PCI|ARFCN.
  }

  return std::vector<Events::RrcEvent>{Events::RrcEvent{std::move(rev)}};
}

// ============================================================================
// 0xB823 — NR RRC Serving Cell Info (scat)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> NrParser::parse_serv_cell_info(
    std::span<const uint8_t> payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);

  const uint16_t rel_min = Converter::read_le<uint16_t>(payload.data(), 0);
  const uint16_t rel_maj = Converter::read_le<uint16_t>(payload.data(), 2);

  uint16_t pci = 0;
  uint32_t dl_arfcn = 0, ul_arfcn = 0;
  uint16_t dl_bw = 0, ul_bw = 0;
  uint64_t cell_id = 0;
  uint16_t mcc = 0, mnc = 0;
  uint8_t mnc_digit = 0;
  uint8_t allowed = 0;
  uint32_t tac = 0;
  uint16_t band = 0;

  auto p = payload.data();
  if (rel_maj == 0x00 && rel_min == 0x04 && payload.size() >= 38) {
    // pci H, dl L, ul L, dl_bw H, ul_bw H, cell Q, mcc H, mnc_digit B, mnc H, allowed B, tac L, band H
    pci = Converter::read_le<uint16_t>(p, 4);
    dl_arfcn = Converter::read_le<uint32_t>(p, 6);
    ul_arfcn = Converter::read_le<uint32_t>(p, 10);
    dl_bw = Converter::read_le<uint16_t>(p, 14);
    ul_bw = Converter::read_le<uint16_t>(p, 16);
    cell_id = Converter::read_le<uint64_t>(p, 18);
    mcc = Converter::read_le<uint16_t>(p, 26);
    mnc_digit = p[28];
    mnc = Converter::read_le<uint16_t>(p, 29);
    allowed = p[31];
    tac = Converter::read_le<uint32_t>(p, 32);
    band = Converter::read_le<uint16_t>(p, 36);
  } else if (rel_maj == 0x03 && rel_min == 0x00 && payload.size() >= 46) {
    // pci H, cgi Q, dl L, ul L, dl_bw H, ul_bw H, cell Q, mcc H, digit B, mnc H, allowed B, tac L, band H
    pci = Converter::read_le<uint16_t>(p, 4);
    cell_id = Converter::read_le<uint64_t>(p, 6);  // nr_cgi; also cell_id field below
    dl_arfcn = Converter::read_le<uint32_t>(p, 14);
    ul_arfcn = Converter::read_le<uint32_t>(p, 18);
    dl_bw = Converter::read_le<uint16_t>(p, 22);
    ul_bw = Converter::read_le<uint16_t>(p, 24);
    const uint64_t cell_alt = Converter::read_le<uint64_t>(p, 26);
    if (cell_alt != 0) cell_id = cell_alt;
    mcc = Converter::read_le<uint16_t>(p, 34);
    mnc_digit = p[36];
    mnc = Converter::read_le<uint16_t>(p, 37);
    allowed = p[39];
    tac = Converter::read_le<uint32_t>(p, 40);
    band = Converter::read_le<uint16_t>(p, 44);
  } else if (rel_maj == 0x03 && (rel_min == 0x02 || rel_min == 0x03) && payload.size() >= 49) {
    // 3-byte prefix then same as 3.0
    pci = Converter::read_le<uint16_t>(p, 7);
    cell_id = Converter::read_le<uint64_t>(p, 9);
    dl_arfcn = Converter::read_le<uint32_t>(p, 17);
    ul_arfcn = Converter::read_le<uint32_t>(p, 21);
    dl_bw = Converter::read_le<uint16_t>(p, 25);
    ul_bw = Converter::read_le<uint16_t>(p, 27);
    const uint64_t cell_alt = Converter::read_le<uint64_t>(p, 29);
    if (cell_alt != 0) cell_id = cell_alt;
    mcc = Converter::read_le<uint16_t>(p, 37);
    mnc_digit = p[39];
    mnc = Converter::read_le<uint16_t>(p, 40);
    allowed = p[42];
    tac = Converter::read_le<uint32_t>(p, 43);
    band = Converter::read_le<uint16_t>(p, 47);
  } else {
    return std::vector<Events::RrcEvent>{};
  }

  if (!valid_nr_pci(pci) || !valid_nr_arfcn(dl_arfcn) || dl_arfcn == 0) {
    return std::vector<Events::RrcEvent>{};
  }

  std::vector<Events::RrcEvent> events;
  Events::RadioParamsEvent<NrRadioParams> rev;
  rev.data.pci = pci;
  rev.data.nrarfcn = dl_arfcn;
  rev.data.ul_nrarfcn = ul_arfcn;
  rev.data.dl_bw = static_cast<uint8_t>(dl_bw > 255 ? 255 : dl_bw);
  rev.data.ul_bw = static_cast<uint8_t>(ul_bw > 255 ? 255 : ul_bw);
  rev.data.band = band;
  rev.data.allowed_access = static_cast<int8_t>(allowed ? 1 : 0);
  events.push_back(Events::RrcEvent{std::move(rev)});

  const bool have_id = cell_id != 0 && tac != 0 && mcc >= 100 && mcc <= 999;
  if (have_id) {
    CellPassport pass;
    pass.cell_id = cell_id;
    pass.tac = tac;
    pass.mcc = mcc;
    pass.mnc = mnc;
    if (mnc_digit == 2 || mnc_digit == 3) pass.mnc_digits = mnc_digit;
    events.push_back(Events::PassportEvent{.passport = std::move(pass)});
  }
  return events;
}

// ============================================================================
// 0xB97F — NR ML1 Searcher Meas Database Update
// ============================================================================
// Container: {minor_ver, major_ver, num_subpkts, reserved}
// Per subpacket: {id, ver, size(u16)} + data
// Per CC: nrarfcn(24 bits), num_cells(4 bits)
// Per cell: PCI(10 bits), num_beams(4 bits), then beams of 8 bytes each.

std::expected<std::vector<Events::RrcEvent>, ParserError> NrParser::parse_ml1_metrics(
    std::span<const uint8_t> payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
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
        NeighborMeasResult nr;
        nr.pci = pci;
        nr.rsrp_dbm = best_rsrp;
        nr.has_rsrp = true;
        nr.rsrq_db = best_rsrq;
        nr.has_rsrq = true;
        nr.sinr_db = best_sinr;
        nr.has_sinr = true;
        nev.neighbors.push_back(nr);
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
    std::span<const uint8_t> payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
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
  passport.cell_id = static_cast<uint64_t>(first.cell_id.to_number());

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
