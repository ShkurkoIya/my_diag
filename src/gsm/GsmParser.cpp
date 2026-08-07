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
  // DSDS log codes prepend a 1-byte radio_id; strip and reuse single-SIM parsers.
  auto body = pkt.payload;
  LogCode code = pkt.log_code;
  switch (code) {
    case GSM_DSDS_RR_SIGNALING:
    case GSM_DSDS_CELL_INFO:
    case GSM_DSDS_SURROUND_DB:
    case GSM_DSDS_BURST_METRICS:
    case GSM_DSDS_NEIGHBOR_ACQ:
    case GSM_DSDS_SERVING_AUX:
    case GSM_DSDS_NEIGHBOR_AUX:
      if (body.size() < 1) return std::unexpected(ParserError::PacketTooShort);
      body = body.subspan(1);
      if (code == GSM_DSDS_RR_SIGNALING) code = GSM_RR_SIGNALING;
      else if (code == GSM_DSDS_CELL_INFO) code = GSM_CELL_INFO;
      else if (code == GSM_DSDS_SURROUND_DB) code = GSM_SURROUND_DB;
      else if (code == GSM_DSDS_BURST_METRICS) code = GSM_BURST_METRICS;
      else if (code == GSM_DSDS_NEIGHBOR_ACQ) code = GSM_NEIGHBOR_ACQ;
      else if (code == GSM_DSDS_SERVING_AUX) code = GSM_SERVING_AUX;
      else code = GSM_NEIGHBOR_AUX;
      break;
    default: break;
  }

  switch (code) {
    case GSM_RR_SIGNALING: return parse_rr_signaling(body);
    case GSM_CELL_INFO: return parse_cell_info(body);
    case GSM_SURROUND_DB: return parse_surround_db(body);
    case GSM_BURST_METRICS: return parse_burst_metrics(body);
    case GSM_NEW_BURST_METRICS: return parse_new_burst_metrics(body);
    case GSM_NEIGHBOR_ACQ: return parse_neighbor_acq(body);
    case GSM_SERVING_AUX: return parse_serving_aux(body);
    case GSM_NEIGHBOR_AUX: return parse_neighbor_aux(body);
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
  // Classic scat layout: chan_type[0], msg_type[1], msg_len[2], L3[3..]
  //   L3 = pseudo_len | PD | msg_type | ...
  // SM8550 / dia_vldos dump layout: chan_type[0], PD[1]=0x06, msg_type[2], body[3..]
  //   body for SI-3/6 starts at CID (no pseudo/PD/MT repeat)
  uint8_t msg_type = p[1];
  uint8_t msg_len = p[2];
  const uint8_t* l3 = p + 3;
  size_t l3_len = std::min<size_t>(msg_len, payload.size() - 3);
  bool compact_body = false;

  const bool classic_si =
      (msg_type == 0x1B || msg_type == 0x1C || msg_type == 0x1E || msg_type == 0x19 ||
       msg_type == 0x1A || msg_type == 0x1D);
  if (!classic_si && payload.size() >= 4) {
    uint8_t alt_type = p[2];
    if (alt_type == 0x1B || alt_type == 0x1C || alt_type == 0x1E || alt_type == 0x19 ||
        alt_type == 0x1A || alt_type == 0x1D) {
      msg_type = alt_type;
      l3_len = payload.size() - 3;
      compact_body = true;  // body starts at CID/LAI, not full RR L3 header
    }
  }

  // SI-3 (3GPP TS 44.018 §9.1.35)
  if (msg_type == 0x1B && l3_len >= 7) {
    if (compact_body) {
      // body: CID(2) LAI(5) CtrlChan(3) CellOpt(1) CellSel(2) RACH(3) Rest...
      std::vector<uint8_t> full(l3_len + 3);
      full[0] = 0x00;  // pseudo
      full[1] = 0x06;  // PD
      full[2] = 0x1B;
      std::memcpy(full.data() + 3, l3, l3_len);
      return parse_si3(full.data(), full.size());
    }
    if (l3_len >= 19) return parse_si3(l3, l3_len);
  }

  // SI-6 (TS 44.018 §9.1.40) — CID + LAI only
  if (msg_type == 0x1E && l3_len >= (compact_body ? 7 : 10)) {
    const uint8_t* id = compact_body ? l3 : l3 + 3;
    const uint8_t* lai_b = compact_body ? l3 + 2 : l3 + 5;
    CellPassport passport;
    passport.cell_id = static_cast<uint16_t>((id[0] << 8) | id[1]);
    auto lai = decode_lai(lai_b);
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
  if (msg_type == 0x1C && l3_len >= (compact_body ? 5 : 10)) {
    auto lai = decode_lai(compact_body ? l3 : l3 + 3);
    if (lai.mcc < 100) return std::vector<Events::RrcEvent>{};

    std::vector<Events::RrcEvent> events;

    // LAI-only passport (cell_id=0) — tracker merges onto serving without wiping CID.
    CellPassport passport;
    passport.mcc = lai.mcc;
    passport.mnc = lai.mnc;
    passport.tac = lai.lac;
    events.push_back(Events::PassportEvent{.passport = std::move(passport)});

    Events::RadioParamsEvent<GsmRadioParams> ev;
    if (!compact_body && l3_len >= 12) {
      ev.data.rxlev_access_min = l3[11] & 0x3F;
      ev.data.ms_txpwr_max_cch = l3[10] & 0x1F;
    } else if (compact_body && l3_len >= 7) {
      ev.data.ms_txpwr_max_cch = l3[5] & 0x1F;
      ev.data.rxlev_access_min = l3[6] & 0x3F;
    }
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
          rev.data.reselect_params_present = true;
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
// 0x5075 / 0x5A75 — Neighbor Cell Acquisition (ARFCN + RxPwr, BSIC usually unknown)
// Layout after optional DSDS radio_id: status(1) reserved(1) arfcn_band(2) rxpwr(2)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> GsmParser::parse_neighbor_acq(
    std::span<const uint8_t> payload) {
  if (payload.size() < 6) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint16_t arfcn = Converter::read_le<uint16_t>(p, 2) & 0x0FFF;
  if (arfcn == 0 || arfcn > 1023) return std::vector<Events::RrcEvent>{};

  int16_t rxpwr = Converter::read_le<int16_t>(p, 4);
  Events::GsmNeighborsEvent ev;
  ev.neighbors.push_back(GsmNeighborCell{
      .arfcn = arfcn,
      .bsic = 0,
      .bsic_valid = false,
      .rxlev = static_cast<int16_t>(rxpwr * 0.0625),
  });

  std::vector<Events::RrcEvent> events;
  events.push_back(std::move(ev));
  return events;
}

// ============================================================================
// 0x506A — L1 New Burst Metrics (versioned; v4 carries serving ARFCN + RxPwr)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> GsmParser::parse_new_burst_metrics(
    std::span<const uint8_t> payload) {
  if (payload.size() < 14) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint8_t version = p[0];
  if (version != 4) return std::vector<Events::RrcEvent>{};

  uint16_t arfcn = Converter::read_le<uint16_t>(p, 6) & 0x0FFF;
  int16_t rxpwr = Converter::read_le<int16_t>(p, 12);
  if (arfcn == 0 || arfcn > 1023) return std::vector<Events::RrcEvent>{};

  // Treat as a measured GSM cell (serving burst) — same upsert path as neighbors.
  // snr_est at offset 22 (int16) when payload long enough (v4 record ≥ 24).
  Events::GsmNeighborsEvent nev;
  GsmNeighborCell nc{
      .arfcn = arfcn,
      .bsic = 0,
      .bsic_valid = false,
      .rxlev = static_cast<int16_t>(rxpwr * 0.0625),
  };
  nev.neighbors.push_back(nc);

  std::vector<Events::RrcEvent> events;
  events.push_back(std::move(nev));

  if (payload.size() >= 24) {
    int16_t snr_est = Converter::read_le<int16_t>(p, 22);
    CellSignal sig;
    GsmSignalParams gp;
    gp.rxlev = static_cast<int8_t>(rxpwr * 0.0625);
    gp.snr = snr_est;
    gp.has_snr = true;
    sig.signal_data = gp;
    events.push_back(Events::SignalUpdateEvent{.signal = std::move(sig)});
  }
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
