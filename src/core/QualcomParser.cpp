#include "core/QualcomParser.h"

#include <iostream>
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
    if (m_dispatch.contains(code)) {
      std::clog << "[QualcomParser] LogCode collision: " << to_string(code)
                << " already registered, skipping\n";
      continue;
    }
    m_dispatch[code] = parser;
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

  // Extract LocalCellKey from metadata if this is an RRC OTA packet
  // For ML1 packets, the key comes from the payload itself
  LocalCellKey key;
  if (pkt.payload.size() >= 7) {
    key.freq = Utils::Converter::read_le<uint16_t>(pkt.payload, 2);
    key.pci_bsic = Utils::Converter::read_le<uint16_t>(pkt.payload, 4);
  }

  // Determine RAT from log code range
  RatType rat = RatType::UNKNOWN;
  uint8_t equipment_id = static_cast<uint8_t>((pkt.log_code >> 12) & 0x0F);
  switch (equipment_id) {
    case 0x0B: rat = RatType::LTE; break;
    case 0x05: rat = RatType::GSM; break;
    case 0x04: rat = RatType::WCDMA; break;
    default:
      // NR log codes use 0xB8xx / 0xB9xx range
      if (pkt.log_code >= 0xB800) rat = RatType::NR;
      break;
  }

  for (auto& event : result.value()) {
    m_tracker.handle_rrc_event(Events::RrcEventEnvelope{
        .key = key,
        .rat = rat,
        .timestamp = pkt.timestamp,
        .event_data = std::move(event),
    });
  }

  if (m_cell_cb) { m_cell_cb(m_tracker.get_snapshot()); }

  return {};
}

}  // namespace QCom
