#include "QualcomParser.h"
#include "CellIdentity.h"
#include "lte/LteRrcParser.h"
#include <algorithm>
#include <cstdint>
#include <expected>
#include <iostream>
#include <memory>

namespace observer_qcom_parser {

QualcommParser::QualcommParser(
    std::vector<std::shared_ptr<IRatParser>> initial_modules) {

  if (initial_modules.empty()) {
    register_parser_module(std::make_shared<lte::LteRrcParser>());
  } else {
    for (auto &module : initial_modules) {
      register_parser_module(module);
    }
  }
}

void QualcommParser::register_parser_module(
    std::shared_ptr<IRatParser> parser_module) {
  if (!parser_module)
    return;

  for (LogCode code : parser_module->get_supported_codes()) {
    if (m_parsers.find(code) != m_parsers.end()) {
      std::clog << "[Warning] Collision detected! LogCode " << to_string(code)
                << " is already occupied. Skipping duplicate registration.\n";
      continue;
    }
    m_parsers[code] = parser_module;
  }
}

std::expected<void, ParserError>
QualcommParser::on_log_packet(std::string_view raw_frame) {
  if (raw_frame.size() < 14)
    return std::unexpected(ParserError::PacketTooShort);

  uint8_t byte_four = static_cast<uint8_t>(raw_frame[4]);
  uint8_t byte_five = static_cast<uint8_t>(raw_frame[5]);

  LogCode log_code = static_cast<LogCode>(byte_four | (byte_five << 8));

  std::string_view payload = raw_frame.substr(14);

  if (auto it = m_parsers.find(log_code); it != m_parsers.end()) {
    auto result = it->second->parse(log_code, payload, 0);

    if (result.has_value()) {
      emit_update();
      return {};
    }
    return result;
  }

  return std::unexpected(ParserError::WrongLogCode);
}

void QualcommParser::emit_update() {
  if (!m_monitor_cb)
    return;

  std::vector<CellIdentity> all_cells;

  for (const auto &[code, parser] : m_parsers) {
    auto cells = parser->get_cells();
    for (const auto &cell : cells) {
      // Ищем соту в итоговом векторе по PCI и частоте
      auto it = std::find_if(
          all_cells.begin(), all_cells.end(), [&cell](const CellIdentity &c) {
            return c.pci_bsic == cell.pci_bsic && c.freq == cell.freq;
          });

      // Собираем и дедуплицируем соты со всех активных плагинов
      if (it == all_cells.end()) {
        all_cells.push_back(cell);
      } else {
        // Если физика уже была в базе, дополняем её паспортом RRC из SIB1
        if (cell.cell_id > 0) {
          it->cell_id = cell.cell_id;
          it->tac = cell.tac;
          it->rat = cell.rat;
          it->mcc = cell.mcc;
          it->mnc = cell.mnc;
        }
        if (cell.is_serving) {
          it->is_serving = true;
        }
      }
    }
  }

  m_monitor_cb(all_cells);
}

} // namespace observer_qcom_parser
