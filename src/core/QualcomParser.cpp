#include <memory>
#include <observer/model/Utils.h>
#include <qcom/lte/LteRrcOta.h>
#include <qcom/parser/QualcomParser.h>
#include <qcom/protocol/LogFrameAdapter.h>
#include <utility>
#include <variant>

#include "gsm/GsmParser.h"
#include "lte/LteParser.h"
#include "lte/LteQcomLayouts.h"
#include "nr/NrParser.h"
#include "wcdma/WcdmaParser.h"

namespace QCom {
namespace {

[[nodiscard]] bool is_identity_event(const Events::RrcEvent& ev) noexcept {
  return std::holds_alternative<Events::PassportEvent>(ev);
}

[[nodiscard]] bool is_neighbor_list_event(const Events::RrcEvent& ev) noexcept {
  return std::holds_alternative<Events::GsmNeighborsEvent>(ev) ||
         std::holds_alternative<Events::WcdmaNeighborsEvent>(ev) ||
         std::holds_alternative<Events::NeighborMeasEvent>(ev);
}

}  // namespace

QualcomParser::QualcomParser() {
  register_parser(std::make_shared<Lte::LteParser>());
  register_parser(std::make_shared<Nr::NrParser>());
  register_parser(std::make_shared<Gsm::GsmParser>());
  register_parser(std::make_shared<Wcdma::WcdmaParser>());
}

void QualcomParser::register_parser(std::shared_ptr<IRatParser> parser) {
  if (!parser) return;

  for (LogCode code : parser->get_supported_codes()) {
    if (!m_dispatch.contains(code)) { m_dispatch.emplace(code, parser); }
  }
}

std::string_view QualcomParser::code_name(LogCode code) const noexcept {
  auto it = m_dispatch.find(code);
  if (it == m_dispatch.end() || !it->second) return {};
  return it->second->log_to_string(code);
}

std::expected<void, ParserError> QualcomParser::on_diag_frame(std::span<const uint8_t> raw_frame) {
  auto pkt = adapt_log_f_frame(raw_frame);
  if (!pkt) return std::unexpected(ParserError::PacketTooShort);
  return on_packet(*pkt);
}

std::expected<void, ParserError> QualcomParser::on_packet(QualcommPacketView pkt) {
  // DIAG events (journal code 0x0000): GPRS_SURROUND_SEARCH_START → GSM SI-3 bind hint.
  if (pkt.log_code == 0x0000) {
    handle_diag_event(pkt.payload);
    return {};
  }

  auto it = m_dispatch.find(pkt.log_code);
  if (it == m_dispatch.end()) {
    ++m_code_stats[pkt.log_code].seen;  // flew, but no registered parser
    return std::unexpected(ParserError::WrongLogCode);
  }

  auto result = it->second->parse(pkt);
  auto& st = m_code_stats[pkt.log_code];
  ++st.seen;
  if (!result.has_value()) {
    ++st.error;
    return std::unexpected(result.error());
  }
  if (result->empty()) {
    ++st.empty;
    return {};
  }
  ++st.with_events;

  RatType rat = classify_rat(pkt.log_code);
  LocalCellKey packet_key = extract_cell_key(pkt, rat);
  const LocalCellKey serving = m_tracker.serving_key(rat);

  LocalCellKey sticky_key = packet_key;

  for (auto& event : result.value()) {
    LocalCellKey key = sticky_key;

    // Multi-cell packets (B0C4 etc.) emit RadioParams then Passport per row.
    // Prefer EARFCN/PCI from the radio event so subsequent Passport binds correctly.
    if (auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&event)) {
      if (auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen)) {
        if (lte->data.earfcn != 0) {
          // 0xB17F/0xB197: restamp PCI from the pack-order SSOT (parser may have
          // run without the session lock; B197 inherits 0xB17F votes).
          if ((pkt.log_code == 0xB17F || pkt.log_code == 0xB197) && packet_key.pci_bsic != 0)
            lte->data.pci = packet_key.pci_bsic;
          key = {.freq = lte->data.earfcn, .pci_bsic = lte->data.pci};
          sticky_key = key;
        }
      }
    }

    // Identity / neighbor lists without a physical key must NOT steal the
    // serving ARFCN/PCI — CellTracker resolves by CID or upserts neighbors.
    if (key.freq == 0 && key.pci_bsic == 0) {
      if (!(is_identity_event(event) || is_neighbor_list_event(event))) { key = serving; }
    }

    m_tracker.handle_rrc_event(Events::RrcEventEnvelope{
        .key = key,
        .rat = rat,
        .timestamp = pkt.timestamp,
        .wall_time = pkt.wall_time,
        .event_data = std::move(event),
    });
  }

  if (m_cell_cb) m_cell_cb(m_tracker.get_snapshot());

  return {};
}

void QualcomParser::handle_diag_event(std::span<const uint8_t> buf) {
  // DCI event payload (journal RAW for 0x0000): [len LE16][event…]
  // Per event: eid LE16 (id=eid&0xFFF, pl_ind=(eid>>13)&3, ts_trunc=(eid>>15)&1),
  // timestamp 8 or 2 bytes, then payload.
  if (buf.size() < 4) return;
  size_t pos = 2;
  if (pos + 2 > buf.size()) return;
  uint16_t eid = Utils::Converter::read_le<uint16_t>(buf, pos);
  pos += 2;
  uint16_t event_id = eid & 0x0FFF;
  uint8_t pl_ind = (eid >> 13) & 0x3;
  uint8_t ts_trunc = (eid >> 15) & 0x1;
  pos += ts_trunc ? 2 : 8;
  if (pos > buf.size()) return;

  const uint8_t* pay = buf.data() + pos;
  size_t paylen = 0;
  if (pl_ind == 1)
    paylen = 1;
  else if (pl_ind == 2)
    paylen = 2;
  else if (pl_ind == 3 && pos < buf.size()) {
    paylen = buf[pos];
    pay = buf.data() + pos + 1;
    ++pos;
  }
  if (pay + paylen > buf.data() + buf.size()) return;

  // Event 500 = GPRS_SURROUND_SEARCH_START — ARFCN in first 2 payload bytes.
  if (event_id == 500 && paylen >= 2) {
    uint16_t arfcn = static_cast<uint16_t>((pay[0] | (pay[1] << 8)) & 0x3FF);
    if (arfcn) m_tracker.set_gsm_surround_arfcn_hint(arfcn);
    return;
  }

  // Event 1606 = EVENT_LTE_RRC_STATE_CHANGE — 1-byte RRC state → serving LTE.
  if (event_id == Lte::Wire::Evt1606::kEventId && paylen >= 1) {
    const uint8_t st = pay[0];
    if (!Lte::Wire::Evt1606::known(st)) return;

    Events::RadioParamsEvent<LteRadioParams> rev;
    rev.data.rrc_state = static_cast<int16_t>(st);
    m_tracker.handle_rrc_event(Events::RrcEventEnvelope{
        .key = {},
        .rat = RatType::LTE,
        .timestamp = 0,
        .event_data = Events::RrcEvent{std::move(rev)},
    });
    if (m_cell_cb) m_cell_cb(m_tracker.get_snapshot());
  }
}

RatType QualcomParser::classify_rat(LogCode code) noexcept {
  // NR codes (0xB8xx, 0xB9xx) share equipment_id 0x0B with LTE — check first
  if (code >= 0xB800) return RatType::NR;

  uint8_t equip = static_cast<uint8_t>((code >> 12) & 0x0F);
  switch (equip) {
    case 0x0B: return RatType::LTE;
    case 0x05: return RatType::GSM;
    case 0x04: return RatType::WCDMA;
    case 0x07: return RatType::WCDMA;
    default: return RatType::UNKNOWN;
  }
}

LocalCellKey QualcomParser::extract_cell_key(const QualcommPacketView& pkt, RatType rat) noexcept {
  bool is_rrc_ota = (pkt.log_code == 0xB0C0 || pkt.log_code == 0xB821);

  if (is_rrc_ota && !pkt.payload.empty()) {
    if (pkt.log_code == 0xB0C0) {
      if (auto ota = Lte::decode_lte_rrc_ota(pkt.payload)) {
        return {.freq = ota->earfcn, .pci_bsic = ota->pci};
      }
      // Synthetic / legacy 7-byte header used by journal_replay
      if (pkt.payload.size() >= 7) {
        uint8_t ch = pkt.payload[6];
        if (ch >= 1 && ch <= 5) {
          return {
              .freq = Utils::Converter::read_le<uint16_t>(pkt.payload, 2),
              .pci_bsic = Utils::Converter::read_le<uint16_t>(pkt.payload, 4),
          };
        }
      }
    } else if (pkt.payload.size() >= 7) {
      return {
          .freq = Utils::Converter::read_le<uint16_t>(pkt.payload, 2),
          .pci_bsic = Utils::Converter::read_le<uint16_t>(pkt.payload, 4),
      };
    }
  }

  if (pkt.payload.size() >= 8) {
    uint8_t version = static_cast<uint8_t>(pkt.payload[0]);

    if (rat == RatType::LTE) {
      if (pkt.log_code == 0xB0C1 && pkt.payload.size() >= 2) {
        uint8_t ver = pkt.payload[0];
        if (ver == 1 && pkt.payload.size() >= 9) {
          uint16_t pci = Utils::Converter::read_le<uint16_t>(pkt.payload, 1);
          uint16_t earfcn = Utils::Converter::read_le<uint16_t>(pkt.payload, 3);
          return {.freq = earfcn, .pci_bsic = pci};
        }
        if (ver == 2 && pkt.payload.size() >= 11) {
          uint16_t pci = Utils::Converter::read_le<uint16_t>(pkt.payload, 1);
          uint32_t earfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 3);
          return {.freq = earfcn, .pci_bsic = pci};
        }
      }
      if (pkt.log_code == 0xB0C2) {
        uint16_t pci = Utils::Converter::read_le<uint16_t>(pkt.payload, 1);
        uint16_t earfcn =
            (version == 3)
                ? static_cast<uint16_t>(Utils::Converter::read_le<uint32_t>(pkt.payload, 3))
                : Utils::Converter::read_le<uint16_t>(pkt.payload, 3);
        return {.freq = earfcn, .pci_bsic = pci};
      }
      if (pkt.log_code == 0xB176 && pkt.payload.size() >= 22) {
        uint32_t earfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 4);
        uint16_t pci = Utils::Converter::read_le<uint16_t>(pkt.payload, 20);
        if (earfcn > 0) return {.freq = earfcn, .pci_bsic = pci};
      }
      if (pkt.log_code == 0xB194 && pkt.payload.size() >= 12 && pkt.payload[0] == 1) {
        for (size_t i = 2; i + 8 < pkt.payload.size(); ++i) {
          if (pkt.payload[i] != 0x1D) continue;
          uint8_t sp_ver = pkt.payload[i + 1];
          if (sp_ver < 0x20 || sp_ver > 0x40) continue;
          uint16_t sp_size = Utils::Converter::read_le<uint16_t>(pkt.payload, i + 2);
          size_t rem = pkt.payload.size() - i;
          size_t use = rem;
          if (sp_size >= 8 && sp_size <= rem) use = sp_size;
          if (use < 8) continue;
          const uint8_t* body = pkt.payload.data() + i + 4;
          size_t body_len = use - 4;
          for (auto [eo, po] : {std::pair<size_t, size_t>{16, 28}, {16, 32}, {20, 36}}) {
            if (body_len < po + 4) continue;
            uint32_t earfcn = Utils::Converter::read_le<uint32_t>(body, eo);
            uint32_t pci_u = Utils::Converter::read_le<uint32_t>(body, po);
            if (Utils::valid_lte_earfcn(earfcn) && pci_u >= 1 && pci_u <= 503)
              return {.freq = earfcn, .pci_bsic = static_cast<uint16_t>(pci_u)};
          }
        }
      }
      if (pkt.log_code == 0xB179 && version == 4 && pkt.payload.size() >= 16) {
        uint32_t earfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 8);
        uint16_t pci = Utils::Converter::read_le<uint16_t>(pkt.payload, 12) & 0x1FF;
        if (earfcn > 0) return {.freq = earfcn, .pci_bsic = pci};
      }
      if (pkt.log_code == 0xB181 && version == 1 && pkt.payload.size() >= 16) {
        uint32_t earfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 8);
        uint16_t pci =
            static_cast<uint16_t>(Utils::Converter::read_le<uint32_t>(pkt.payload, 12) & 0x1FF);
        if (earfcn > 0) return {.freq = earfcn, .pci_bsic = pci};
      }
      if (pkt.log_code == 0xB192 && version == 1 && pkt.payload.size() >= 48 &&
          pkt.payload[32] == 0x1B) {
        uint32_t earfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 36);
        uint16_t pci =
            static_cast<uint16_t>(Utils::Converter::read_le<uint32_t>(pkt.payload, 44) & 0x1FF);
        if (earfcn > 0 && pci <= 503) return {.freq = earfcn, .pci_bsic = pci};
      }
      if (pkt.log_code == 0xB195 && version == 1 && pkt.payload.size() >= 16) {
        // Connected neigh: walk first result subpkt 31 for EARFCN|PCI (same framing as B192).
        size_t off = 4;
        const uint8_t n_sub = pkt.payload[1];
        for (uint8_t i = 0; i < n_sub && off + 4 <= pkt.payload.size(); ++i) {
          const size_t start = off;
          const uint8_t sid = pkt.payload[off];
          const uint8_t sver = pkt.payload[off + 1];
          const uint16_t ssize = Utils::Converter::read_le<uint16_t>(pkt.payload, off + 2);
          off += 4;
          if (ssize < 4 || start + ssize > pkt.payload.size()) break;
          // SIM8300 uses sver=40 (0x28) for result subpkt — same wide layout as v4.
          if (sid == 31 && (sver == 3 || sver == 4 || sver == 24 || (sver >= 2 && sver <= 64))) {
            const bool wide = (sver != 3);
            const size_t earfcn_w = wide ? 4u : 2u;
            if (off + earfcn_w + 2 + (wide ? 2u : 0u) + 4 <= start + ssize) {
              uint32_t earfcn = wide ? Utils::Converter::read_le<uint32_t>(pkt.payload, off)
                                     : Utils::Converter::read_le<uint16_t>(pkt.payload, off);
              off += earfcn_w + 2;
              if (wide) off += 2;
              uint16_t pci = static_cast<uint16_t>(
                  Utils::Converter::read_le<uint32_t>(pkt.payload, off) & 0x3FF);
              if (Utils::valid_lte_earfcn(earfcn) && pci >= 1 && pci <= 503)
                return {.freq = earfcn, .pci_bsic = pci};
            }
          }
          off = start + ssize;
        }
      }
      if (pkt.log_code == 0xB197) {
        uint32_t earfcn = 0;
        uint16_t packed = 0;
        if (version == 1 && pkt.payload.size() >= 8) {
          earfcn = Utils::Converter::read_le<uint16_t>(pkt.payload, 4);
          packed = Utils::Converter::read_le<uint16_t>(pkt.payload, 6);
        } else if (version == 2 && pkt.payload.size() >= 12) {
          earfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 4);
          packed = static_cast<uint16_t>(Utils::Converter::read_le<uint32_t>(pkt.payload, 8) &
                                         0xFFFF);
        }
        if (earfcn > 0) {
          m_pci_pack.observe(packed);
          return {.freq = earfcn, .pci_bsic = m_pci_pack.unpack(packed).pci};
        }
      }
      if (pkt.log_code == 0xB17F) {
        uint32_t earfcn = (version >= 5) ? Utils::Converter::read_le<uint32_t>(pkt.payload, 4)
                                         : Utils::Converter::read_le<uint16_t>(pkt.payload, 4);
        size_t pci_off = (version >= 5) ? 8 : 6;
        const uint16_t packed = Utils::Converter::read_le<uint16_t>(pkt.payload, pci_off);
        m_pci_pack.observe(packed);
        return {.freq = earfcn, .pci_bsic = m_pci_pack.unpack(packed).pci};
      }
      if (pkt.log_code == 0xB193 && version == 1 && pkt.payload.size() >= 8) {
        // Container: pkt_ver, num_subpkts, reserved; then subpkt id/ver/size.
        // Pull EARFCN (+ first PCI when layout known) so NeighborMeasEvent keys correctly.
        const auto* p = pkt.payload.data();
        const size_t plen = pkt.payload.size();
        uint8_t num_subpkts = p[1];
        size_t pos = 4;
        for (uint8_t s = 0; s < num_subpkts && pos + 4 <= plen; ++s) {
          uint8_t sp_id = p[pos];
          uint8_t sp_ver = p[pos + 1];
          uint16_t sp_size = Utils::Converter::read_le<uint16_t>(p, pos + 2);
          if (sp_size < 4 || pos + sp_size > plen) break;
          if (sp_id == 0x19 && sp_size >= 8) {
            const uint8_t* body = p + pos + 4;
            size_t body_len = static_cast<size_t>(sp_size) - 4;
            if (body_len >= 4) {
              uint32_t earfcn = Utils::Converter::read_le<uint32_t>(body, 0);
              uint16_t pci = 0;
              auto take_pci = [&](uint16_t raw) {
                uint16_t v = Utils::lte_pci_from_meas_word(raw);
                return (v >= 1 && v <= 503) ? v : uint16_t{0};
              };
              if (sp_ver == 59 && body_len >= 8 + 10) {
                pci = take_pci(Utils::Converter::read_le<uint16_t>(body, 8 + 8));
              } else if ((sp_ver == 36 || sp_ver == 48 || sp_ver == 50) && body_len >= 10) {
                size_t cell_start = (sp_ver == 36) ? 8u : 12u;
                if (body_len >= cell_start + 2) {
                  uint16_t val0 = Utils::Converter::read_le<uint16_t>(body, cell_start);
                  pci = Utils::lte_pci_from_meas_word(val0);
                  if (pci > 503) pci = 0;
                }
              }
              if (earfcn > 0) return {.freq = earfcn, .pci_bsic = pci};
            }
          }
          pos += sp_size;
        }
      }
    }

    if (rat == RatType::GSM) {
      auto cell_info = pkt.payload;
      if (pkt.log_code == 0x5B34 && cell_info.size() >= 1) cell_info = cell_info.subspan(1);
      if ((pkt.log_code == 0x5134 || pkt.log_code == 0x5B34) && cell_info.size() >= 6) {
        uint16_t arfcn = Utils::Converter::read_le<uint16_t>(cell_info, 0) & 0x0FFF;
        uint8_t bcc = cell_info[2];
        uint8_t ncc = cell_info[3];
        uint16_t bsic = static_cast<uint16_t>(((ncc & 7) << 3) | (bcc & 7));
        return {.freq = arfcn, .pci_bsic = bsic};
      }
    }

    if (rat == RatType::NR) {
      if (pkt.log_code == 0xB822 && pkt.payload.size() >= 10) {
        const uint16_t pci = Utils::Converter::read_le<uint16_t>(pkt.payload, 4);
        const uint32_t nrarfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 6);
        if (Utils::valid_nr_arfcn(nrarfcn) && nrarfcn && Utils::valid_nr_pci(pci))
          return {.freq = nrarfcn, .pci_bsic = pci};
      }
      if (pkt.log_code == 0xB823 && pkt.payload.size() >= 14) {
        const uint16_t rel_min = Utils::Converter::read_le<uint16_t>(pkt.payload, 0);
        const uint16_t rel_maj = Utils::Converter::read_le<uint16_t>(pkt.payload, 2);
        uint16_t pci = 0;
        uint32_t nrarfcn = 0;
        if (rel_maj == 0 && rel_min == 4 && pkt.payload.size() >= 10) {
          pci = Utils::Converter::read_le<uint16_t>(pkt.payload, 4);
          nrarfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 6);
        } else if (rel_maj == 3 && rel_min == 0 && pkt.payload.size() >= 18) {
          pci = Utils::Converter::read_le<uint16_t>(pkt.payload, 4);
          nrarfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 14);
        } else if (rel_maj == 3 && (rel_min == 2 || rel_min == 3) && pkt.payload.size() >= 21) {
          pci = Utils::Converter::read_le<uint16_t>(pkt.payload, 7);
          nrarfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 17);
        }
        if (Utils::valid_nr_arfcn(nrarfcn) && nrarfcn && Utils::valid_nr_pci(pci))
          return {.freq = nrarfcn, .pci_bsic = pci};
      }
      if (pkt.payload.size() >= 16) {
        uint32_t nrarfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 8) & 0x00FFFFFF;
        uint16_t pci = Utils::Converter::read_le<uint16_t>(pkt.payload, 12) & 0x03FF;
        if (nrarfcn > 0 && pci <= 1007) return {.freq = nrarfcn, .pci_bsic = pci};
      }
    }

    if (rat == RatType::WCDMA) {
      if ((pkt.log_code == 0x4027 || pkt.log_code == 0x4127) && pkt.payload.size() >= 18) {
        uint32_t uarfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 4);
        uint16_t psc_raw = Utils::Converter::read_le<uint16_t>(pkt.payload, 16);
        uint16_t psc = (pkt.log_code == 0x4027) ? (psc_raw >> 4) : psc_raw;
        return {.freq = uarfcn, .pci_bsic = psc};
      }
      // 0x4005 reselection: first 3G cell is serving (UARFCN|PSC).
      if (pkt.log_code == 0x4005 && pkt.payload.size() >= 12) {
        const uint8_t ver = (pkt.payload[0] >> 6) & 0x03;
        size_t off = (ver == 2) ? 7u : 2u;
        if (off + 4 <= pkt.payload.size()) {
          uint16_t uarfcn = Utils::Converter::read_le<uint16_t>(pkt.payload, off);
          uint16_t psc = Utils::Converter::read_le<uint16_t>(pkt.payload, off + 2);
          if (uarfcn > 0 && uarfcn <= 16383 && psc <= 511) return {.freq = uarfcn, .pci_bsic = psc};
        }
      }
      // 0x4111 active set: UARFCN at byte 1, first PSC at offset 4.
      if (pkt.log_code == 0x4111 && pkt.payload.size() >= 6) {
        uint16_t uarfcn = Utils::Converter::read_le<uint16_t>(pkt.payload, 1);
        uint16_t psc = Utils::Converter::read_le<uint16_t>(pkt.payload, 4);
        if (uarfcn > 0 && uarfcn <= 16383 && psc <= 511) return {.freq = uarfcn, .pci_bsic = psc};
      }
    }
  }

  return {};
}

}  // namespace QCom
