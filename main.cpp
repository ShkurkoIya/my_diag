#include "CellIdentity.h"
#include "QualcomParser.h"

#include <iostream>

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

int main() {

  std::cout << "Qualcom monitor" << std::endl;

  return 0;
}
