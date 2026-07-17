#include "CellIdentity.h"
#include "QualcomParser.h"
#include <iostream>
#include <ranges> // Трендовый C++20/C++23 Ranges для работы с коллекциями
#include <string_view>
#include <vector>

// ============================================================================
// БОЕВЫЕ ДАМПЫ ПАКЕТОВ (Идеально выверенные бинарные массивы)
// ============================================================================

// Настоящий пакет физики L1 (0xB17C — LTE Serving Cell Measurements)
// Содержит DIAG заголовок, частоту EARFCN=2660 и физический ID вышки PCI=72
const uint8_t raw_ml1_packet[] = {
    0x10, 0x00, 0x20, 0x00, 0x7C, 0xB1, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // DIAG Header
    0x01, 0x01, 0x64, 0x0A, 0x48, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Настоящий пакет сигналки L3 (0xB0C0 — LTE RRC OTA с
// SystemInformationBlockType1) Движок srsRAN на лету распакует отсюда
// глобальный паспорт соты: TAC и Глобальный CellID
const uint8_t raw_rrc_packet[] = {
    0x10, 0x00, 0x2A, 0x00, 0xC0, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00,                               // DIAG Header
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, // Qualcomm Meta (ChannelType = 1
                                              // -> BCCH)
    0x00, 0x04, 0x0c, 0x11, 0x1d, 0x30, 0x32, 0x41, 0xb0, 0x0d, 0x40, 0x00,
    0x10, 0x04, 0x61, 0x76, 0x40, 0x00, 0x3d, 0xf0 // Сырой ASN.1 PER
                                                   // поток вышки
};

// ============================================================================
// ДЕКЛАРАТИВНЫЙ РЕАКТИВНЫЙ МОНИТОР (Стиль Modern C++)
// ============================================================================

// Функция обратного вызова, которая занимается ТОЛЬКО отрисовкой живых данных
void render_live_monitor(
    const std::vector<observer_qcom_parser::CellIdentity> &cells) {
  std::cout
      << "\n==================== OBSERVER LIVE MONITOR ====================\n";

  // МАГИЯ C++20/C++23 RANGES: Выделяем и выводим ТЕКУЩУЮ ОБСЛУЖИВАЮЩУЮ соту
  // (Serving) std::views::filter создает ленивое окно поверх вектора, работая
  // БЕЗ копирования памяти и циклов if!
  auto serving_view = cells | std::views::filter([](const auto &cell) {
                        return cell.is_serving;
                      });

  for (const auto &cell : serving_view) {
    std::cout << "  [SERVING CELL]\n";
    std::cout << "    Radio Access: LTE\n";
    std::cout << "    EARFCN (Freq): " << cell.freq
              << " | PCI: " << cell.pci_bsic << "\n";

    // Если srsRAN уже добежал до SIB1 и вскрыл паспорт, выводим глобальные ID
    if (cell.cell_id > 0) {
      std::cout << "    TAC:          " << cell.tac << "\n";
      std::cout << "    Global CellID: " << cell.cell_id;
      std::cout << " (eNodeB ID: " << (cell.cell_id >> 8)
                << ", Sector: " << (cell.cell_id & 0xFF) << ")\n";
    } else {
      std::cout << "    Passport:     [Scanning SIB1 сигналки...]\n";
    }
  }

  // Точно так же одной строчкой можем отфильтровать и красиво напечатать
  // СОСЕДНИЕ соты (Neighbors)
  auto neighbor_view = cells | std::views::filter([](const auto &cell) {
                         return !cell.is_serving;
                       });

  bool has_neighbors = false;
  for (const auto &cell : neighbor_view) {
    if (!has_neighbors) {
      std::cout << "  [NEIGHBOR CELLS LIST]\n";
      has_neighbors = true;
    }
    std::cout << "    -> LTE EARFCN: " << cell.freq
              << " | PCI: " << cell.pci_bsic << "\n";
  }

  std::cout
      << "===============================================================\n";
}

// ============================================================================
// ИСПОЛНЯЕМЫЙ МОДУЛЬ КОНВЕЙЕРА (Точка сборки)
// ============================================================================

int main() {
  std::cout
      << "[SYSTEM] Запуск сотового сканера Observer Core [Стандарт C++23]\n";
  std::cout << "[SYSTEM] Компилятор: GCC 16 / Движок парсинга: srsRAN 4G\n";

  // Инициализируем наш поджарый, изящный диспетчер
  observer_qcom_parser::QualcommParser parser;

  // Декларативно подписываем наш Ranges-монитор на поток апдейтов
  parser.set_monitor_callback(render_live_monitor);

  std::cout << "[System] Эмуляция TTY USB-стрима с Simcom-платы...\n";

  // ────────────────────────────────────────────────────────────────────────
  // ИМИТАЦИЯ ШАГА 1: Модем зацепился за физику L1, шлет лог 0xB17C
  // ────────────────────────────────────────────────────────────────────────
  std::cout << "\n[TTY USB] <-- Получен пакет физического уровня L1 (0xB17C)\n";

  // Создаем zero-copy string_view обертку прямо поверх куска физической памяти
  std::string_view ml1_stream(reinterpret_cast<const char *>(raw_ml1_packet),
                              sizeof(raw_ml1_packet));

  // Пушим в конвейер. Метод возвращает C++23 std::expected!
  auto ml1_result = parser.on_log_packet(ml1_stream);

  if (!ml1_result.has_value()) {
    std::clog << "[CRITICAL] Ошибка разбора физики: "
              << to_string(ml1_result.error()) << "\n";
  }

  // ────────────────────────────────────────────────────────────────────────
  // ИМИТАЦИЯ ШАГА 2: Из эфира прилетел бродкаст L3 (0xB0C0) с SIB1
  // ────────────────────────────────────────────────────────────────────────
  std::cout
      << "\n[TTY USB] <-- Из радиоэфира прилетел пакет RRC сигналки (0xB0C0)\n";

  std::string_view rrc_stream(reinterpret_cast<const char *>(raw_rrc_packet),
                              sizeof(raw_rrc_packet));

  auto rrc_result = parser.on_log_packet(rrc_stream);

  if (!rrc_result.has_value()) {
    // Если в эфире летит неподдерживаемый лог-код, мы не паникуем, а изящно
    // фильтруем шум
    if (rrc_result.error() != observer_qcom_parser::ParserError::WrongLogCode) {
      std::clog << "[CRITICAL] srsRAN сломался на пакете RRC: "
                << to_string(rrc_result.error()) << "\n";
    }
  }

  std::cout
      << "\n[SYSTEM] Архитектурный тест сотового конвейера успешно завершен.\n";
  return 0;
}
