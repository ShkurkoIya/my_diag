/// @file WcdmaParser.cpp
/// @brief WCDMA DIAG binary parser — Cell ID, Resel Rank, Active Set, Serving Cell.
#include "wcdma/WcdmaParser.h"

#include <algorithm>
#include <cstring>

#include "gsm/GsmParser.h"  // for decode_lai

namespace QCom::Wcdma {

using Utils::Converter;

// ============================================================================
// Main dispatch
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> WcdmaParser::parse(
    QualcommPacketView pkt) {
  switch (pkt.log_code) {
    case WCDMA_CELL_ID: return parse_cell_id(pkt.payload);
    case WCDMA_RESEL_RANK: return parse_resel_rank(pkt.payload);
    case WCDMA_ACTIVE_SET: return parse_active_set(pkt.payload);
    case WCDMA_SERV_CELL: return parse_serv_cell(pkt.payload);
    case WCDMA_RRC_OTA: return std::vector<Events::RrcEvent>{};
    case UMTS_NAS_OTA: return parse_umts_nas(pkt.payload);
    default: return std::unexpected(ParserError::WrongLogCode);
  }
}

// ============================================================================
// 0x4027 — Cell ID (serving cell identity)
// ============================================================================
// Scat layout: '<LL LH BB H 3s 3s LL'
// 32 bytes minimum.

std::expected<std::vector<Events::RrcEvent>, ParserError> WcdmaParser::parse_cell_id(
    std::span<const uint8_t> payload) {
  if (payload.size() < 32) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint32_t ul_uarfcn = Converter::read_le<uint32_t>(p, 0);
  uint32_t dl_uarfcn = Converter::read_le<uint32_t>(p, 4);
  uint32_t cell_id = Converter::read_le<uint32_t>(p, 8) & 0x0FFFFFFFu;
  uint16_t psc_raw = Converter::read_le<uint16_t>(p, 16);
  uint16_t psc = psc_raw >> 4;
  uint16_t mcc = bcd3_to_int(p + 18);
  uint16_t mnc = bcd3_to_int(p + 21);
  uint32_t lac = Converter::read_le<uint32_t>(p, 24);

  if (mcc == 0 || mcc > 999 || mnc > 999 || psc > 511) { return std::vector<Events::RrcEvent>{}; }

  std::vector<Events::RrcEvent> events;

  CellPassport passport;
  passport.cell_id = cell_id;
  passport.mcc = mcc;
  passport.mnc = mnc;
  passport.tac = static_cast<uint16_t>(lac & 0xFFFF);
  events.push_back(Events::PassportEvent{.passport = std::move(passport)});

  Events::RadioParamsEvent<WcdmaRadioParams> rev;
  rev.data.dl_uarfcn = dl_uarfcn;
  rev.data.ul_uarfcn = ul_uarfcn;
  rev.data.psc = psc;
  events.push_back(Events::RrcEvent{std::move(rev)});

  events.push_back(Events::ServingChangedEvent{.is_serving = true});
  return events;
}

// ============================================================================
// 0x4005 — Search Cell Reselection Rank (serving + neighbors with RSCP/EcNo)
// ============================================================================
// Version in bits[7:6] of byte 0. Versions 0, 1, 2 supported.

std::expected<std::vector<Events::RrcEvent>, ParserError> WcdmaParser::parse_resel_rank(
    std::span<const uint8_t> payload) {
  if (payload.size() < 4) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint8_t ver = (p[0] >> 6) & 0x03;
  uint8_t num_3g = p[0] & 0x3F;

  size_t start_pos;
  size_t stride_3g;

  switch (ver) {
    case 0:
      start_pos = 2;
      stride_3g = 10;
      break;
    case 1:
      start_pos = 2;
      stride_3g = 11;
      break;
    case 2:
      start_pos = 7;
      stride_3g = 16;
      break;
    default: return std::vector<Events::RrcEvent>{};
  }

  if (num_3g > 16) num_3g = 16;

  std::vector<Events::RrcEvent> events;
  Events::WcdmaNeighborsEvent nev;

  bool first_is_serving = true;

  for (uint8_t i = 0; i < num_3g; ++i) {
    size_t off = start_pos + stride_3g * i;
    if (off + 8 > payload.size()) break;

    uint16_t uarfcn = Converter::read_le<uint16_t>(p, off);
    uint16_t psc = Converter::read_le<uint16_t>(p, off + 2);
    int8_t rscp_raw = static_cast<int8_t>(p[off + 4]);
    int8_t ecio_raw = static_cast<int8_t>(p[off + 7]);

    int16_t rscp = static_cast<int16_t>(std::clamp(static_cast<int>(rscp_raw) - 21, -140, 0));
    int16_t ecio = static_cast<int16_t>(std::clamp(static_cast<int>(ecio_raw) / 2, -50, 0));

    if (psc > 511) continue;

    if (first_is_serving) {
      CellSignal sig;
      sig.signal_data = WcdmaSignalParams{
          .rscp = static_cast<float>(rscp),
          .ecio = static_cast<float>(ecio),
          .has_ecio = true,
      };
      events.push_back(Events::SignalUpdateEvent{.signal = std::move(sig)});
      events.push_back(Events::ServingChangedEvent{.is_serving = true});
      first_is_serving = false;
    } else {
      nev.neighbors.push_back(WcdmaNeighborCell{
          .uarfcn = uarfcn,
          .psc = psc,
          .rscp = rscp,
          .ecio = ecio,
      });
    }
  }

  if (!nev.neighbors.empty()) events.push_back(std::move(nev));
  return events;
}

// ============================================================================
// 0x4111 — Active Set (PSC list in soft handover active set)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> WcdmaParser::parse_active_set(
    std::span<const uint8_t> payload) {
  if (payload.size() < 4) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint16_t uarfcn = Converter::read_le<uint16_t>(p, 1);
  uint8_t num_cells = p[3];
  if (num_cells > 16) num_cells = 16;

  Events::WcdmaNeighborsEvent ev;

  for (uint8_t i = 0; i < num_cells; ++i) {
    size_t off = 4 + 7 * i;
    if (off + 2 > payload.size()) break;
    uint16_t psc = Converter::read_le<uint16_t>(p, off);
    if (psc > 511) continue;
    ev.neighbors.push_back(WcdmaNeighborCell{.uarfcn = uarfcn, .psc = psc});
  }

  std::vector<Events::RrcEvent> events;
  if (!ev.neighbors.empty()) events.push_back(std::move(ev));
  return events;
}

// ============================================================================
// 0x4127 — Serving Cell Info (identity, same layout as 0x4027 but PSC has no >>4)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> WcdmaParser::parse_serv_cell(
    std::span<const uint8_t> payload) {
  if (payload.size() < 32) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint32_t ul_uarfcn = Converter::read_le<uint32_t>(p, 0);
  uint32_t dl_uarfcn = Converter::read_le<uint32_t>(p, 4);
  uint32_t cell_id = Converter::read_le<uint32_t>(p, 8) & 0x0FFFFFFFu;
  uint16_t psc = Converter::read_le<uint16_t>(p, 16);
  uint16_t mcc = bcd3_to_int(p + 18);
  uint16_t mnc = bcd3_to_int(p + 21);
  uint32_t lac = Converter::read_le<uint32_t>(p, 24);

  if (mcc == 0 || mcc > 999 || mnc > 999 || psc > 511) { return std::vector<Events::RrcEvent>{}; }

  std::vector<Events::RrcEvent> events;

  CellPassport passport;
  passport.cell_id = cell_id;
  passport.mcc = mcc;
  passport.mnc = mnc;
  passport.tac = static_cast<uint16_t>(lac & 0xFFFF);
  events.push_back(Events::PassportEvent{.passport = std::move(passport)});

  Events::RadioParamsEvent<WcdmaRadioParams> rev;
  rev.data.dl_uarfcn = dl_uarfcn;
  rev.data.ul_uarfcn = ul_uarfcn;
  rev.data.psc = psc;
  events.push_back(Events::RrcEvent{std::move(rev)});

  events.push_back(Events::ServingChangedEvent{.is_serving = true});
  return events;
}

// ============================================================================
// 0x713A — UMTS NAS OTA (GMM/MM/SM messages)
// ============================================================================
// Header: direction[1], nas_hdr_len[1], msg_len[4]
// NAS PDU at offset 6 + nas_hdr_len
// GMM Attach Accept (PD=0x08, type=0x02): RAI at body[0..5] = PLMN(3 BCD) + LAC(2) + RAC(1)
// RAU Accept (PD=0x08, type=0x09): RAI at body[2..7]

std::expected<std::vector<Events::RrcEvent>, ParserError> WcdmaParser::parse_umts_nas(
    std::span<const uint8_t> payload) {
  if (payload.size() < 10) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint8_t nas_hdr_len = p[1];
  size_t pdu_off = 6 + nas_hdr_len;
  if (pdu_off + 2 > payload.size()) return std::vector<Events::RrcEvent>{};

  const uint8_t* pdu = p + pdu_off;
  size_t pdu_len = payload.size() - pdu_off;

  uint8_t pd = pdu[0] & 0x0F;
  uint8_t msg_type = pdu[1];

  // GMM Attach Accept (0x02) or RAU Accept (0x09)
  if (pd == 0x08 && (msg_type == 0x02 || msg_type == 0x09)) {
    const uint8_t* body = pdu + 2;
    size_t body_len = pdu_len - 2;

    // RAI offset: Attach Accept at body[0], RAU Accept at body[2]
    size_t rai_off = (msg_type == 0x02) ? 0 : 2;
    if (rai_off + 6 > body_len) return std::vector<Events::RrcEvent>{};

    // RAI = PLMN(3 BCD) + LAC(2 BE) + RAC(1)
    auto lai = Gsm::decode_lai(body + rai_off);
    if (lai.mcc < 100 || lai.mcc > 999) return std::vector<Events::RrcEvent>{};

    CellPassport passport;
    passport.mcc = lai.mcc;
    passport.mnc = lai.mnc;
    passport.tac = lai.lac;

    std::vector<Events::RrcEvent> events;
    events.push_back(Events::PassportEvent{.passport = std::move(passport)});
    return events;
  }

  return std::vector<Events::RrcEvent>{};
}

}  // namespace QCom::Wcdma
