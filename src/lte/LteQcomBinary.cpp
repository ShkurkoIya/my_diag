/// @file LteQcomBinary.cpp
/// @brief Qualcomm proprietary binary ML1 log parsing for LTE.
///
/// All formats here are reverse-engineered from scat/QCSuper/dia_vldos
/// and SIM8300 live dumps (fail-closed on unknown versions).
#include <cstdint>
#include <optional>
#include <set>

#include "core/BinaryCursor.h"
#include "core/RevWordBits.h"
#include "lte/LteParser.h"
#include "lte/LteQcomLayouts.h"

namespace QCom::Lte {

using Utils::BinaryCursor;
using Utils::bits;
using Utils::Converter;
using Utils::ml1_rsrp;
using Utils::ml1_rsrq;
using Utils::ml1_rssi;
using Utils::RevWordBits;
using Utils::valid_lte_earfcn;
using Utils::valid_lte_pci;
using Utils::valid_lte_rsrp;

// ============================================================================
// 0xB0C2 — Serving Cell Info (proprietary Qualcomm identity packet)
// ============================================================================
// Qualcomm duplicates SIB1 identity into this fixed-layout packet.
// Arrives faster than SIB1 ASN.1 decode — useful as primary identity source.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_serv_cell_info(
    std::span<const uint8_t> payload) {
  if (payload.size() < 2) return std::unexpected(ParserError::PacketTooShort);

  // Wire offsets/SSOT: lte/LteQcomLayouts.h (B0c2::V2 / V3).
  const auto decoded = Wire::B0c2::decode(BinaryCursor{payload});
  if (!decoded) return std::vector<Events::RrcEvent>{};

  CellPassport passport;
  LteRadioParams radio;
  radio.pci = decoded->pci;
  radio.earfcn = decoded->earfcn;
  radio.ul_earfcn = decoded->ul_earfcn;
  radio.dl_bw = Wire::B0c2::bw_raw_to_mhz(decoded->dl_bw_raw);
  radio.ul_bw = Wire::B0c2::bw_raw_to_mhz(decoded->ul_bw_raw);
  passport.cell_id = decoded->cell_id;
  passport.tac = decoded->tac;
  radio.freq_band_ind = static_cast<uint8_t>(decoded->band);
  passport.mcc = decoded->mcc;
  passport.mnc = decoded->mnc;
  if (decoded->mnc_digit == 2 || decoded->mnc_digit == 3) {
    passport.mnc_digits = decoded->mnc_digit;
  }
  radio.allowed_access = Wire::B0c2::allowed_access_bool(decoded->allowed_raw);

  if (!valid_lte_pci(radio.pci) || radio.pci == 0 || !valid_lte_earfcn(radio.earfcn)) {
    return std::vector<Events::RrcEvent>{};
  }

  std::vector<Events::RrcEvent> events;

  // Always mint RADIO for a valid EARFCN|PCI. COPS/? PLMN search often emits
  // B0C2 with RF key but incomplete/padding ECI/TAC/MCC — dropping the whole
  // packet was why a fat COPS firehose left the registry nearly empty.
  Events::RadioParamsEvent<LteRadioParams> rev;
  rev.data = radio;
  events.push_back(Events::RrcEvent{std::move(rev)});

  const bool have_id = Utils::valid_lte_eci(passport.cell_id) &&
                       Utils::valid_lte_tac(passport.tac) && passport.mcc >= 100 &&
                       passport.mcc <= 999;
  if (have_id) { events.push_back(Events::PassportEvent{.passport = std::move(passport)}); }
  // Do NOT emit ServingChanged here: during PLMN/COPS sweep B0C2 fires for
  // every briefly-camped cell; thrashing is_serving hides the real QMI camp.

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
// SIM8300: 0x1D header may not sit at payload[4] — scan for id/ver/size.
// Two verified response body shapes (fail-closed):
//   A) COPS multi-hit: EARFCN u32 @16, then N×16B records @24 with PCI u32 @+8
//   B) Single-hit: EARFCN+PCI at fixed offsets ({16,28}|{16,32}|{20,36})
// Emit every distinct EARFCN|PCI — one frame may list several PCIs.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ml1_search_rr(
    std::span<const uint8_t> payload) {
  if (payload.size() < 12) return std::unexpected(ParserError::PacketTooShort);
  if (payload[0] != 1) return std::vector<Events::RrcEvent>{};

  auto push_key = [](std::vector<LocalCellKey>& out, uint32_t earfcn, uint16_t pci) {
    if (!valid_lte_earfcn(earfcn) || !valid_lte_pci(pci) || pci == 0) return;
    for (const auto& e : out) {
      if (e.freq == earfcn && e.pci_bsic == pci) return;
    }
    out.push_back(LocalCellKey{.freq = earfcn, .pci_bsic = pci});
  };

  auto try_body = [&](const uint8_t* body, size_t body_len, std::vector<LocalCellKey>& out) {
    // A) Multi-PCI list (live COPS / PLMN search on SIM8300).
    if (body_len >= 40) {
      const uint32_t earfcn = Converter::read_le<uint32_t>(body, 16);
      if (valid_lte_earfcn(earfcn)) {
        size_t minted = 0;
        for (size_t off = 24; off + 16 <= body_len; off += 16) {
          const uint32_t pci_u = Converter::read_le<uint32_t>(body, off + 8);
          if (pci_u < 1 || pci_u > 503) break;
          push_key(out, earfcn, static_cast<uint16_t>(pci_u));
          ++minted;
        }
        if (minted > 0) return;
      }
    }
    // B) Single-cell layouts from earlier SIM8300 dumps.
    const std::pair<size_t, size_t> layouts[] = {{16, 28}, {16, 32}, {20, 36}};
    for (auto [eo, po] : layouts) {
      if (body_len < po + 4) continue;
      const uint32_t earfcn = Converter::read_le<uint32_t>(body, eo);
      const uint32_t pci_u = Converter::read_le<uint32_t>(body, po);
      if (!valid_lte_earfcn(earfcn) || pci_u < 1 || pci_u > 503) continue;
      push_key(out, earfcn, static_cast<uint16_t>(pci_u));
      return;
    }
  };

  std::vector<Events::RrcEvent> events;
  std::vector<LocalCellKey> keys;
  for (size_t i = 2; i + 8 < payload.size();) {
    if (payload[i] != 0x1D) {
      ++i;
      continue;
    }
    const uint8_t sp_ver = payload[i + 1];
    if (sp_ver < 0x20 || sp_ver > 0x40) {
      ++i;
      continue;
    }
    const uint16_t sp_size = Converter::read_le<uint16_t>(payload.data(), i + 2);
    const size_t rem = payload.size() - i;
    // Qualcomm size field is often short by a few bytes vs USB pad — use min.
    size_t use = rem;
    if (sp_size >= 8 && sp_size <= rem) use = sp_size;
    if (use < 8) {
      ++i;
      continue;
    }
    try_body(payload.data() + i + 4, use - 4, keys);
    // Advance past this subpacket so we don't re-match the same 0x1D.
    i += (sp_size >= 8 && sp_size <= rem) ? sp_size : 1;
  }
  for (const auto& key : keys) {
    Events::RadioParamsEvent<LteRadioParams> rev;
    rev.data.earfcn = key.freq;
    rev.data.pci = key.pci_bsic;
    events.push_back(Events::RrcEvent{std::move(rev)});
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
  uint8_t cell_resel_prio = 0;
  uint32_t rsrp_raw = 0, rsrq_raw = 0, rssi_raw = 0;
  uint32_t rxlev_w = 0, s_search_w = 0;

  if (version == 4 && payload.size() >= 32) {
    earfcn = Converter::read_le<uint16_t>(p, 4);
    const auto slp = Utils::lte_unpack_pci_slp(Converter::read_le<uint16_t>(p, 6));
    pci = slp.pci;
    cell_resel_prio = slp.prio;
    rsrp_raw = Converter::read_le<uint32_t>(p, 8) & 0xFFF;
    rsrq_raw = Converter::read_le<uint32_t>(p, 16) >> 22;
    rssi_raw = (Converter::read_le<uint32_t>(p, 20) >> 11) & 0x7FF;
    rxlev_w = Converter::read_le<uint32_t>(p, 24);
    s_search_w = Converter::read_le<uint32_t>(p, 28);
  } else if (version == 5 && payload.size() >= 36) {
    earfcn = Converter::read_le<uint32_t>(p, 4);
    const auto slp = Utils::lte_unpack_pci_slp(Converter::read_le<uint16_t>(p, 8));
    pci = slp.pci;
    cell_resel_prio = slp.prio;
    rsrp_raw = Converter::read_le<uint32_t>(p, 12) & 0xFFF;
    rsrq_raw = Converter::read_le<uint32_t>(p, 20) >> 22;
    rssi_raw = (Converter::read_le<uint32_t>(p, 24) >> 11) & 0x7FF;
    rxlev_w = Converter::read_le<uint32_t>(p, 28);
    s_search_w = Converter::read_le<uint32_t>(p, 32);
  } else if (version == 4 && payload.size() >= 24) {
    // Short legacy frames (tests): signal only.
    earfcn = Converter::read_le<uint16_t>(p, 4);
    const auto slp = Utils::lte_unpack_pci_slp(Converter::read_le<uint16_t>(p, 6));
    pci = slp.pci;
    cell_resel_prio = slp.prio;
    rsrp_raw = Converter::read_le<uint32_t>(p, 8) & 0xFFF;
    rsrq_raw = Converter::read_le<uint32_t>(p, 16) >> 22;
    rssi_raw = (Converter::read_le<uint32_t>(p, 20) >> 11) & 0x7FF;
  } else if (version == 5 && payload.size() >= 28) {
    earfcn = Converter::read_le<uint32_t>(p, 4);
    const auto slp = Utils::lte_unpack_pci_slp(Converter::read_le<uint16_t>(p, 8));
    pci = slp.pci;
    cell_resel_prio = slp.prio;
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

  // scat MSB bitfields on rxlev / s_search words.
  const uint8_t q_rxlevmin = static_cast<uint8_t>((rxlev_w >> 26) & 0x3F);
  const uint8_t p_max_raw = static_cast<uint8_t>((rxlev_w >> 19) & 0x7F);
  const uint8_t s_intra = static_cast<uint8_t>((s_search_w >> 26) & 0x3F);
  const uint8_t s_non = static_cast<uint8_t>((s_search_w >> 20) & 0x3F);

  std::vector<Events::RrcEvent> events;
  if (valid_lte_earfcn(earfcn) && valid_lte_pci(pci) && pci != 0) {
    Events::RadioParamsEvent<LteRadioParams> rev;
    rev.data.earfcn = earfcn;
    rev.data.pci = pci;
    if (cell_resel_prio) rev.data.cell_resel_prio = cell_resel_prio;
    if (s_intra) rev.data.s_intra_search = static_cast<int8_t>(s_intra);
    if (s_non) rev.data.s_non_intra_search = static_cast<int8_t>(s_non);
    if (p_max_raw) {
      rev.data.p_max_present = true;
      rev.data.p_max = static_cast<int8_t>(p_max_raw);
    }
    events.push_back(Events::RrcEvent{std::move(rev)});
  }
  if (q_rxlevmin) {
    CellPassport pass;
    pass.q_rx_lev_min = static_cast<int8_t>(q_rxlevmin * 2);
    events.push_back(Events::PassportEvent{.passport = std::move(pass)});
  }

  CellSignal sig;
  sig.signal_data = LteSignalParams{.rsrp = rsrp, .rsrq = rsrq, .rssi = rssi, .has_rssi = true};
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

  const BinaryCursor pkt{payload};
  if (pkt.u8(0) != 1) return std::vector<Events::RrcEvent>{};

  const uint8_t num_subpkts = pkt.u8(1);
  size_t pos = 4;

  std::vector<Events::RrcEvent> events;

  for (uint8_t sp = 0; sp < num_subpkts && pkt.has(pos, 4); ++sp) {
    const uint8_t sp_id = pkt.u8(pos);
    const uint8_t sp_ver = pkt.u8(pos + 1);
    const uint16_t sp_size = pkt.le16(pos + 2);

    if (sp_size < 4 || !pkt.has(pos, sp_size)) break;

    if (sp_id == Wire::B193::kSubpacketId) {
      const BinaryCursor body = pkt.slice(pos + 4, sp_size - 4);
      if (body.size() < 8) {
        pos += sp_size;
        continue;
      }

      // v59: separate header/cell shape (see Wire::B193::Sp19V59).
      if (sp_ver == Wire::B193::Sp19V59::kVersion) {
        using V59 = Wire::B193::Sp19V59;
        if (!body.has(0, V59::kMinBody)) {
          pos += sp_size;
          continue;
        }
        const uint32_t earfcn59 = body.le32(V59::earfcn);
        uint32_t num_cells59 = body.le32(V59::num_cells);
        if (num_cells59 > 8) num_cells59 = 8;

        if (valid_lte_earfcn(earfcn59)) {
          Events::RadioParamsEvent<LteRadioParams> rev;
          rev.data.earfcn = earfcn59;
          events.push_back(Events::RrcEvent{std::move(rev)});
        }

        Events::NeighborMeasEvent nev;
        for (uint32_t c = 0; c < num_cells59; ++c) {
          const BinaryCursor cell = body.at(V59::cell_start + V59::cell_stride * c);
          if (!cell.has(0, V59::rsrp_word + 4)) break;

          const uint16_t pci = Utils::lte_pci_from_meas_word(cell.le16(V59::pci));
          if (!valid_lte_pci(pci) || pci == 0) continue;

          const float rsrp = ml1_rsrp(cell.le32_bits(V59::rsrp_word, 12, 12));
          if (!valid_lte_rsrp(rsrp)) continue;

          NeighborMeasResult nr;
          nr.pci = pci;
          nr.rsrp_dbm = rsrp;
          nr.has_rsrp = true;
          nev.neighbors.push_back(nr);
        }
        if (!nev.neighbors.empty()) { events.push_back(Events::RrcEvent{std::move(nev)}); }
        pos += sp_size;
        continue;
      }

      const auto layout = Wire::B193::sp19_rev_layout(sp_ver);
      if (!layout) {
        pos += sp_size;
        continue;
      }

      const uint32_t earfcn = body.le32(Wire::B193::kBodyEarfcn);
      uint16_t num_cells = body.le16(Wire::B193::kBodyNumCells);
      if (num_cells > 8) num_cells = 8;
      const uint16_t valid_rx = body.le16(Wire::B193::kBodyValidRx);

      if (valid_lte_earfcn(earfcn)) {
        Events::RadioParamsEvent<LteRadioParams> rev;
        rev.data.earfcn = earfcn;
        if (valid_rx) rev.data.valid_rx = static_cast<uint8_t>(valid_rx & 0xFF);
        events.push_back(Events::RrcEvent{std::move(rev)});
      }

      // Wire SSOT: Wire::B193. v48/50 = LE meas; v36 = legacy RevWordBits.
      Events::NeighborMeasEvent nev;
      for (uint16_t c = 0; c < num_cells; ++c) {
        const size_t cell_off = layout->cell_start + layout->cell_stride * c;
        const BinaryCursor cell = body.at(cell_off);

        NeighborMeasResult nr;
        LteSignalParams lp;
        LteRadioParams radio{};
        radio.earfcn = earfcn;
        if (valid_rx) radio.valid_rx = static_cast<uint8_t>(valid_rx & 0xFF);
        bool is_serving = false;

        if (layout->le_meas) {
          const auto meas = Wire::B193::decode_sp19_le_cell(cell);
          if (!meas) continue;

          nr.pci = meas->pci;
          nr.rsrp_dbm = meas->rsrp_inst;
          nr.has_rsrp = true;
          if (meas->has_rsrp_filt) {
            nr.rsrp_filt = meas->rsrp_filt;
            nr.has_rsrp_filt = true;
          }
          if (meas->has_rsrq) {
            nr.rsrq_db = meas->rsrq_inst;
            nr.has_rsrq = true;
          }
          if (meas->has_rsrq_filt) {
            nr.rsrq_filt = meas->rsrq_filt;
            nr.has_rsrq_filt = true;
          }
          if (meas->has_rssi) {
            nr.rssi_dbm = meas->rssi_inst;
            nr.has_rssi = true;
          }
          if (meas->has_snr) {
            nr.sinr_db = meas->snr_best;
            nr.has_sinr = true;
            nr.sinr_rx0 = meas->snr_rx0;
            nr.sinr_rx1 = meas->snr_rx1;
            nr.has_sinr_per_rx = true;
          }

          lp.rsrp = meas->rsrp_inst;
          lp.rsrq = meas->rsrq_inst;
          if (meas->has_rsrp_filt) {
            lp.rsrp_filt = meas->rsrp_filt;
            lp.has_rsrp_filt = true;
          }
          if (meas->has_rsrq_filt) {
            lp.rsrq_filt = meas->rsrq_filt;
            lp.has_rsrq_filt = true;
          }
          if (meas->has_rssi) {
            lp.rssi = meas->rssi_inst;
            lp.has_rssi = true;
          }
          if (meas->has_snr) {
            lp.sinr = meas->snr_best;
            lp.has_sinr = true;
            lp.sinr_rx0 = meas->snr_rx0;
            lp.sinr_rx1 = meas->snr_rx1;
            lp.has_sinr_per_rx = true;
          }

          radio.pci = meas->pci;
          radio.sfn = meas->sfn;
          radio.subframe = meas->subframe;
          radio.has_sfn_sf = true;
          radio.serving_cell_index = meas->serving_cell_index;
          radio.is_restricted = meas->is_restricted;
          is_serving = meas->is_serving;
        } else {
          if (!cell.has(0, Wire::B193::kMeasWordsOff + 48)) break;

          const uint16_t val0 = cell.le16(0);
          const uint16_t pci = Utils::lte_pci_from_meas_word(val0);
          is_serving = cell.le16_bits(0, 12, 1) != 0;
          if (!valid_lte_pci(pci) || pci == 0) continue;

          const RevWordBits rb = Wire::B193::cell_meas_bits(cell);
          const float rsrp = ml1_rsrp(rb.slice(Wire::B193::kRsrp.begin, Wire::B193::kRsrp.end));
          if (!valid_lte_rsrp(rsrp)) continue;

          nr.pci = pci;
          nr.rsrp_dbm = rsrp;
          nr.has_rsrp = true;
          nr.rsrq_db = ml1_rsrq(rb.slice(Wire::B193::kRsrq.begin, Wire::B193::kRsrq.end));
          nr.has_rsrq = (nr.rsrq_db > -30.0f && nr.rsrq_db <= -1.0f);
          lp.rsrp = rsrp;
          lp.rsrq = nr.rsrq_db;

          const float rssi = ml1_rssi(rb.slice(Wire::B193::kRssi.begin, Wire::B193::kRssi.end));
          if (rssi > -120.0f && rssi < -20.0f) {
            lp.rssi = rssi;
            lp.has_rssi = true;
            nr.rssi_dbm = rssi;
            nr.has_rssi = true;
          }
          if (cell.has(layout->snr_off, 8)) {
            const RevWordBits snr_rb(cell.data() + layout->snr_off, 2);
            const float best = Wire::B193::snr_best_of(snr_rb);
            if (best > -20.0f && best < 40.0f) {
              lp.sinr = best;
              lp.has_sinr = true;
              nr.sinr_db = best;
              nr.has_sinr = true;
            }
          }
          radio.pci = pci;
        }

        nev.neighbors.push_back(nr);

        // Per-cell Radio+Signal so QualcomParser sticky_key binds each PCI.
        events.push_back(Events::RrcEvent{Events::RadioParamsEvent<LteRadioParams>{.data = radio}});
        CellSignal sig;
        sig.signal_data = lp;
        events.push_back(Events::SignalUpdateEvent{.signal = std::move(sig)});
        if (is_serving) { events.push_back(Events::ServingChangedEvent{.is_serving = true}); }
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

  // scat parse_lte_ml1_cell_info used >>7 (MSB). Unpack by value — same word as 0xB17F.
  if (version == 1 && payload.size() >= 8) {
    if (payload.size() >= 3) {
      dl_bw = Wire::B0c2::bw_raw_to_mhz(p[1]);
      sfn = Converter::read_le<uint16_t>(p, 2);
    }
    earfcn = Converter::read_le<uint16_t>(p, 4);
    pci = Utils::lte_unpack_pci_slp(Converter::read_le<uint16_t>(p, 6)).pci;
  } else if (version == 2 && payload.size() >= 12) {
    if (payload.size() >= 4) {
      dl_bw = Wire::B0c2::bw_raw_to_mhz(p[1]);
      sfn = Converter::read_le<uint16_t>(p, 2);
    }
    earfcn = Converter::read_le<uint32_t>(p, 4);
    const uint16_t packed =
        static_cast<uint16_t>(Converter::read_le<uint32_t>(p, 8) & 0xFFFF);
    pci = Utils::lte_unpack_pci_slp(packed).pci;
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
// 0xB0EE — LTE NAS EMM State (registration / GUTI meta)
// ============================================================================
// No EARFCN|PCI — QualcomParser binds empty key to current serving (like B114).
// Does NOT emit PassportEvent (NAS PLMN ≠ cell SIB identity). M-TMSI not exported.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_emm_state(
    std::span<const uint8_t> payload) {
  if (payload.size() < 2) return std::unexpected(ParserError::PacketTooShort);

  const auto decoded = Wire::B0ee::decode(BinaryCursor{payload});
  if (!decoded) return std::vector<Events::RrcEvent>{};

  Events::RadioParamsEvent<LteRadioParams> rev;
  rev.data.emm_state = static_cast<int16_t>(decoded->emm_state);
  rev.data.emm_substate = static_cast<int16_t>(decoded->emm_substate);
  rev.data.emm_mcc = decoded->plmn.mcc;
  rev.data.emm_mnc = decoded->plmn.mnc;
  rev.data.emm_mnc_digits = decoded->plmn.mnc_digits;
  if (decoded->guti_valid) {
    rev.data.mme_group_id = decoded->mme_group_id;
    rev.data.mme_code = decoded->mme_code;
    rev.data.mme_present = true;
  }
  return std::vector<Events::RrcEvent>{Events::RrcEvent{std::move(rev)}};
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
// 0xB179 — Connected-mode intra-freq meas (MobileInsight CMLIFMR + SIM8300)
// ============================================================================
// MI v4: hdr 8B, then EARFCN u32, PCI u16, SFN u16, RSRP u16, skip2, RSRQ u16, skip2,
//        n_neigh u8, n_det u8, skip2; then neigh {pci u16, rsrp u16, skip2, rsrq u16, skip4}.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ml1_conn_intra(
    std::span<const uint8_t> payload) {
  if (payload.size() < 16) return std::unexpected(ParserError::PacketTooShort);

  const uint8_t version = payload[0];
  std::vector<Events::RrcEvent> events;

  if (version == 4 && payload.size() >= 28) {
    // MI: initial Fmt consumes 8 bytes, then v4 header.
    const uint32_t earfcn = Converter::read_le<uint32_t>(payload.data(), 8);
    const uint16_t pci =
        static_cast<uint16_t>(Converter::read_le<uint16_t>(payload.data(), 12) & 0x1FF);
    if (!valid_lte_earfcn(earfcn) || !valid_lte_pci(pci) || pci == 0) {
      return std::vector<Events::RrcEvent>{};
    }

    Events::RadioParamsEvent<LteRadioParams> rev;
    rev.data.earfcn = earfcn;
    rev.data.pci = pci;
    events.push_back(Events::RrcEvent{std::move(rev)});

    const uint16_t rsrp_raw = Converter::read_le<uint16_t>(payload.data(), 16);
    const uint16_t rsrq_raw = Converter::read_le<uint16_t>(payload.data(), 20);
    const float rsrp = static_cast<float>(rsrp_raw) * 0.0625f - 180.0f;
    const float rsrq = static_cast<float>(rsrq_raw) * 0.0625f - 30.0f;
    if (valid_lte_rsrp(rsrp)) {
      LteSignalParams lp{.rsrp = rsrp};
      if (rsrq > -30.0f && rsrq <= -1.0f) lp.rsrq = rsrq;
      CellSignal sig;
      sig.signal_data = lp;
      events.push_back(Events::SignalUpdateEvent{.signal = std::move(sig)});
      events.push_back(Events::ServingChangedEvent{.is_serving = true});
    }

    if (payload.size() >= 26) {
      const uint8_t n_neigh = payload[24];
      Events::NeighborMeasEvent nev;
      size_t off = 28;
      const uint8_t n = n_neigh > 16 ? 16 : n_neigh;
      for (uint8_t i = 0; i < n && off + 12 <= payload.size(); ++i, off += 12) {
        const uint16_t npci =
            static_cast<uint16_t>(Converter::read_le<uint16_t>(payload.data(), off) & 0x1FF);
        const float nrsrp =
            static_cast<float>(Converter::read_le<uint16_t>(payload.data(), off + 2)) * 0.0625f -
            180.0f;
        const float nrsrq =
            static_cast<float>(Converter::read_le<uint16_t>(payload.data(), off + 6)) * 0.0625f -
            30.0f;
        if (!valid_lte_pci(npci) || npci == 0 || !valid_lte_rsrp(nrsrp)) continue;
        NeighborMeasResult nr;
        nr.pci = npci;
        nr.rsrp_dbm = nrsrp;
        nr.has_rsrp = true;
        if (nrsrq > -30.0f && nrsrq <= -1.0f) {
          nr.rsrq_db = nrsrq;
          nr.has_rsrq = true;
        }
        nev.neighbors.push_back(nr);
        Events::RadioParamsEvent<LteRadioParams> nrev;
        nrev.data.earfcn = earfcn;
        nrev.data.pci = npci;
        events.push_back(Events::RrcEvent{std::move(nrev)});
      }
      if (!nev.neighbors.empty()) events.push_back(std::move(nev));
    }
    return events;
  }

  // Fallback: SIM8300 short frames with EARFCN|PCI only.
  auto try_layout = [&](size_t eo, size_t po) -> std::optional<LocalCellKey> {
    if (payload.size() < po + 2) return std::nullopt;
    const uint32_t earfcn = Converter::read_le<uint32_t>(payload.data(), eo);
    const uint16_t pci =
        static_cast<uint16_t>(Converter::read_le<uint16_t>(payload.data(), po) & 0x1FF);
    if (!valid_lte_earfcn(earfcn) || !valid_lte_pci(pci) || pci == 0) return std::nullopt;
    return LocalCellKey{.freq = earfcn, .pci_bsic = pci};
  };

  std::optional<LocalCellKey> key;
  if (version == 1) {
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
      const uint32_t earfcn = (sver == 1) ? Converter::read_le<uint16_t>(payload.data(), off)
                                          : Converter::read_le<uint32_t>(payload.data(), off);
      off += earfcn_w;
      const uint8_t nc_raw = payload[off++];
      const int num_cells = nc_raw & 0x0F;
      off += 1;                 // skip pad after Num Cells byte
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
      const uint32_t earfcn = (sver == 2) ? Converter::read_le<uint16_t>(payload.data(), off)
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
    const uint32_t earfcn = wide ? Converter::read_le<uint32_t>(payload.data(), o)
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
// 0xB114 — LL1 Serving Cell Frame Timing (TA index → serving radio)
// ============================================================================
// No EARFCN/PCI in packet — QualcomParser binds empty key to current serving.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ll1_frame_timing(
    std::span<const uint8_t> payload) {
  const auto hdr = Wire::B114::decode_header(BinaryCursor{payload});
  if (!hdr) {
    if (payload.size() < Wire::B114::kHeaderSize)
      return std::unexpected(ParserError::PacketTooShort);
    return std::vector<Events::RrcEvent>{};  // unknown version — fail-closed
  }
  // TA 0 is valid (very close) but our merge treats 0 as unset; skip emit.
  if (hdr->timing_advance == 0) return std::vector<Events::RrcEvent>{};

  Events::RadioParamsEvent<LteRadioParams> rev;
  rev.data.timing_advance = hdr->timing_advance;
  return std::vector<Events::RrcEvent>{Events::RrcEvent{std::move(rev)}};
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
// 0xB115 — LL1 SSS Results (SIM8300 ver=122 / 0x7A)
// ============================================================================
// Short (8B): EARFCN u32 @4 — freq confirm only.
// Long: EARFCN @4, then N×16B detected-cell records @8.
//   PCI = (u16 LE @ record+2) & 0x1FF  (QXDM Physical Cell ID, 9 bits)
//   N   = payload[1] >> 5  (live: 0x80→4, 0x60→3, 0x20→1), capped by length.
// During AT+COPS=? this is the dominant cell-minting firehose on SIM8300.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_ll1_sss(
    std::span<const uint8_t> payload) {
  if (payload.size() < 8) return std::unexpected(ParserError::PacketTooShort);
  const uint32_t earfcn = Converter::read_le<uint32_t>(payload.data(), 4);
  if (!valid_lte_earfcn(earfcn)) return std::vector<Events::RrcEvent>{};

  std::vector<Events::RrcEvent> events;
  size_t n_hint = static_cast<size_t>(payload[1] >> 5);
  const size_t n_fit = (payload.size() > 8) ? (payload.size() - 8) / 16 : 0;
  if (n_hint == 0 || n_hint > n_fit) n_hint = n_fit;
  if (n_hint > 16) n_hint = 16;

  for (size_t i = 0; i < n_hint; ++i) {
    const size_t off = 8 + i * 16;
    const uint16_t pci =
        static_cast<uint16_t>(Converter::read_le<uint16_t>(payload.data(), off + 2) & 0x1FF);
    if (!valid_lte_pci(pci) || pci == 0) continue;
    Events::RadioParamsEvent<LteRadioParams> rev;
    rev.data.earfcn = earfcn;
    rev.data.pci = pci;
    events.push_back(Events::RrcEvent{std::move(rev)});
  }

  if (events.empty()) {
    // Freq-only confirm — still useful as SIB5-style carrier hint on serving.
    Events::InterFreqCarriersEvent carriers;
    carriers.carriers.push_back(InterFreqCarrier{.earfcn = earfcn});
    events.push_back(std::move(carriers));
  }
  return events;
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
