/// @file LteQcomBinary.cpp
/// @brief Qualcomm proprietary binary ML1 log parsing for LTE.
///
/// All formats here are reverse-engineered from scat/QCSuper/dia_vldos
/// and SIM8300 live dumps (fail-closed on unknown versions).
#include <cstdint>
#include <optional>
#include <set>

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

namespace {

[[nodiscard]] uint8_t rb_or_mhz_to_mhz(uint8_t v) noexcept {
  // B0C2 may carry RB count (6..100) or already-MHz (1..20).
  switch (v) {
    case 6: return 1;
    case 15: return 3;
    case 25: return 5;
    case 50: return 10;
    case 75: return 15;
    case 100: return 20;
    default: return v;
  }
}

/// scat/dia_vldos RevWordBits: reverse LE word order, lay BE, index MSB-first.
struct RevWordBits {
  uint8_t be[48]{};
  int nbytes{0};

  RevWordBits(const uint8_t* le_words, int nwords) {
    if (nwords > 12) nwords = 12;
    nbytes = nwords * 4;
    for (int i = 0; i < nwords; ++i) {
      uint32_t w = Converter::read_le<uint32_t>(le_words, static_cast<size_t>(nwords - 1 - i) * 4);
      be[i * 4 + 0] = static_cast<uint8_t>((w >> 24) & 0xFF);
      be[i * 4 + 1] = static_cast<uint8_t>((w >> 16) & 0xFF);
      be[i * 4 + 2] = static_cast<uint8_t>((w >> 8) & 0xFF);
      be[i * 4 + 3] = static_cast<uint8_t>(w & 0xFF);
    }
  }

  [[nodiscard]] uint32_t slice(int a, int b) const {
    uint32_t v = 0;
    for (int i = a; i < b; ++i) {
      const int by = i >> 3;
      const int bit = 7 - (i & 7);
      if (by >= nbytes) break;
      v = (v << 1) | static_cast<uint32_t>((be[by] >> bit) & 1u);
    }
    return v;
  }
};

}  // namespace

// ============================================================================
// 0xB0C2 — Serving Cell Info (proprietary Qualcomm identity packet)
// ============================================================================
// Qualcomm duplicates SIB1 identity into this fixed-layout packet.
// Arrives faster than SIB1 ASN.1 decode — useful as primary identity source.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_serv_cell_info(
    std::span<const uint8_t> payload) {
  if (payload.size() < 2) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint8_t version = p[0];
  const uint8_t* body = p + 1;
  size_t body_len = payload.size() - 1;

  CellPassport passport;
  LteRadioParams radio;

  if (version == 2 && body_len >= 24) {
    radio.pci = Converter::read_le<uint16_t>(body, 0);
    radio.earfcn = Converter::read_le<uint16_t>(body, 2);
    radio.dl_bw = rb_or_mhz_to_mhz(body[6]);
    radio.ul_bw = rb_or_mhz_to_mhz(body[7]);
    passport.cell_id = Converter::read_le<uint32_t>(body, 8);
    passport.tac = Converter::read_le<uint16_t>(body, 12);
    radio.freq_band_ind = static_cast<uint8_t>(Converter::read_le<uint32_t>(body, 14));
    passport.mcc = Converter::read_le<uint16_t>(body, 18);
    passport.mnc = Converter::read_le<uint16_t>(body, 21);
    radio.ul_earfcn = Converter::read_le<uint16_t>(body, 4);
  } else if (version == 3 && body_len >= 28) {
    radio.pci = Converter::read_le<uint16_t>(body, 0);
    radio.earfcn = Converter::read_le<uint32_t>(body, 2);
    radio.dl_bw = rb_or_mhz_to_mhz(body[10]);
    radio.ul_bw = rb_or_mhz_to_mhz(body[11]);
    passport.cell_id = Converter::read_le<uint32_t>(body, 12);
    passport.tac = Converter::read_le<uint16_t>(body, 16);
    radio.freq_band_ind = static_cast<uint8_t>(Converter::read_le<uint32_t>(body, 18));
    passport.mcc = Converter::read_le<uint16_t>(body, 22);
    passport.mnc = Converter::read_le<uint16_t>(body, 25);
    radio.ul_earfcn = Converter::read_le<uint32_t>(body, 6);
  } else {
    return std::vector<Events::RrcEvent>{};
  }

  if (!valid_lte_pci(radio.pci) || radio.pci == 0 || !valid_lte_earfcn(radio.earfcn)) {
    return std::vector<Events::RrcEvent>{};
  }
  // Reject empty / all-ones identity (modem padding during reselection).
  if (!Utils::valid_lte_eci(passport.cell_id) || !Utils::valid_lte_tac(passport.tac)) {
    return std::vector<Events::RrcEvent>{};
  }
  if (passport.mcc < 100 || passport.mcc > 999) return std::vector<Events::RrcEvent>{};

  std::vector<Events::RrcEvent> events;
  events.push_back(Events::PassportEvent{.passport = std::move(passport)});

  Events::RadioParamsEvent<LteRadioParams> rev;
  rev.data = radio;
  events.push_back(Events::RrcEvent{std::move(rev)});
  // Do NOT emit ServingChanged here: during PLMN/COPS sweep B0C2 fires for
  // every briefly-camped cell; thrashing is_serving hides the real QMI camp.
  // Passports still accumulate on EARFCN+PCI rows (dia_vldos serv_cells_ model).

  return events;
}

// ============================================================================
// 0xB0C1 — Dedicated RRC MIB log (proprietary binary, not ASN.1)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_rrc_mib(
    std::span<const uint8_t> payload) {
  if (payload.size() < 2) return std::unexpected(ParserError::PacketTooShort);

  const uint8_t ver = payload[0];
  const uint8_t* p = payload.data() + 1;
  const size_t body_len = payload.size() - 1;

  auto rb_count_to_mhz = [](uint8_t rb) -> uint8_t {
    switch (rb) {
      case 6: return 1;
      case 15: return 3;
      case 25: return 5;
      case 50: return 10;
      case 75: return 15;
      case 100: return 20;
      default: return 0;
    }
  };

  LteRadioParams radio;
  if (ver == 1 && body_len >= 8) {
    radio.pci = Converter::read_le<uint16_t>(p, 0);
    radio.earfcn = Converter::read_le<uint16_t>(p, 2);
    radio.sfn = Converter::read_le<uint16_t>(p, 4);
    radio.dl_bw = rb_count_to_mhz(p[7]);
  } else if (ver == 2 && body_len >= 10) {
    radio.pci = Converter::read_le<uint16_t>(p, 0);
    radio.earfcn = Converter::read_le<uint32_t>(p, 2);
    radio.sfn = Converter::read_le<uint16_t>(p, 6);
    radio.dl_bw = rb_count_to_mhz(p[9]);
  } else {
    return std::vector<Events::RrcEvent>{};
  }

  if (!valid_lte_pci(radio.pci) || !valid_lte_earfcn(radio.earfcn)) {
    return std::vector<Events::RrcEvent>{};
  }

  std::vector<Events::RrcEvent> events;
  Events::RadioParamsEvent<LteRadioParams> rev;
  rev.data = radio;
  events.push_back(Events::RrcEvent{std::move(rev)});
  return events;
}

// ============================================================================
// 0xB0C3 — LTE RRC PLMN Search Request (no cell payload)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_plmn_search_req(
    std::span<const uint8_t> payload) {
  if (payload.empty()) return std::unexpected(ParserError::PacketTooShort);
  return std::vector<Events::RrcEvent>{};
}

// ============================================================================
// 0xB0C4 — LTE RRC PLMN Search Response
// ============================================================================
// Captured on SIM8300 (v2): header then 8-byte entries
//   [type:u8][PLMN:3 BCD 24.008][u32 earfcn_or_0]
// type 0x02 = PLMN only, type 0x03 = PLMN + EARFCN. Fail-closed on other versions.

namespace {

[[nodiscard]] bool decode_plmn_24_008(const uint8_t* p, CellPassport& out) noexcept {
  const uint8_t d1 = p[0] & 0x0F, d2 = (p[0] >> 4) & 0x0F, d3 = p[1] & 0x0F;
  const uint8_t m3 = (p[1] >> 4) & 0x0F, m1 = p[2] & 0x0F, m2 = (p[2] >> 4) & 0x0F;
  if (d1 > 9 || d2 > 9 || d3 > 9) return false;
  out.mcc = static_cast<uint16_t>(d1 * 100 + d2 * 10 + d3);
  if (m3 == 0x0F) {
    if (m1 > 9 || m2 > 9) return false;
    out.mnc = static_cast<uint16_t>(m1 * 10 + m2);
  } else {
    if (m1 > 9 || m2 > 9 || m3 > 9) return false;
    out.mnc = static_cast<uint16_t>(m1 * 100 + m2 * 10 + m3);
  }
  return out.mcc >= 100 && out.mcc <= 999;
}

}  // namespace

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_plmn_search_rsp(
    std::span<const uint8_t> payload) {
  if (payload.size() < 16) return std::unexpected(ParserError::PacketTooShort);
  if (payload[0] != 2) return std::vector<Events::RrcEvent>{};  // only v2 verified

  std::vector<Events::RrcEvent> events;
  // Entries start at offset 8 (after version + 7-byte header).
  for (size_t off = 8; off + 8 <= payload.size(); off += 8) {
    const uint8_t type = payload[off];
    if (type != 0x02 && type != 0x03) break;

    CellPassport passport;
    if (!decode_plmn_24_008(payload.data() + off + 1, passport)) break;

    const uint32_t earfcn = Converter::read_le<uint32_t>(payload.data() + off, 4);
    if (type == 0x03 && valid_lte_earfcn(earfcn)) {
      LteRadioParams radio;
      radio.earfcn = earfcn;
      // PCI unknown during PLMN search — open EARFCN|0 row (same as GSM surround).
      Events::RadioParamsEvent<LteRadioParams> rev;
      rev.data = radio;
      events.push_back(Events::RrcEvent{std::move(rev)});
      events.push_back(Events::PassportEvent{.passport = std::move(passport)});
    }
    // type 0x02: PLMN without EARFCN — nothing to key a cell row on.
  }
  return events;
}

// ============================================================================
// 0xB176 — LTE Initial Acquisition Results
// ============================================================================
// Live SIM8300 sample: EARFCN @+4, PCI @+20 (u16). Version/status dword varies.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_initial_acq(
    std::span<const uint8_t> payload) {
  if (payload.size() < 22) return std::unexpected(ParserError::PacketTooShort);

  const uint32_t earfcn = Converter::read_le<uint32_t>(payload.data(), 4);
  const uint16_t pci = Converter::read_le<uint16_t>(payload.data(), 20);
  if (!valid_lte_earfcn(earfcn) || !valid_lte_pci(pci) || pci == 0) {
    return std::vector<Events::RrcEvent>{};
  }

  LteRadioParams radio;
  radio.earfcn = earfcn;
  radio.pci = pci;
  Events::RadioParamsEvent<LteRadioParams> rev;
  rev.data = radio;
  return std::vector<Events::RrcEvent>{Events::RrcEvent{std::move(rev)}};
}

// ============================================================================
// 0xB194 — LTE ML1 Search Request / Response (subpacket container)
// ============================================================================
// WirelessMetrix: subpkt 0x1C = request, 0x1D = response.
// SIM8300: 0x1D header may not sit at payload[4] — scan for id/ver/size,
// then try verified body layouts (EARFCN+PCI), fail-closed.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ml1_search_rr(
    std::span<const uint8_t> payload) {
  if (payload.size() < 12) return std::unexpected(ParserError::PacketTooShort);
  if (payload[0] != 1) return std::vector<Events::RrcEvent>{};

  auto try_body = [](const uint8_t* body, size_t body_len) -> std::optional<LocalCellKey> {
    const std::pair<size_t, size_t> layouts[] = {{16, 28}, {16, 32}, {20, 36}};
    for (auto [eo, po] : layouts) {
      if (body_len < po + 4) continue;
      uint32_t earfcn = Converter::read_le<uint32_t>(body, eo);
      uint32_t pci_u = Converter::read_le<uint32_t>(body, po);
      if (!valid_lte_earfcn(earfcn) || pci_u < 1 || pci_u > 503) continue;
      return LocalCellKey{.freq = earfcn, .pci_bsic = static_cast<uint16_t>(pci_u)};
    }
    return std::nullopt;
  };

  std::vector<Events::RrcEvent> events;
  for (size_t i = 2; i + 8 < payload.size(); ++i) {
    if (payload[i] != 0x1D) continue;
    const uint8_t sp_ver = payload[i + 1];
    if (sp_ver < 0x20 || sp_ver > 0x40) continue;
    const uint16_t sp_size = Converter::read_le<uint16_t>(payload.data(), i + 2);
    const size_t rem = payload.size() - i;
    // Qualcomm size field is often short by a few bytes vs USB pad — use min.
    size_t use = rem;
    if (sp_size >= 8 && sp_size <= rem) use = sp_size;
    if (use < 8) continue;
    if (auto key = try_body(payload.data() + i + 4, use - 4)) {
      Events::RadioParamsEvent<LteRadioParams> rev;
      rev.data.earfcn = key->freq;
      rev.data.pci = key->pci_bsic;
      events.push_back(Events::RrcEvent{std::move(rev)});
      break;
    }
  }
  return events;
}

// ============================================================================
// 0xB17F — ML1 Serving Cell Meas & Eval
// ============================================================================
// Contains serving cell RSRP, RSRQ, RSSI with bitfield extraction.
// Version 4: 16-bit EARFCN. Version 5: 32-bit EARFCN.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ml1_serving(
    std::span<const uint8_t> payload) {
  if (payload.size() < 20) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
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
  sig.signal_data = LteSignalParams{.rsrp = rsrp, .rsrq = rsrq, .rssi = rssi, .has_rssi = true};

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
    std::span<const uint8_t> payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
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

    NeighborMeasResult nr;
    nr.pci = pci;
    nr.rsrp_dbm = rsrp;
    nr.has_rsrp = true;
    nr.rsrq_db = ml1_rsrq(rsrq_raw);
    nr.has_rsrq = true;
    nev.neighbors.push_back(nr);
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
    std::span<const uint8_t> payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
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
      } else if (sp_ver == 59) {
        // SM8550 v59 (data-driven from journal+B17F correlation):
        //   header 8B: earfcn:u32 LE, num_cells:u32 LE (not v48's 12B header)
        //   cell stride 148B; PCI = rd16(cell+8)&0x1FF; RSRP u12 at cell+44>>12
        // RSRQ offset not yet confident — omit. Fail-closed on pci/rsrp.
        if (body_len < 8) {
          pos += sp_size;
          continue;
        }
        uint32_t earfcn59 = Converter::read_le<uint32_t>(body, 0);
        uint32_t num_cells59 = Converter::read_le<uint32_t>(body, 4);
        if (num_cells59 > 8) num_cells59 = 8;

        if (valid_lte_earfcn(earfcn59)) {
          Events::RadioParamsEvent<LteRadioParams> rev;
          rev.data.earfcn = earfcn59;
          events.push_back(Events::RrcEvent{std::move(rev)});
        }

        Events::NeighborMeasEvent nev;
        constexpr size_t kCellStart = 8;
        constexpr size_t kCellStride = 148;
        for (uint32_t c = 0; c < num_cells59; ++c) {
          const size_t cell_off = kCellStart + kCellStride * c;
          // Need through rd32 at cell+44.
          if (cell_off + 48 > body_len) break;

          uint16_t pci_word = Converter::read_le<uint16_t>(body, cell_off + 8);
          uint16_t pci = Utils::lte_pci_from_meas_word(pci_word);
          if (!valid_lte_pci(pci) || pci == 0) continue;

          uint32_t rsrp_raw =
              (Converter::read_le<uint32_t>(body, cell_off + 44) >> 12) & 0xFFF;
          float rsrp = ml1_rsrp(rsrp_raw);
          if (!valid_lte_rsrp(rsrp)) continue;

          NeighborMeasResult nr;
          nr.pci = pci;
          nr.rsrp_dbm = rsrp;
          nr.has_rsrp = true;
          nev.neighbors.push_back(nr);
        }
        if (!nev.neighbors.empty()) {
          events.push_back(Events::RrcEvent{std::move(nev)});
        }
        pos += sp_size;
        continue;
      } else {
        pos += sp_size;
        continue;
      }

      if (num_cells > 8) num_cells = 8;

      if (valid_lte_earfcn(earfcn)) {
        Events::RadioParamsEvent<LteRadioParams> rev;
        rev.data.earfcn = earfcn;
        events.push_back(Events::RrcEvent{std::move(rev)});
      }

      Events::NeighborMeasEvent nev;
      for (uint16_t c = 0; c < num_cells; ++c) {
        size_t cell_off = cell_start + cell_stride * c;
        if (cell_off + 16 + 48 > body_len) break;

        uint16_t val0 = Converter::read_le<uint16_t>(body, cell_off);
        // MI: PCI = low 9 bits, is_serving = bit12. Live SIM8300 v48 confirms.
        // scat MSB bitstring >>7 mis-read PCI 468 as 35 on the same dumps.
        uint16_t pci = Utils::lte_pci_from_meas_word(val0);
        bool is_serving = ((val0 >> 12) & 1) != 0;
        if (!valid_lte_pci(pci) || pci == 0) continue;

        // scat RevWordBits @ cell+16: RSRP[108:120], RSRQ[224:234].
        RevWordBits rb(body + cell_off + 16, 12);
        float rsrp = ml1_rsrp(rb.slice(108, 120));
        if (!valid_lte_rsrp(rsrp)) continue;

        NeighborMeasResult nr;
        nr.pci = pci;
        nr.rsrp_dbm = rsrp;
        nr.has_rsrp = true;
        nr.rsrq_db = ml1_rsrq(rb.slice(224, 234));
        nr.has_rsrq = (nr.rsrq_db > -30.0f && nr.rsrq_db <= -1.0f);
        nev.neighbors.push_back(nr);

        if (is_serving) {
          Events::RadioParamsEvent<LteRadioParams> srev;
          srev.data.earfcn = earfcn;
          srev.data.pci = pci;
          events.push_back(Events::RrcEvent{std::move(srev)});
          events.push_back(Events::ServingChangedEvent{.is_serving = true});
          CellSignal sig;
          sig.signal_data = LteSignalParams{.rsrp = rsrp, .rsrq = nr.rsrq_db};
          events.push_back(Events::SignalUpdateEvent{.signal = std::move(sig)});
        }
      }
      if (!nev.neighbors.empty()) events.push_back(Events::RrcEvent{std::move(nev)});
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
    std::span<const uint8_t> payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);

  auto p = payload.data();
  uint8_t version = p[0];

  uint32_t earfcn = 0;
  uint16_t pci = 0;
  uint8_t dl_bw = 0;
  uint16_t sfn = 0;

  // scat parse_lte_ml1_cell_info: PCI via >>7 (MSB bitstring). Keep that for B197.
  if (version == 1 && payload.size() >= 8) {
    if (payload.size() >= 3) {
      dl_bw = rb_or_mhz_to_mhz(p[1]);
      sfn = Converter::read_le<uint16_t>(p, 2);
    }
    earfcn = Converter::read_le<uint16_t>(p, 4);
    uint16_t pci_word = Converter::read_le<uint16_t>(p, 6);
    pci = ((pci_word & 0xFFFF) >> 7) & 0x1FF;
  } else if (version == 2 && payload.size() >= 12) {
    if (payload.size() >= 4) {
      dl_bw = rb_or_mhz_to_mhz(p[1]);
      sfn = Converter::read_le<uint16_t>(p, 2);
    }
    earfcn = Converter::read_le<uint32_t>(p, 4);
    uint32_t pci_word = Converter::read_le<uint32_t>(p, 8);
    pci = ((pci_word & 0xFFFF) >> 7) & 0x1FF;
  } else {
    return std::vector<Events::RrcEvent>{};
  }

  if (!valid_lte_pci(pci) || !valid_lte_earfcn(earfcn)) { return std::vector<Events::RrcEvent>{}; }

  std::vector<Events::RrcEvent> events;

  Events::RadioParamsEvent<LteRadioParams> rev;
  rev.data.earfcn = earfcn;
  rev.data.pci = pci;
  if (dl_bw == 1 || dl_bw == 3 || dl_bw == 5 || dl_bw == 10 || dl_bw == 15 || dl_bw == 20)
    rev.data.dl_bw = dl_bw;
  if (sfn <= 1023) rev.data.sfn = sfn;
  events.push_back(Events::RrcEvent{std::move(rev)});
  events.push_back(Events::ServingChangedEvent{.is_serving = true});
  return events;
}

// ============================================================================
// 0xB0EC — LTE NAS EMM DL (Attach Accept / TAU Accept → TAI)
// ============================================================================
// DIAG sub-header: ext_hdr[1], rrc_rel[1], rrc_ver[1], bearer[1], msg_len[4]
// NAS PDU at payload offset 8.
// EMM PD=0x07. Attach Accept type=0x42, TAU Accept type=0x49.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_lte_nas(
    std::span<const uint8_t> payload) {
  // Short / keep-alive NAS frames are common — empty, not hard error (keeps stats honest).
  if (payload.size() < 12) return std::vector<Events::RrcEvent>{};

  auto p = payload.data();
  // Some builds prepend 1–4 pad bytes before the 8B DIAG NAS sub-header.
  size_t hdr = 8;
  if (payload.size() >= 16) {
    auto looks_emm = [](const uint8_t* m) {
      const uint8_t pd = m[0] & 0x0F;
      return pd == 0x07 || pd == 0x02;  // EMM or ESM
    };
    if (!looks_emm(p + 8)) {
      for (size_t skip = 1; skip <= 4 && 8 + skip + 2 <= payload.size(); ++skip) {
        if (looks_emm(p + 8 + skip)) {
          hdr = 8 + skip;
          break;
        }
      }
    }
  }
  const uint8_t* msg = p + hdr;
  size_t msg_len = payload.size() - hdr;

  if (msg_len < 4) return std::vector<Events::RrcEvent>{};

  uint8_t sec_hdr = (msg[0] >> 4) & 0x0F;
  uint8_t pd = msg[0] & 0x0F;
  if (pd != 0x07) return std::vector<Events::RrcEvent>{};

  // Integrity-only (1/3): peel seq+MAC and decode plain NAS underneath.
  // Ciphered (2/4) needs keys — fail-closed.
  const uint8_t* nas = msg;
  size_t nas_len = msg_len;
  if (sec_hdr == 1 || sec_hdr == 3) {
    if (msg_len < 8) return std::vector<Events::RrcEvent>{};
    nas = msg + 6;
    nas_len = msg_len - 6;
    if (nas_len < 2) return std::vector<Events::RrcEvent>{};
    if ((nas[0] & 0x0F) != 0x07) return std::vector<Events::RrcEvent>{};
    if (((nas[0] >> 4) & 0x0F) != 0) return std::vector<Events::RrcEvent>{};
  } else if (sec_hdr != 0) {
    return std::vector<Events::RrcEvent>{};
  }

  uint8_t msg_type = nas[1];
  const uint8_t* body = nas + 2;
  size_t body_len = nas_len - 2;

  // Attach Accept (0x42): TAI list at body[2] (length) + body[3..]
  if (msg_type == 0x42 && body_len >= 8) {
    uint8_t tai_len = body[2];
    if (tai_len >= 6 && body_len >= static_cast<size_t>(3 + tai_len)) {
      return decode_tai_list(body + 3, tai_len);
    }
  }

  // TAU Accept (0x49): TAI list is optional TLV with IEI=0x54
  if (msg_type == 0x49 && body_len >= 2) {
    size_t off = 1;  // skip EPS update result
    while (off + 2 < body_len) {
      uint8_t iei = body[off];
      if (iei == 0x54) {
        uint8_t tai_len = body[off + 1];
        if (off + 2 + tai_len <= body_len && tai_len >= 6) {
          return decode_tai_list(body + off + 2, tai_len);
        }
        break;
      }
      // Skip TLV
      if ((iei & 0xF0) >= 0x80) {
        ++off;  // Type-1 IE, 1 byte
      } else {
        off += 2 + body[off + 1];  // TLV
      }
    }
  }

  // Attach/TAU Request: Last visited registered TAI (Type-3 IEI 0x52, 5-byte TAI).
  if ((msg_type == 0x41 || msg_type == 0x48) && body_len >= 6) {
    for (size_t off = 0; off + 6 <= body_len; ++off) {
      if (body[off] != 0x52) continue;
      auto ev = decode_tai(body + off + 1, 5);
      if (!ev.empty()) return ev;
    }
  }

  return std::vector<Events::RrcEvent>{};
}

std::vector<Events::RrcEvent> LteParser::decode_tai(const uint8_t* tai, size_t len) {
  if (len < 5) return {};

  uint8_t d1 = tai[0] & 0x0F, d2 = (tai[0] >> 4) & 0x0F, d3 = tai[1] & 0x0F;
  uint8_t m3 = (tai[1] >> 4) & 0x0F, m1 = tai[2] & 0x0F, m2 = (tai[2] >> 4) & 0x0F;
  if (d1 > 9 || d2 > 9 || d3 > 9) return {};

  CellPassport passport;
  passport.mcc = d1 * 100 + d2 * 10 + d3;
  if (m3 == 0x0F)
    passport.mnc = m1 * 10 + m2;
  else if (m1 <= 9 && m2 <= 9 && m3 <= 9)
    passport.mnc = m1 * 100 + m2 * 10 + m3;
  else
    return {};

  passport.tac = static_cast<uint16_t>((tai[3] << 8) | tai[4]);
  if (passport.mcc < 100 || passport.mcc > 999 || passport.tac == 0) return {};

  return {Events::PassportEvent{.passport = std::move(passport)}};
}

std::vector<Events::RrcEvent> LteParser::decode_tai_list(const uint8_t* tai, size_t len) {
  if (len < 6) return {};
  // TAI list element: type[2 bits] + num[5 bits] at tai[0], then PLMN+TAC.
  return decode_tai(tai + 1, len - 1);
}

// ============================================================================
// 0xB179 — Connected-mode intra-freq meas (SIM8300 v4)
// ============================================================================
// Live dump: ver=4, EARFCN u32 @8, PCI u16 @12 (low 9 bits = PCI).

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ml1_conn_intra(
    std::span<const uint8_t> payload) {
  if (payload.size() < 16) return std::unexpected(ParserError::PacketTooShort);

  auto try_layout = [&](size_t eo, size_t po) -> std::optional<LocalCellKey> {
    if (payload.size() < po + 2) return std::nullopt;
    const uint32_t earfcn = Converter::read_le<uint32_t>(payload.data(), eo);
    const uint16_t pci =
        static_cast<uint16_t>(Converter::read_le<uint16_t>(payload.data(), po) & 0x1FF);
    if (!valid_lte_earfcn(earfcn) || !valid_lte_pci(pci) || pci == 0) return std::nullopt;
    return LocalCellKey{.freq = earfcn, .pci_bsic = pci};
  };

  std::optional<LocalCellKey> key;
  if (payload[0] == 4) {
    // Live SIM8300 v4: EARFCN u32 @8, PCI u16 @12 (low 9).
    key = try_layout(8, 12);
  } else if (payload[0] == 1) {
    // Live dump: ver=1 is v4 shifted by +1 byte (earfcn@9, pci@13).
    key = try_layout(9, 13);
    if (!key) key = try_layout(8, 12);
  }
  if (!key) return std::vector<Events::RrcEvent>{};

  Events::RadioParamsEvent<LteRadioParams> rev;
  rev.data.earfcn = key->freq;
  rev.data.pci = key->pci_bsic;
  return std::vector<Events::RrcEvent>{Events::RrcEvent{std::move(rev)}};
}

// ============================================================================
// 0xB181 — Intra-frequency cell reselection (SIM8300)
// ============================================================================
// Header: EARFCN u32 @8, PCI word u32 @12 (&0x1FF).
// Body: TLV markers 0x79 .. followed by EARFCN u32 (reselection candidates).

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ml1_intra_resel(
    std::span<const uint8_t> payload) {
  if (payload.size() < 16) return std::unexpected(ParserError::PacketTooShort);
  if (payload[0] != 1) return std::vector<Events::RrcEvent>{};

  std::vector<Events::RrcEvent> events;

  const uint32_t earfcn = Converter::read_le<uint32_t>(payload.data(), 8);
  const uint16_t pci =
      static_cast<uint16_t>(Converter::read_le<uint32_t>(payload.data(), 12) & 0x1FF);
  if (valid_lte_earfcn(earfcn) && valid_lte_pci(pci) && pci != 0) {
    Events::RadioParamsEvent<LteRadioParams> rev;
    rev.data.earfcn = earfcn;
    rev.data.pci = pci;
    events.push_back(Events::RrcEvent{std::move(rev)});
  }

  Events::InterFreqCarriersEvent carriers;
  std::set<uint32_t> seen;
  if (valid_lte_earfcn(earfcn)) seen.insert(earfcn);

  for (size_t i = 16; i + 8 <= payload.size(); ++i) {
    if (payload[i] != 0x79) continue;
    const uint32_t cand = Converter::read_le<uint32_t>(payload.data(), i + 4);
    if (!valid_lte_earfcn(cand) || seen.contains(cand)) continue;
    // Guard: next bytes after tag often repeat length nibbles (08 08 / 0e 0e / 0c 0c).
    if (payload[i + 2] != payload[i + 3]) continue;
    seen.insert(cand);
    carriers.carriers.push_back(InterFreqCarrier{.earfcn = cand});
  }
  if (!carriers.carriers.empty()) events.push_back(std::move(carriers));

  return events;
}

// ============================================================================
// 0xB192 — LTE PHY Idle Neighbor Cell Meas (MobileInsight + SIM8300)
// ============================================================================
// Packet v1: Version, NumSubPackets, skip2, then subpackets:
//   hdr: id u8, ver u8, size u16
//   id=26 ver=1/2: request — EARFCN + PCI list (no RSRP)
//   id=27 ver=2/4: result  — EARFCN + PCI + Inst RSRP/RSRQ (MI formulas)
// Unknown id/ver: skip by SubPacket Size (fail-closed, no invented cells).

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ml1_neigh_req(
    std::span<const uint8_t> payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);
  if (payload[0] != 1) return std::vector<Events::RrcEvent>{};

  const uint8_t n_sub = payload[1];
  if (n_sub == 0 || n_sub > 16) return std::vector<Events::RrcEvent>{};

  std::vector<Events::RrcEvent> events;
  Events::NeighborMeasEvent nev;
  size_t off = 4;

  auto append_radio = [&](uint32_t earfcn, uint16_t pci) {
    if (!valid_lte_earfcn(earfcn) || !valid_lte_pci(pci) || pci == 0) return;
    Events::RadioParamsEvent<LteRadioParams> rev;
    rev.data.earfcn = earfcn;
    rev.data.pci = pci;
    events.push_back(Events::RrcEvent{std::move(rev)});
  };

  for (uint8_t si = 0; si < n_sub && off + 4 <= payload.size(); ++si) {
    const size_t start = off;
    const uint8_t sid = payload[off];
    const uint8_t sver = payload[off + 1];
    const uint16_t ssize = Converter::read_le<uint16_t>(payload.data(), off + 2);
    off += 4;
    if (ssize < 4 || start + ssize > payload.size()) break;

    if (sid == 26 && (sver == 1 || sver == 2)) {
      // Request subpacket — EARFCN width differs by version.
      const size_t earfcn_w = (sver == 1) ? 2u : 4u;
      if (off + earfcn_w + 4 > start + ssize) {
        off = start + ssize;
        continue;
      }
      const uint32_t earfcn = (sver == 1)
                                  ? Converter::read_le<uint16_t>(payload.data(), off)
                                  : Converter::read_le<uint32_t>(payload.data(), off);
      off += earfcn_w;
      const uint8_t nc_raw = payload[off++];
      const int num_cells = nc_raw & 0x0F;
      off += 1;  // skip pad after Num Cells byte
      if (sver == 2) off += 2;  // MI 26v2 extra skip
      if (num_cells < 0 || num_cells > 16) {
        off = start + ssize;
        continue;
      }
      // Cell record: Cell ID u16 (+packed flags) + 14 bytes skip = 16.
      for (int j = 0; j < num_cells && off + 16 <= start + ssize; ++j) {
        const uint16_t cell_word = Converter::read_le<uint16_t>(payload.data(), off);
        const uint16_t pci = static_cast<uint16_t>(cell_word & 0x3FF);
        off += 16;
        append_radio(earfcn, pci);
      }
    } else if (sid == 27 && (sver == 2 || sver == 4)) {
      // Result subpacket — EARFCN width differs; v4 has SKIP 2 after Num Cells.
      const size_t earfcn_w = (sver == 2) ? 2u : 4u;
      if (off + earfcn_w + 2 > start + ssize) {
        off = start + ssize;
        continue;
      }
      const uint32_t earfcn = (sver == 2)
                                  ? Converter::read_le<uint16_t>(payload.data(), off)
                                  : Converter::read_le<uint32_t>(payload.data(), off);
      off += earfcn_w;
      const uint16_t nc_raw = Converter::read_le<uint16_t>(payload.data(), off);
      off += 2;
      if (sver == 4) {
        if (off + 2 > start + ssize) {
          off = start + ssize;
          continue;
        }
        off += 2;  // MI Payload_27v4 SKIP 2
      }
      const int num_cells = nc_raw & 0x3F;
      if (num_cells < 0 || num_cells > 16) {
        off = start + ssize;
        continue;
      }
      // Cell: PCI u32 + 8 skip + RSRP u16 + 2 skip + 4 skip + RSRQ u32 + 4 skip
      //       + RSSI u32 + 8 skip = 40 bytes (MI LtePhyIncm_Subpacket_27v2_cell).
      constexpr size_t kCell = 40;
      for (int j = 0; j < num_cells && off + kCell <= start + ssize; ++j) {
        const uint32_t pci_word = Converter::read_le<uint32_t>(payload.data(), off);
        const uint16_t pci = static_cast<uint16_t>(pci_word & 0x3FF);
        const uint16_t rsrp_raw = Converter::read_le<uint16_t>(payload.data(), off + 12);
        const uint32_t rsrq_raw = Converter::read_le<uint32_t>(payload.data(), off + 20);
        off += kCell;

        if (!valid_lte_earfcn(earfcn) || !valid_lte_pci(pci) || pci == 0) continue;
        append_radio(earfcn, pci);

        NeighborMeasResult nr;
        nr.pci = pci;
        // MI: RSRP = (raw & 4095) * 0.0625 - 180
        nr.rsrp_dbm = static_cast<float>(rsrp_raw & 0x0FFF) * 0.0625f - 180.0f;
        nr.has_rsrp = valid_lte_rsrp(nr.rsrp_dbm);
        // MI: RSRQ = ((raw >> 10) & 1023) * 0.0625 - 30
        nr.rsrq_db = static_cast<float>((rsrq_raw >> 10) & 0x3FF) * 0.0625f - 30.0f;
        nr.has_rsrq = (nr.rsrq_db >= -40.0f && nr.rsrq_db <= 0.0f);
        if (nr.has_rsrp) nev.neighbors.push_back(nr);
      }
    }

    // Realign to declared subpacket end (MI does the same for unknown/partial).
    off = start + ssize;
  }

  if (!nev.neighbors.empty()) events.push_back(std::move(nev));
  return events;
}

// ============================================================================
// 0xB195 — LTE PHY Connected Mode Neighbor Meas (MobileInsight)
// ============================================================================
// Outer: Version, NumSubPackets, skip2. Result subpacket id=31 ver=3/4/24.
// Request id=30 ignored. Unknown id/ver → skip by SubPacket Size.
// Live SIM8300 often emits empty/request-only frames while idle — harden further
// with a connected-mode hex dump when the modem is present.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ml1_conn_neigh(
    std::span<const uint8_t> payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);
  // MI documents pkt_ver=1; allow small range seen on SDX55 builds.
  if (payload[0] < 1 || payload[0] > 5) return std::vector<Events::RrcEvent>{};

  // n_sub is a hint only — SIM8300 often under-counts (n_sub=1 with request+result).
  const uint8_t n_sub_hint = payload[1];
  if (n_sub_hint == 0) return std::vector<Events::RrcEvent>{};

  std::vector<Events::RrcEvent> events;
  Events::NeighborMeasEvent nev;
  size_t off = 4;

  auto append_radio = [&](uint32_t earfcn, uint16_t pci) {
    if (!valid_lte_earfcn(earfcn) || !valid_lte_pci(pci) || pci == 0) return;
    Events::RadioParamsEvent<LteRadioParams> rev;
    rev.data.earfcn = earfcn;
    rev.data.pci = pci;
    events.push_back(Events::RrcEvent{std::move(rev)});
  };

  auto parse_result_31 = [&](uint8_t sver, size_t start, uint16_t ssize, size_t body_off) {
    // v3: EARFCN u16; v4/v24/v40: EARFCN u32 + SKIP 2 after Num Cells (MI + SIM8300).
    const bool wide = (sver != 3);
    const size_t earfcn_w = wide ? 4u : 2u;
    size_t o = body_off;
    if (o + earfcn_w + 2 > start + ssize) return;
    const uint32_t earfcn =
        wide ? Converter::read_le<uint32_t>(payload.data(), o)
             : Converter::read_le<uint16_t>(payload.data(), o);
    o += earfcn_w;
    const uint16_t nc_raw = Converter::read_le<uint16_t>(payload.data(), o);
    o += 2;
    if (wide) {
      if (o + 2 > start + ssize) return;
      o += 2;
    }
    const int num_cells = nc_raw & 0x3F;
    if (num_cells < 1 || num_cells > 16) return;

    const size_t remain = (start + ssize > o) ? (start + ssize - o) : 0;
    // Prefer MI 52B cell; fall back to 40B (idle-neigh layout) if size fits better.
    size_t stride = 52;
    if (remain < static_cast<size_t>(num_cells) * 52 &&
        remain >= static_cast<size_t>(num_cells) * 40)
      stride = 40;
    if (remain < static_cast<size_t>(num_cells) * stride) return;

    for (int j = 0; j < num_cells; ++j) {
      const size_t cell = o + static_cast<size_t>(j) * stride;
      const uint32_t pci_word = Converter::read_le<uint32_t>(payload.data(), cell);
      const uint16_t pci = static_cast<uint16_t>(pci_word & 0x3FF);
      const uint16_t rsrp_raw = Converter::read_le<uint16_t>(payload.data(), cell + 12);
      const uint32_t rsrq_raw = Converter::read_le<uint32_t>(payload.data(), cell + 20);

      if (!valid_lte_earfcn(earfcn) || !valid_lte_pci(pci) || pci == 0) continue;
      append_radio(earfcn, pci);

      NeighborMeasResult nr;
      nr.pci = pci;
      nr.rsrp_dbm = static_cast<float>(rsrp_raw & 0x0FFF) * 0.0625f - 180.0f;
      nr.has_rsrp = valid_lte_rsrp(nr.rsrp_dbm);
      nr.rsrq_db = static_cast<float>((rsrq_raw >> 10) & 0x3FF) * 0.0625f - 30.0f;
      nr.has_rsrq = (nr.rsrq_db > -30.0f && nr.rsrq_db <= -1.0f);
      if (nr.has_rsrp) nev.neighbors.push_back(nr);
    }
  };

  // Walk by SubPacket Size to EOF (not only n_sub) — catches SIM8300 under-count.
  for (uint8_t si = 0; si < 16 && off + 4 <= payload.size(); ++si) {
    const uint8_t sid = payload[off];
    const uint8_t sver = payload[off + 1];
    const uint16_t ssize = Converter::read_le<uint16_t>(payload.data(), off + 2);
    // Some SIM8300 frames insert a pad byte before the first subpkt — resync.
    if ((sid != 30 && sid != 31) || ssize < 4 || off + ssize > payload.size()) {
      bool synced = false;
      for (size_t j = off; j + 4 < payload.size(); ++j) {
        const uint8_t id = payload[j];
        if (id != 30 && id != 31) continue;
        const uint16_t sz = Converter::read_le<uint16_t>(payload.data(), j + 2);
        if (sz >= 4 && j + sz <= payload.size()) {
          off = j;
          synced = true;
          break;
        }
      }
      if (!synced) break;
      continue;
    }
    const size_t start = off;
    off += 4;

    // Result: id=31. MI docs ver 3/4/24; SIM8300 live uses ver=40 (0x28) wide EARFCN.
    if (sid == 31 && (sver == 3 || sver == 4 || sver == 24 || (sver >= 2 && sver <= 64))) {
      parse_result_31(sver, start, ssize, off);
    }
    // sid==30 (0x1E) request: no stable cell rows.

    off = start + ssize;
  }

  if (!nev.neighbors.empty()) events.push_back(std::move(nev));
  return events;
}

// ============================================================================
// 0xB113 / 0xB123 — LL1 PSS / Neighbor CER
// ============================================================================
// SIM8300 dumps: correlation / energy buffers with no stable EARFCN|PCI fields.
// Registered so log-code table shows support; emit nothing (not халва).

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ll1_pss(
    std::span<const uint8_t> payload) {
  if (payload.empty()) return std::unexpected(ParserError::PacketTooShort);
  return std::vector<Events::RrcEvent>{};
}

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ll1_ncell_cer(
    std::span<const uint8_t> payload) {
  if (payload.size() < 4) return std::unexpected(ParserError::PacketTooShort);
  return std::vector<Events::RrcEvent>{};
}

// ============================================================================
// 0xB115 — LL1 SSS / freq confirm (SIM8300)
// ============================================================================
// EARFCN u32 @4 when len>=8. No PCI in short frames → attach as inter-freq
// carrier on serving (does not invent PCI rows).

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ll1_sss(
    std::span<const uint8_t> payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);
  const uint32_t earfcn = Converter::read_le<uint32_t>(payload.data(), 4);
  if (!valid_lte_earfcn(earfcn)) return std::vector<Events::RrcEvent>{};

  Events::InterFreqCarriersEvent carriers;
  carriers.carriers.push_back(InterFreqCarrier{.earfcn = earfcn});
  return std::vector<Events::RrcEvent>{std::move(carriers)};
}

// ============================================================================
// 0xB0CB / 0xB0CD — paging / UE CA capability (not cell identity)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_rrc_paging(
    std::span<const uint8_t> payload) {
  if (payload.empty()) return std::unexpected(ParserError::PacketTooShort);
  return std::vector<Events::RrcEvent>{};
}

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_rrc_ca_combos(
    std::span<const uint8_t> payload) {
  if (payload.size() < 4) return std::unexpected(ParserError::PacketTooShort);
  // UE capability bitmap — useful for modem fingerprint, not tower table.
  return std::vector<Events::RrcEvent>{};
}

}  // namespace QCom::Lte
