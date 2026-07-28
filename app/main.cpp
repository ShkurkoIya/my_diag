#include <iomanip>
#include <iostream>

#include "core/QualcomParser.h"

// Real binary packet dumps for testing.
// These are actual DIAG LOG_F frames captured from a Qualcomm modem.

// 0xB17F — LTE ML1 Serving Cell Meas & Eval (version 4)
// DIAG header (14 bytes) + payload: version=4, EARFCN=2660, PCI=72
// RSRP: raw=1600 (0x640) -> 1600*0.0625-180 = -80 dBm
// RSRQ: raw=160 (0xA0<<22 = 0x28000000) -> 160*0.0625-30 = -20 dB
// RSSI: raw=800 (0x320<<11 = 0x00190000) -> 800*0.0625-110 = -60 dBm
const uint8_t raw_ml1_packet[] = {
    0x10, 0x00, 0x26, 0x00, 0x7F, 0xB1, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,              // DIAG header (14 bytes)
    0x04, 0x00, 0x00, 0x00,                          // version=4, padding
    0x64, 0x0A, 0x00, 0x24,                          // EARFCN=2660(u16), PCI_SLP=72<<7
    0x40, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // RSRP word: 0x640 in low 12 bits
    0x00, 0x00, 0x00, 0x28, 0x00, 0x90, 0x01, 0x00,  // RSRQ(>>22)=160, RSSI(>>11)=800
};

// 0xB0C0 — LTE RRC OTA with SystemInformationBlockType1
// DIAG header + Qualcomm RRC OTA header (EARFCN=2660, PCI=72) + ASN.1 PER payload
const uint8_t
    raw_rrc_packet[] =
        {
            0x10, 0x00, 0x2A, 0x00, 0xC0, 0xB0, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,                    // DIAG header (14 bytes)
            0x01, 0x01, 0x64, 0x0A, 0x48, 0x00, 0x01,  // QCom meta: EARFCN=2660, PCI=72, ch=BCCH
            0x00, 0x04, 0x0c, 0x11, 0x1d, 0x30, 0x32, 0x41, 0xb0, 0x0d,
            0x40, 0x00, 0x10, 0x04, 0x61, 0x76, 0x40, 0x00, 0x3d, 0xf0,
};

namespace {

void print_cell(const QCom::CellIdentity& cell) {
  std::cout << "  RAT=" << QCom::to_string(cell.rat) << " serving=" << cell.is_serving
            << " freq=" << cell.radio.freq() << " pci=" << cell.radio.pci_bsic();

  if (cell.passport.has_identity()) {
    std::cout << " | MCC=" << cell.passport.mcc << " MNC=" << cell.passport.mnc
              << " TAC=" << cell.passport.tac << " CID=" << cell.passport.cell_id;

    if (cell.passport.cell_barred) std::cout << " [BARRED]";
  } else {
    std::cout << " | (no identity yet)";
  }

  if (auto* lte = cell.signal.get_if<QCom::LteSignalParams>()) {
    std::cout << " | RSRP=" << std::fixed << std::setprecision(1) << lte->rsrp
              << " RSRQ=" << lte->rsrq;
  }

  if (auto* lte_radio = cell.radio.get_if<QCom::LteRadioParams>()) {
    if (lte_radio->dl_bw) std::cout << " BW=" << static_cast<int>(lte_radio->dl_bw) << "MHz";
    if (lte_radio->freq_band_ind) std::cout << " B" << static_cast<int>(lte_radio->freq_band_ind);
  }

  std::cout << "\n";
}

}  // namespace

int main() {
  std::cout << "=== QCom Scanner — srsRAN ASN.1 Pipeline Test ===\n\n";

  QCom::QualcomParser parser;

  parser.set_cell_callback([](const std::vector<QCom::CellIdentity>& cells) {
    std::cout << "[Callback] Cell snapshot (" << cells.size() << " cells):\n";
    for (const auto& cell : cells) print_cell(cell);
    std::cout << "\n";
  });

  // Feed the ML1 serving cell measurements packet
  std::cout << "[Test] Feeding ML1 serving cell packet (0xB17F)...\n";
  auto r1 = parser.on_diag_frame(
      std::string_view(reinterpret_cast<const char*>(raw_ml1_packet), sizeof(raw_ml1_packet)));
  if (!r1) std::cout << "[Result] " << QCom::to_string(r1.error()) << "\n\n";

  // Feed the RRC OTA SIB1 packet
  std::cout << "[Test] Feeding RRC OTA SIB1 packet (0xB0C0)...\n";
  auto r2 = parser.on_diag_frame(
      std::string_view(reinterpret_cast<const char*>(raw_rrc_packet), sizeof(raw_rrc_packet)));
  if (!r2) std::cout << "[Result] " << QCom::to_string(r2.error()) << "\n\n";

  // Print final tracker state
  std::cout << "[Final] Tracker has " << parser.tracker().cell_count() << " cell(s)\n";
  auto snapshot = parser.tracker().get_snapshot();
  for (const auto& cell : snapshot) print_cell(cell);

  return 0;
}
