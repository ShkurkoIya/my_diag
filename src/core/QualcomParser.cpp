#include "core/QualcomParser.h"

#include <memory>

#include "gsm/GsmParser.h"
#include "lte/LteParser.h"
#include "nr/NrParser.h"
#include "wcdma/WcdmaParser.h"

namespace QCom {

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

std::expected<void, ParserError> QualcomParser::on_diag_frame(std::span<const uint8_t> raw_frame) {
  // Qualcomm DIAG LOG_F frame layout:
  //   [0]    cmd_code (0x10 = LOG_F)
  //   [1-3]  padding
  //   [4-5]  log_code (LE16)
  //   [6-13] timestamp + misc
  //   [14+]  payload
  if (raw_frame.size() < 14) return std::unexpected(ParserError::PacketTooShort);

  LogCode log_code = Utils::Converter::read_le<uint16_t>(raw_frame, 4);
  uint64_t timestamp = Utils::Converter::read_le<uint64_t>(raw_frame, 6);
  auto payload = raw_frame.subspan(14);

  return on_packet(QualcommPacketView{
      .log_code = log_code,
      .timestamp = timestamp,
      .payload = payload,
  });
}

std::expected<void, ParserError> QualcomParser::on_packet(QualcommPacketView pkt) {
  auto it = m_dispatch.find(pkt.log_code);
  if (it == m_dispatch.end()) return std::unexpected(ParserError::WrongLogCode);

  auto result = it->second->parse(pkt);
  if (!result.has_value()) return std::unexpected(result.error());

  RatType rat = classify_rat(pkt.log_code);
  LocalCellKey key = extract_cell_key(pkt, rat);

  // Fallback: if no cell key extracted, attach to current serving cell of same RAT
  if (key.freq == 0 && key.pci_bsic == 0) { key = m_tracker.serving_key(rat); }

  for (auto& event : result.value()) {
    m_tracker.handle_rrc_event(Events::RrcEventEnvelope{
        .key = key,
        .rat = rat,
        .timestamp = pkt.timestamp,
        .event_data = std::move(event),
    });
  }

  if (m_cell_cb) m_cell_cb(m_tracker.get_snapshot());

  return {};
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
  // Only RRC OTA packets (0xB0C0 LTE, 0xB821 NR) have the standard 7-byte
  // Qualcomm metadata header with EARFCN at offset 2 and PCI at offset 4.
  // ML1/proprietary packets have completely different layouts — their cell keys
  // are extracted inside the parser and attached to events via CellTracker.
  bool is_rrc_ota = (pkt.log_code == 0xB0C0 || pkt.log_code == 0xB821);

  if (is_rrc_ota && pkt.payload.size() >= 7) {
    return LocalCellKey{
        .freq = Utils::Converter::read_le<uint16_t>(pkt.payload, 2),
        .pci_bsic = Utils::Converter::read_le<uint16_t>(pkt.payload, 4),
    };
  }

  // For proprietary packets (0xB0C2, 0xB17F, 0xB180, etc.), extract key
  // from the payload using log-code-specific offsets
  if (pkt.payload.size() >= 8) {
    uint8_t version = static_cast<uint8_t>(pkt.payload[0]);

    if (rat == RatType::LTE) {
      if (pkt.log_code == 0xB0C2) {
        // 0xB0C2: PCI at +1, EARFCN at +3 (v2) or +3 (v3, 32-bit)
        uint16_t pci = Utils::Converter::read_le<uint16_t>(pkt.payload, 1);
        uint16_t earfcn =
            (version == 3)
                ? static_cast<uint16_t>(Utils::Converter::read_le<uint32_t>(pkt.payload, 3))
                : Utils::Converter::read_le<uint16_t>(pkt.payload, 3);
        return {.freq = earfcn, .pci_bsic = pci};
      }
      if (pkt.log_code == 0xB17F || pkt.log_code == 0xB197) {
        uint32_t earfcn = (version >= 5) ? Utils::Converter::read_le<uint32_t>(pkt.payload, 4)
                                         : Utils::Converter::read_le<uint16_t>(pkt.payload, 4);
        size_t pci_off = (version >= 5) ? 8 : 6;
        uint16_t pci_slp = Utils::Converter::read_le<uint16_t>(pkt.payload, pci_off);
        uint16_t pci = (pci_slp >> 7) & 0x1FF;
        return {.freq = earfcn, .pci_bsic = pci};
      }
    }

    // GSM: 0x5134 Cell Info has ARFCN at +0, BSIC = (NCC<<3|BCC) at +2/+3
    if (rat == RatType::GSM) {
      if (pkt.log_code == 0x5134 && pkt.payload.size() >= 6) {
        uint16_t arfcn = Utils::Converter::read_le<uint16_t>(pkt.payload, 0) & 0x0FFF;
        uint8_t bcc = pkt.payload[2];
        uint8_t ncc = pkt.payload[3];
        uint16_t bsic = static_cast<uint16_t>(((ncc & 7) << 3) | (bcc & 7));
        return {.freq = arfcn, .pci_bsic = bsic};
      }
      // 0x512F, 0x5071 etc — no per-packet cell key, use serving cell's key
    }

    // NR: 0xB992 serving has subpacket container, NRARFCN at sp_data+0 (24 bits), PCI at sp_data+4
    // (10 bits)
    if (rat == RatType::NR && pkt.payload.size() >= 16) {
      // Container: 4 bytes header, subpkt: 4 bytes header, sp_data starts at payload[8]
      uint32_t nrarfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 8) & 0x00FFFFFF;
      uint16_t pci = Utils::Converter::read_le<uint16_t>(pkt.payload, 12) & 0x03FF;
      if (nrarfcn > 0 && pci <= 1007) return {.freq = nrarfcn, .pci_bsic = pci};
    }

    // WCDMA: 0x4027/0x4127 have DL_UARFCN at +4, PSC at +16
    if (rat == RatType::WCDMA) {
      if ((pkt.log_code == 0x4027 || pkt.log_code == 0x4127) && pkt.payload.size() >= 18) {
        uint32_t uarfcn = Utils::Converter::read_le<uint32_t>(pkt.payload, 4);
        uint16_t psc_raw = Utils::Converter::read_le<uint16_t>(pkt.payload, 16);
        uint16_t psc = (pkt.log_code == 0x4027) ? (psc_raw >> 4) : psc_raw;
        return {.freq = uarfcn, .pci_bsic = psc};
      }
    }
  }

  return {};
}

}  // namespace QCom
