/// @file WcdmaParser.cpp
/// @brief WCDMA DIAG binary parser — Cell ID, Resel Rank, Active Set, Serving Cell, RRC OTA hdr.
#include "wcdma/WcdmaParser.h"

#include <algorithm>
#include <cstring>

#include "gsm/GsmParser.h"  // for decode_lai

namespace QCom::Wcdma {

using Utils::Converter;

namespace {

[[nodiscard]] int16_t real_rscp(int8_t raw) noexcept {
  return static_cast<int16_t>(std::clamp(static_cast<int>(raw) - 21, -140, 0));
}

[[nodiscard]] int16_t real_ecio(int8_t raw) noexcept {
  // scat: Ec/Io = raw / 2 (already half-dB units → dB)
  return static_cast<int16_t>(std::clamp(static_cast<int>(raw) / 2, -50, 0));
}

[[nodiscard]] const char* rrc_channel_name(uint8_t ch) noexcept {
  switch (ch) {
    case 0: return "UL_CCCH";
    case 1: return "UL_DCCH";
    case 2: return "DL_CCCH";
    case 3: return "DL_DCCH";
    case 4: return "BCCH_BCH";
    case 5: return "BCCH_FACH";
    case 6: return "PCCH";
    case 7: return "MCCH";
    case 8: return "MSCH";
    case 9: return "BCCH_BCH_ext";
    case 10: return "SI_Container";
    case 0xFE: return "BCCH_BCH_decoded";
    case 0xFF: return "BCCH_FACH_decoded";
    default: return "UNKNOWN";
  }
}

}  // namespace

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
    case WCDMA_RRC_OTA: return parse_rrc_ota(pkt.payload);
    case UMTS_NAS_OTA: return parse_umts_nas(pkt.payload);
    default: return std::unexpected(ParserError::WrongLogCode);
  }
}

// ============================================================================
// Shared identity decode — scat '<LL LH BB H 3s 3s LL'
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> WcdmaParser::parse_identity_common(
    std::span<const uint8_t> payload, bool psc_shift4) {
  if (payload.size() < 32) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint32_t ul_uarfcn = Converter::read_le<uint32_t>(p, 0);
  uint32_t dl_uarfcn = Converter::read_le<uint32_t>(p, 4);
  uint32_t cell_id = Converter::read_le<uint32_t>(p, 8) & 0x0FFFFFFFu;
  uint16_t ura_id = Converter::read_le<uint16_t>(p, 12);
  uint8_t flags = p[14];
  uint8_t access = p[15];
  uint16_t psc_raw = Converter::read_le<uint16_t>(p, 16);
  uint16_t psc = psc_shift4 ? static_cast<uint16_t>(psc_raw >> 4) : psc_raw;
  uint16_t mcc = bcd3_to_int(p + 18);
  uint16_t mnc = bcd3_to_int(p + 21);
  // LAC / RAC are LE uint32 in the 32-byte layout; use low 16 bits.
  uint32_t lac = Converter::read_le<uint32_t>(p, 24) & 0xFFFFu;
  uint32_t rac = Converter::read_le<uint32_t>(p, 28) & 0xFFFFu;

  if (mcc == 0 || mcc > 999 || mnc > 999 || psc > 511 || dl_uarfcn == 0 || dl_uarfcn > 16383) {
    return std::vector<Events::RrcEvent>{};
  }

  std::vector<Events::RrcEvent> events;

  CellPassport passport;
  passport.cell_id = cell_id;
  passport.mcc = mcc;
  passport.mnc = mnc;
  passport.tac = static_cast<uint16_t>(lac);
  passport.rac = static_cast<uint16_t>(rac);
  // Digits: BCD stop nibble → 2 or 3
  {
    uint8_t digs = 0;
    for (int i = 0; i < 3; ++i) {
      if ((p[21 + i] & 0x0F) == 0x0F) break;
      ++digs;
    }
    if (digs == 2 || digs == 3) passport.mnc_digits = digs;
  }
  events.push_back(Events::PassportEvent{.passport = std::move(passport)});

  Events::RadioParamsEvent<WcdmaRadioParams> rev;
  rev.data.dl_uarfcn = dl_uarfcn;
  rev.data.ul_uarfcn = ul_uarfcn;
  rev.data.psc = psc;
  rev.data.ura_id = ura_id;
  rev.data.flags = flags;
  rev.data.access = access;
  events.push_back(Events::RrcEvent{std::move(rev)});

  events.push_back(Events::ServingChangedEvent{.is_serving = true});
  return events;
}

std::expected<std::vector<Events::RrcEvent>, ParserError> WcdmaParser::parse_cell_id(
    std::span<const uint8_t> payload) {
  return parse_identity_common(payload, /*psc_shift4=*/true);
}

std::expected<std::vector<Events::RrcEvent>, ParserError> WcdmaParser::parse_serv_cell(
    std::span<const uint8_t> payload) {
  return parse_identity_common(payload, /*psc_shift4=*/false);
}

// ============================================================================
// 0x4005 — Search Cell Reselection Rank (serving + 3G neigh + optional 2G)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> WcdmaParser::parse_resel_rank(
    std::span<const uint8_t> payload) {
  if (payload.size() < 4) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint8_t ver = (p[0] >> 6) & 0x03;
  uint8_t num_3g = p[0] & 0x3F;
  uint8_t num_2g = p[1] & 0x3F;

  size_t start_pos;
  size_t stride_3g;
  size_t stride_2g;

  switch (ver) {
    case 0:
      start_pos = 2;
      stride_3g = 10;
      stride_2g = 7;
      break;
    case 1:
      start_pos = 2;
      stride_3g = 11;
      stride_2g = 8;
      break;
    case 2:
      start_pos = 7;
      stride_3g = 16;
      stride_2g = 13;
      break;
    default: return std::vector<Events::RrcEvent>{};
  }

  if (num_3g > 16) num_3g = 16;
  if (num_2g > 16) num_2g = 16;

  std::vector<Events::RrcEvent> events;
  Events::WcdmaNeighborsEvent nev;
  Events::GsmNeighborsEvent gev;

  bool first_is_serving = true;

  for (uint8_t i = 0; i < num_3g; ++i) {
    size_t off = start_pos + stride_3g * i;
    if (off + 8 > payload.size()) break;

    uint16_t uarfcn = Converter::read_le<uint16_t>(p, off);
    uint16_t psc = Converter::read_le<uint16_t>(p, off + 2);
    int8_t rscp_raw = static_cast<int8_t>(p[off + 4]);
    int8_t ecio_raw = static_cast<int8_t>(p[off + 7]);

    int16_t rscp = real_rscp(rscp_raw);
    int16_t ecio = real_ecio(ecio_raw);

    if (psc > 511 || uarfcn == 0 || uarfcn > 16383) continue;

    if (first_is_serving) {
      Events::RadioParamsEvent<WcdmaRadioParams> radio;
      radio.data.dl_uarfcn = uarfcn;
      radio.data.psc = psc;
      events.push_back(Events::RrcEvent{std::move(radio)});

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

  size_t gsm_off = start_pos + stride_3g * num_3g;
  for (uint8_t i = 0; i < num_2g; ++i) {
    size_t off = gsm_off + stride_2g * i;
    if (off + 5 > payload.size()) break;
    uint16_t arfcn_raw = Converter::read_le<uint16_t>(p, off);
    uint16_t arfcn = arfcn_raw & 0x0FFF;
    uint8_t bsic = p[off + 2];
    int8_t rssi_raw = static_cast<int8_t>(p[off + 3]);
    if (arfcn == 0 || arfcn > 1023) continue;
    GsmNeighborCell g;
    g.arfcn = arfcn;
    g.bsic = bsic;
    g.bsic_valid = true;
    g.rxlev = static_cast<int16_t>(rssi_raw);  // already dBm-ish in scat dumps
    gev.neighbors.push_back(g);
  }

  if (!nev.neighbors.empty()) events.push_back(std::move(nev));
  if (!gev.neighbors.empty()) events.push_back(std::move(gev));
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
  std::vector<Events::RrcEvent> events;

  if (uarfcn > 0 && uarfcn <= 16383) {
    Events::RadioParamsEvent<WcdmaRadioParams> radio;
    radio.data.dl_uarfcn = uarfcn;
    events.push_back(Events::RrcEvent{std::move(radio)});
  }

  for (uint8_t i = 0; i < num_cells; ++i) {
    size_t off = 4 + 7 * i;
    if (off + 2 > payload.size()) break;
    uint16_t psc = Converter::read_le<uint16_t>(p, off);
    if (psc > 511) continue;
    ev.neighbors.push_back(WcdmaNeighborCell{.uarfcn = uarfcn, .psc = psc});
  }

  if (!ev.neighbors.empty()) events.push_back(std::move(ev));
  return events;
}

// ============================================================================
// 0x412F — WCDMA RRC OTA (header only; no UMTS ASN.1 in tree)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> WcdmaParser::parse_rrc_ota(
    std::span<const uint8_t> payload) {
  if (payload.size() < 4) return std::unexpected(ParserError::PacketTooShort);

  uint8_t channel_type = payload[0];
  uint8_t rbid = payload[1];
  uint16_t len = Converter::read_le<uint16_t>(payload.data(), 2);

  Events::RadioParamsEvent<WcdmaRadioParams> radio;
  radio.data.last_rrc_channel = channel_type;
  radio.data.last_rrc_len = len;
  (void)rbid;
  (void)rrc_channel_name(channel_type);

  std::vector<Events::RrcEvent> events;
  events.push_back(Events::RrcEvent{std::move(radio)});
  return events;
}

// ============================================================================
// 0x713A — UMTS NAS OTA (GMM/MM/SM messages)
// ============================================================================

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

    size_t rai_off = (msg_type == 0x02) ? 0 : 2;
    if (rai_off + 6 > body_len) return std::vector<Events::RrcEvent>{};

    auto lai = Gsm::decode_lai(body + rai_off);
    if (lai.mcc < 100 || lai.mcc > 999) return std::vector<Events::RrcEvent>{};

    CellPassport passport;
    passport.mcc = lai.mcc;
    passport.mnc = lai.mnc;
    passport.tac = lai.lac;
    passport.rac = body[rai_off + 5];  // RAC follows LAC in RAI

    std::vector<Events::RrcEvent> events;
    events.push_back(Events::PassportEvent{.passport = std::move(passport)});
    return events;
  }

  return std::vector<Events::RrcEvent>{};
}

}  // namespace QCom::Wcdma
