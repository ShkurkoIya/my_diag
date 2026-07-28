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

std::expected<void, ParserError> QualcomParser::on_diag_frame(std::string_view raw_frame) {
  // Qualcomm DIAG LOG_F frame layout:
  //   [0]    cmd_code (0x10 = LOG_F)
  //   [1-3]  padding
  //   [4-5]  log_code (LE16)
  //   [6-13] timestamp + misc
  //   [14+]  payload
  if (raw_frame.size() < 14) return std::unexpected(ParserError::PacketTooShort);

  LogCode log_code = Utils::Converter::read_le<uint16_t>(raw_frame, 4);
  uint64_t timestamp = Utils::Converter::read_le<uint64_t>(raw_frame, 6);
  std::string_view payload = raw_frame.substr(14);

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
        // EARFCN and PCI_SLP packed in header
        uint32_t earfcn = (version >= 5) ? Utils::Converter::read_le<uint32_t>(pkt.payload, 4)
                                         : Utils::Converter::read_le<uint16_t>(pkt.payload, 4);
        size_t pci_off = (version >= 5) ? 8 : 6;
        uint16_t pci_slp = Utils::Converter::read_le<uint16_t>(pkt.payload, pci_off);
        uint16_t pci = (pci_slp >> 7) & 0x1FF;
        return {.freq = earfcn, .pci_bsic = pci};
      }
    }
  }

  // Fallback: key will be {0,0} — CellTracker will create a generic entry.
  // This is acceptable for neighbor-only or measurement-only packets.
  return {};
}

}  // namespace QCom
