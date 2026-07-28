/// @file LteQcomBinary.cpp
/// @brief Qualcomm proprietary binary ML1 log parsing for LTE.
///
/// All formats here are reverse-engineered from scat/QCSuper/dia_vldos.
/// Layouts are version-dependent — unknown versions are silently skipped (fail-closed).
#include <cstdint>

#include "lte/LteParser.h"

namespace QCom::Lte {

using Utils::bits;
using Utils::Converter;
using Utils::ml1_rsrp;
using Utils::ml1_rsrq;
using Utils::ml1_rssi;
using Utils::valid_lte_earfcn;
using Utils::valid_lte_pci;
using Utils::valid_lte_rsrp;

// ============================================================================
// 0xB0C2 — Serving Cell Info (proprietary Qualcomm identity packet)
// ============================================================================
// Qualcomm duplicates SIB1 identity into this fixed-layout packet.
// Arrives faster than SIB1 ASN.1 decode — useful as primary identity source.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_serv_cell_info(
    std::string_view payload) {
  if (payload.size() < 2) return std::unexpected(ParserError::PacketTooShort);

  auto p = reinterpret_cast<const uint8_t*>(payload.data());
  uint8_t version = p[0];
  const uint8_t* body = p + 1;
  size_t body_len = payload.size() - 1;

  CellPassport passport;
  LteRadioParams radio;

  if (version == 2 && body_len >= 24) {
    radio.pci = Converter::read_le<uint16_t>(body, 0);
    radio.earfcn = Converter::read_le<uint16_t>(body, 2);
    radio.dl_bw = body[6];
    radio.ul_bw = body[7];
    passport.cell_id = Converter::read_le<uint32_t>(body, 8);
    passport.tac = Converter::read_le<uint16_t>(body, 12);
    radio.freq_band_ind = static_cast<uint8_t>(Converter::read_le<uint32_t>(body, 14));
    passport.mcc = Converter::read_le<uint16_t>(body, 18);
    passport.mnc = Converter::read_le<uint16_t>(body, 21);
    radio.ul_earfcn = Converter::read_le<uint16_t>(body, 4);
  } else if (version == 3 && body_len >= 28) {
    radio.pci = Converter::read_le<uint16_t>(body, 0);
    radio.earfcn = Converter::read_le<uint32_t>(body, 2);
    radio.dl_bw = body[10];
    radio.ul_bw = body[11];
    passport.cell_id = Converter::read_le<uint32_t>(body, 12);
    passport.tac = Converter::read_le<uint16_t>(body, 16);
    radio.freq_band_ind = static_cast<uint8_t>(Converter::read_le<uint32_t>(body, 18));
    passport.mcc = Converter::read_le<uint16_t>(body, 22);
    passport.mnc = Converter::read_le<uint16_t>(body, 25);
    radio.ul_earfcn = Converter::read_le<uint32_t>(body, 6);
  } else {
    return std::vector<Events::RrcEvent>{};
  }

  if (!valid_lte_pci(radio.pci) || !valid_lte_earfcn(radio.earfcn)) {
    return std::vector<Events::RrcEvent>{};
  }

  std::vector<Events::RrcEvent> events;
  events.push_back(Events::PassportEvent{.passport = std::move(passport)});

  Events::RadioParamsEvent<LteRadioParams> rev;
  rev.data = radio;
  events.push_back(Events::RrcEvent{std::move(rev)});
  events.push_back(Events::ServingChangedEvent{.is_serving = true});

  return events;
}

// ============================================================================
// 0xB17F — ML1 Serving Cell Meas & Eval
// ============================================================================
// Contains serving cell RSRP, RSRQ, RSSI with bitfield extraction.
// Version 4: 16-bit EARFCN. Version 5: 32-bit EARFCN.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ml1_serving(
    std::string_view payload) {
  if (payload.size() < 20) return std::unexpected(ParserError::PacketTooShort);

  auto p = reinterpret_cast<const uint8_t*>(payload.data());
  uint8_t version = p[0];

  uint32_t earfcn = 0;
  uint16_t pci = 0;
  uint32_t rsrp_raw = 0, rsrq_raw = 0, rssi_raw = 0;

  if (version == 4 && payload.size() >= 24) {
    earfcn = Converter::read_le<uint16_t>(p, 4);
    uint16_t pci_slp = Converter::read_le<uint16_t>(p, 6);
    pci = (pci_slp >> 7) & 0x1FF;
    rsrp_raw = Converter::read_le<uint32_t>(p, 8) & 0xFFF;
    rsrq_raw = Converter::read_le<uint32_t>(p, 16) >> 22;
    rssi_raw = (Converter::read_le<uint32_t>(p, 20) >> 11) & 0x7FF;
  } else if (version == 5 && payload.size() >= 28) {
    earfcn = Converter::read_le<uint32_t>(p, 4);
    uint16_t pci_slp = Converter::read_le<uint16_t>(p, 8);
    pci = (pci_slp >> 7) & 0x1FF;
    rsrp_raw = Converter::read_le<uint32_t>(p, 12) & 0xFFF;
    rsrq_raw = Converter::read_le<uint32_t>(p, 20) >> 22;
    rssi_raw = (Converter::read_le<uint32_t>(p, 24) >> 11) & 0x7FF;
  } else {
    return std::vector<Events::RrcEvent>{};
  }

  float rsrp = ml1_rsrp(rsrp_raw);
  float rsrq = ml1_rsrq(rsrq_raw);
  float rssi = ml1_rssi(rssi_raw);

  if (!valid_lte_rsrp(rsrp)) return std::vector<Events::RrcEvent>{};

  CellSignal sig;
  sig.signal_data = LteSignalParams{.rsrp = rsrp, .rsrq = rsrq, .rssi = rssi};

  std::vector<Events::RrcEvent> events;
  events.push_back(Events::SignalUpdateEvent{.signal = std::move(sig)});
  events.push_back(Events::ServingChangedEvent{.is_serving = true});
  return events;
}

// ============================================================================
// 0xB180 — ML1 Neighbor Measurements
// ============================================================================
// Per-cell stride: 32 bytes. Up to 16 neighbors.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ml1_neighbors(
    std::string_view payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);

  auto p = reinterpret_cast<const uint8_t*>(payload.data());
  uint8_t version = p[0];

  uint32_t earfcn = 0;
  uint32_t n_cells = 0;
  size_t cell_start = 0;

  if (version == 4 && payload.size() >= 8) {
    earfcn = Converter::read_le<uint16_t>(p, 4);
    n_cells = Converter::read_le<uint16_t>(p, 6) >> 6;
    cell_start = 8;
  } else if (version == 5 && payload.size() >= 12) {
    earfcn = Converter::read_le<uint32_t>(p, 4);
    n_cells = Converter::read_le<uint32_t>(p, 8) >> 6;
    cell_start = 12;
  } else {
    return std::vector<Events::RrcEvent>{};
  }

  if (n_cells > 16) n_cells = 16;

  Events::NeighborMeasEvent nev;
  constexpr size_t CELL_STRIDE = 32;

  for (uint32_t i = 0; i < n_cells; ++i) {
    size_t off = cell_start + CELL_STRIDE * i;
    if (off + 12 > payload.size()) break;

    uint32_t val0 = Converter::read_le<uint32_t>(p, off);
    uint32_t val2 = Converter::read_le<uint32_t>(p, off + 8);

    uint16_t pci = static_cast<uint16_t>((val0 >> 23) & 0x1FF);
    uint32_t rsrp_raw = val0 & 0xFFF;
    uint32_t rsrq_raw = (val2 >> 12) & 0x3FF;

    float rsrp = ml1_rsrp(rsrp_raw);
    if (!valid_lte_pci(pci) || !valid_lte_rsrp(rsrp)) continue;

    nev.neighbors.push_back(NeighborMeasResult{
        .pci = pci,
        .rsrp = static_cast<uint8_t>(rsrp_raw >> 4),
        .rsrq = static_cast<uint8_t>(rsrq_raw >> 2),
    });
  }

  if (nev.neighbors.empty()) return std::vector<Events::RrcEvent>{};

  std::vector<Events::RrcEvent> events;
  events.push_back(std::move(nev));
  return events;
}

// ============================================================================
// 0xB193 — ML1 Serving Cell Meas Response (subpacket-based)
// ============================================================================
// Container: pkt_version=1, num_subpkts, subpackets with id/ver/size headers.
// Only process subpacket id=0x19.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ml1_meas_resp(
    std::string_view payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);

  auto p = reinterpret_cast<const uint8_t*>(payload.data());
  uint8_t pkt_ver = p[0];
  if (pkt_ver != 1) return std::vector<Events::RrcEvent>{};

  uint8_t num_subpkts = p[1];
  size_t pos = 4;

  std::vector<Events::RrcEvent> events;

  for (uint8_t sp = 0; sp < num_subpkts && pos + 4 <= payload.size(); ++sp) {
    uint8_t sp_id = p[pos];
    uint8_t sp_ver = p[pos + 1];
    uint16_t sp_size = Converter::read_le<uint16_t>(p, pos + 2);

    if (sp_size < 4 || pos + sp_size > payload.size()) break;

    if (sp_id == 0x19) {
      const uint8_t* body = p + pos + 4;
      size_t body_len = sp_size - 4;

      if (body_len < 8) {
        pos += sp_size;
        continue;
      }

      uint32_t earfcn = Converter::read_le<uint32_t>(body, 0);
      uint16_t num_cells = Converter::read_le<uint16_t>(body, 4);

      size_t cell_start = 0;
      size_t cell_stride = 0;

      if (sp_ver == 36) {
        cell_start = 8;
        cell_stride = 128;
      } else if (sp_ver == 48 || sp_ver == 50) {
        cell_start = 12;
        cell_stride = 140;
      } else {
        pos += sp_size;
        continue;
      }

      if (num_cells > 8) num_cells = 8;

      for (uint16_t c = 0; c < num_cells; ++c) {
        size_t cell_off = cell_start + cell_stride * c;
        if (cell_off + 64 > body_len) break;

        uint16_t val0 = Converter::read_le<uint16_t>(body, cell_off);
        uint16_t pci = (val0 >> 7) & 0x1FF;
        bool is_serving = (val0 >> 3) & 1;

        if (!valid_lte_pci(pci)) continue;

        // RevWordBits: 12 LE words at cell_off+16
        // RSRP at bit slice [108:120], RSRQ at [224:234], RSSI at [320:331]
        // Simplified: read the raw words directly
        if (cell_off + 16 + 48 > body_len) continue;

        const uint8_t* words_base = body + cell_off + 16;
        // Word 3 (offset 12) contains RSRP bits at its high end
        uint32_t w3 = Converter::read_le<uint32_t>(words_base, 12);
        uint32_t rsrp_raw = (w3 >> 12) & 0xFFF;
        // Word 7 (offset 28) contains RSRQ
        uint32_t w7 = Converter::read_le<uint32_t>(words_base, 28);
        uint32_t rsrq_raw = w7 & 0x3FF;

        float rsrp = ml1_rsrp(rsrp_raw);
        if (!valid_lte_rsrp(rsrp)) continue;

        CellSignal sig;
        sig.signal_data = LteSignalParams{
            .rsrp = rsrp,
            .rsrq = ml1_rsrq(rsrq_raw),
        };

        events.push_back(Events::SignalUpdateEvent{.signal = std::move(sig)});
        if (is_serving) { events.push_back(Events::ServingChangedEvent{.is_serving = true}); }
      }
    }

    pos += sp_size;
  }

  return events;
}

// ============================================================================
// 0xB197 — ML1 Serving Cell Information (identity anchor)
// ============================================================================
// Confirms serving EARFCN + PCI. No signal data.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ml1_serv_info(
    std::string_view payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);

  auto p = reinterpret_cast<const uint8_t*>(payload.data());
  uint8_t version = p[0];

  uint32_t earfcn = 0;
  uint16_t pci = 0;

  if (version == 1 && payload.size() >= 8) {
    earfcn = Converter::read_le<uint16_t>(p, 4);
    uint16_t pci_word = Converter::read_le<uint16_t>(p, 6);
    pci = ((pci_word & 0xFFFF) >> 7) & 0x1FF;
  } else if (version == 2 && payload.size() >= 12) {
    earfcn = Converter::read_le<uint32_t>(p, 4);
    uint32_t pci_word = Converter::read_le<uint32_t>(p, 8);
    pci = ((pci_word & 0xFFFF) >> 7) & 0x1FF;
  } else {
    return std::vector<Events::RrcEvent>{};
  }

  if (!valid_lte_pci(pci) || !valid_lte_earfcn(earfcn)) { return std::vector<Events::RrcEvent>{}; }

  std::vector<Events::RrcEvent> events;
  events.push_back(Events::ServingChangedEvent{.is_serving = true});
  return events;
}

}  // namespace QCom::Lte
