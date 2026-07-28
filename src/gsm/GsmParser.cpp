/// @file GsmParser.cpp
/// @brief GSM DIAG binary parser — SI-3, Cell Info, Burst Metrics, Surround DB.
#include "gsm/GsmParser.h"

#include <algorithm>
#include <cstring>

namespace QCom::Gsm {

using Utils::Converter;

// ============================================================================
// Main dispatch
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> GsmParser::parse(QualcommPacketView pkt) {
  switch (pkt.log_code) {
    case GSM_RR_SIGNALING: return parse_rr_signaling(pkt.payload);
    case GSM_CELL_INFO: return parse_cell_info(pkt.payload);
    case GSM_SURROUND_DB: return parse_surround_db(pkt.payload);
    case GSM_BURST_METRICS: return parse_burst_metrics(pkt.payload);
    case GSM_SERVING_AUX: return parse_serving_aux(pkt.payload);
    case GSM_NEIGHBOR_AUX: return parse_neighbor_aux(pkt.payload);
    default: return std::unexpected(ParserError::WrongLogCode);
  }
}

// ============================================================================
// 0x512F — RR Signaling (contains SI-1 through SI-6)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> GsmParser::parse_rr_signaling(
    std::span<const uint8_t> payload) {
  if (payload.size() < 6) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  // GsmRrHeader: chan_type_dir[0], msg_type[1], msg_len[2]
  uint8_t msg_type = p[1];
  uint8_t msg_len = p[2];
  size_t l3_len = std::min<size_t>(msg_len, payload.size() - 3);
  const uint8_t* l3 = p + 3;

  // SI-3 (3GPP TS 44.018 §9.1.35)
  if (msg_type == 0x1B && l3_len >= 19) { return parse_si3(l3, l3_len); }

  // SI-6 (TS 44.018 §9.1.40) — CID + LAI only
  if (msg_type == 0x1E && l3_len >= 10) {
    CellPassport passport;
    passport.cell_id = static_cast<uint16_t>((l3[3] << 8) | l3[4]);
    auto lai = decode_lai(l3 + 5);
    passport.mcc = lai.mcc;
    passport.mnc = lai.mnc;
    passport.tac = lai.lac;

    if (passport.mcc < 100 || passport.mcc > 999 || passport.cell_id == 0) {
      return std::vector<Events::RrcEvent>{};
    }

    std::vector<Events::RrcEvent> events;
    events.push_back(Events::PassportEvent{.passport = std::move(passport)});
    return events;
  }

  // SI-4 (TS 44.018 §9.1.36) — LAI + Cell Selection Params, no CID
  if (msg_type == 0x1C && l3_len >= 10) {
    auto lai = decode_lai(l3 + 3);
    if (lai.mcc < 100) return std::vector<Events::RrcEvent>{};

    Events::RadioParamsEvent<GsmRadioParams> ev;
    if (l3_len >= 12) {
      ev.data.rxlev_access_min = l3[11] & 0x3F;
      ev.data.ms_txpwr_max_cch = l3[10] & 0x1F;
    }

    std::vector<Events::RrcEvent> events;
    events.push_back(Events::RrcEvent{std::move(ev)});
    return events;
  }

  return std::vector<Events::RrcEvent>{};
}

// ============================================================================
// SI-3 full parser: CID, LAI, Cell Selection, Rest Octets (CSN.1)
// ============================================================================

std::vector<Events::RrcEvent> GsmParser::parse_si3(const uint8_t* l3, size_t len) {
  std::vector<Events::RrcEvent> events;

  // l3[0]=pseudo_length, l3[1]=PD, l3[2]=msg_type, l3[3..4]=CID, l3[5..9]=LAI
  CellPassport passport;
  passport.cell_id = static_cast<uint16_t>((l3[3] << 8) | l3[4]);
  auto lai = decode_lai(l3 + 5);
  passport.mcc = lai.mcc;
  passport.mnc = lai.mnc;
  passport.tac = lai.lac;

  if (passport.mcc < 100 || passport.mcc > 999 || passport.cell_id == 0) return events;

  events.push_back(Events::PassportEvent{.passport = std::move(passport)});

  // Cell Selection Parameters at l3[14..15] (TS 44.018 §10.5.2.4)
  if (len >= 16) {
    Events::RadioParamsEvent<GsmRadioParams> rev;
    rev.data.ms_txpwr_max_cch = l3[14] & 0x1F;
    rev.data.rxlev_access_min = l3[15] & 0x3F;
    rev.data.ncc_permitted = (len >= 19) ? l3[18] : 0xFF;

    // SI-3 Rest Octets start at l3[19] — CSN.1 encoding
    if (len > 19) {
      BitReader bits(l3 + 19, len - 19);
      // Selection Parameters presence flag
      if (!bits.eof() && bits.get(1) == 1) {
        if (!bits.eof(15)) {
          bits.get(1);  // CBQ (skip)
          rev.data.cell_reselect_offset = static_cast<uint8_t>(bits.get(6));
          rev.data.temporary_offset = static_cast<uint8_t>(bits.get(3));
          rev.data.penalty_time = static_cast<uint8_t>(bits.get(5));
        }
      }
    }

    events.push_back(Events::RrcEvent{std::move(rev)});
  }

  return events;
}

// ============================================================================
// 0x5134 — Cell Info (serving cell identity + ARFCN/BSIC)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> GsmParser::parse_cell_info(
    std::span<const uint8_t> payload) {
  if (payload.size() < 13) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint16_t arfcn_band = Converter::read_le<uint16_t>(p, 0);
  uint16_t arfcn = arfcn_band & 0x0FFF;
  uint8_t bcc = p[2];
  uint8_t ncc = p[3];
  uint16_t cid = Converter::read_le<uint16_t>(p, 4);
  auto lai = decode_lai(p + 6);

  if (arfcn > 1023) return std::vector<Events::RrcEvent>{};

  std::vector<Events::RrcEvent> events;

  CellPassport passport;
  passport.cell_id = cid;
  passport.mcc = lai.mcc;
  passport.mnc = lai.mnc;
  passport.tac = lai.lac;

  if (passport.mcc >= 100 && passport.mcc <= 999 && cid != 0) {
    events.push_back(Events::PassportEvent{.passport = std::move(passport)});
  }

  Events::RadioParamsEvent<GsmRadioParams> rev;
  rev.data.arfcn = arfcn;
  rev.data.bsic = static_cast<uint16_t>(((ncc & 0x07) << 3) | (bcc & 0x07));
  rev.data.ncc = ncc & 0x07;
  rev.data.bcc = bcc & 0x07;
  rev.data.ncc_permitted = (payload.size() >= 13) ? p[12] : 0xFF;
  events.push_back(Events::RrcEvent{std::move(rev)});

  events.push_back(Events::ServingChangedEvent{.is_serving = true});
  return events;
}

// ============================================================================
// 0x5071 — Surround Cell BA List (neighbor cells)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> GsmParser::parse_surround_db(
    std::span<const uint8_t> payload) {
  if (payload.size() < 1) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint8_t num_cells = p[0];
  if (num_cells > 64) num_cells = 64;

  constexpr size_t ENTRY_SIZE = 12;
  Events::GsmNeighborsEvent ev;

  for (uint8_t i = 0; i < num_cells; ++i) {
    size_t off = 1 + ENTRY_SIZE * i;
    if (off + ENTRY_SIZE > payload.size()) break;

    uint16_t arfcn_band = Converter::read_le<uint16_t>(p, off);
    int16_t rxpwr = Converter::read_le<int16_t>(p, off + 2);
    uint8_t bsic_valid = p[off + 4];
    uint8_t bsic = p[off + 5];

    uint16_t arfcn = arfcn_band & 0x0FFF;
    if (arfcn > 1023) continue;

    ev.neighbors.push_back(GsmNeighborCell{
        .arfcn = arfcn,
        .bsic = (bsic_valid == 1) ? bsic : uint8_t{0xFF},
        .bsic_valid = (bsic_valid == 1),
        .rxlev = static_cast<int16_t>(rxpwr * 0.0625),
    });
  }

  std::vector<Events::RrcEvent> events;
  if (!ev.neighbors.empty()) events.push_back(std::move(ev));
  return events;
}

// ============================================================================
// 0x506C — Burst Metrics (serving cell signal)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> GsmParser::parse_burst_metrics(
    std::span<const uint8_t> payload) {
  if (payload.size() < 13) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  int16_t rxpwr = Converter::read_le<int16_t>(p, 11);

  if (rxpwr == 0) return std::vector<Events::RrcEvent>{};

  CellSignal sig;
  sig.signal_data = GsmSignalParams{
      .rxlev = static_cast<int8_t>(rxpwr * 0.0625),
  };

  std::vector<Events::RrcEvent> events;
  events.push_back(Events::SignalUpdateEvent{.signal = std::move(sig)});
  return events;
}

// ============================================================================
// 0x507A — Serving Auxiliary Measurements
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> GsmParser::parse_serving_aux(
    std::span<const uint8_t> payload) {
  if (payload.size() < 3) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  int16_t rxpwr = Converter::read_le<int16_t>(p, 0);

  CellSignal sig;
  sig.signal_data = GsmSignalParams{
      .rxlev = static_cast<int8_t>(rxpwr * 0.0625),
  };

  std::vector<Events::RrcEvent> events;
  events.push_back(Events::SignalUpdateEvent{.signal = std::move(sig)});
  return events;
}

// ============================================================================
// 0x507B — Neighbor Auxiliary Measurements
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> GsmParser::parse_neighbor_aux(
    std::span<const uint8_t> payload) {
  if (payload.size() < 1) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint8_t num_cells = p[0];
  if (num_cells > 64) num_cells = 64;

  Events::GsmNeighborsEvent ev;

  for (uint8_t i = 0; i < num_cells; ++i) {
    size_t off = 1 + 4 * i;
    if (off + 4 > payload.size()) break;

    uint16_t arfcn_band = Converter::read_le<uint16_t>(p, off);
    int16_t rxpwr = Converter::read_le<int16_t>(p, off + 2);
    uint16_t arfcn = arfcn_band & 0x0FFF;

    if (arfcn > 1023) continue;

    ev.neighbors.push_back(GsmNeighborCell{
        .arfcn = arfcn,
        .rxlev = static_cast<int16_t>(rxpwr * 0.0625),
    });
  }

  std::vector<Events::RrcEvent> events;
  if (!ev.neighbors.empty()) events.push_back(std::move(ev));
  return events;
}

}  // namespace QCom::Gsm
