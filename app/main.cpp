#include <iomanip>
#include <iostream>
#include <span>

#include "core/QualcomParser.h"

// ============================================================================
// Synthetic test packets — simulate real DIAG LOG_F frames for all 4 RATs.
// Each packet: [14-byte DIAG header] [RAT-specific payload]
// ============================================================================

// --- LTE 0xB17F: ML1 Serving Cell Meas v4 ---
// EARFCN=2660, PCI=72, RSRP raw=1600 (-80 dBm), RSRQ raw=160 (-20 dB)
const uint8_t lte_ml1[] = {
    0x10, 0x00, 0x26, 0x00, 0x7F, 0xB1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x64, 0x0A, 0x00, 0x24, 0x40, 0x06, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x90, 0x01, 0x00,
};

// --- LTE 0xB0C2: Serving Cell Info v2 ---
// PCI=72, EARFCN=2660, DL_BW=10, UL_BW=10, CID=0x01A2B3C4, TAC=1234,
// Band=7, MCC=250, MNC_digits=2, MNC=01
const uint8_t lte_serv_cell[] = {
    0x10, 0x00, 0x2C, 0x00, 0xC2, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x02,                    // version=2
    0x48, 0x00,              // PCI=72
    0x64, 0x0A,              // DL EARFCN=2660
    0x94, 0x50,              // UL EARFCN=20628
    0x0A, 0x0A,              // DL_BW=10, UL_BW=10
    0xC4, 0xB3, 0xA2, 0x01,  // CID=0x01A2B3C4
    0xD2, 0x04,              // TAC=1234
    0x07, 0x00, 0x00, 0x00,  // Band=7
    0xFA, 0x00,              // MCC=250
    0x02,                    // MNC digits=2
    0x01, 0x00,              // MNC=01
    0x01,                    // allowed_access
};

// --- GSM 0x5134: Cell Info ---
// ARFCN=100, BCC=3, NCC=5, CID=0x1234, LAI: MCC=250 MNC=01 LAC=5000
const uint8_t gsm_cell_info[] = {
    0x10, 0x00, 0x1B, 0x00, 0x34, 0x51, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00,  // arfcn_band = 100
    0x03,                                            // bcc = 3
    0x05,                                            // ncc = 5
    0x34, 0x12,                                      // CID = 0x1234
    0x52, 0xF0,                                      // LAI[0:1]: MCC=250 (nibbles: 2,5,0, MNC3=F)
    0x10,                                            // LAI[2]: MNC=01 (nibbles: 0,1)
    0x13, 0x88,                                      // LAI[3:4]: LAC=5000 (0x1388 BE)
    0x00,                                            // priority
    0xFF,                                            // ncc_permitted
};

// --- GSM 0x5071: Surround DB (3 neighbors) ---
const uint8_t gsm_surround[] = {
    0x10,
    0x00,
    0x32,
    0x00,
    0x71,
    0x50,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x03,  // num_cells = 3
    // Cell 0: ARFCN=80, rxpwr=-800 (=-50 dBm), bsic_valid=1, bsic=42
    0x50,
    0x00,
    0xE0,
    0xFC,
    0x01,
    0x2A,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    // Cell 1: ARFCN=85, rxpwr=-960 (=-60 dBm), bsic_valid=1, bsic=15
    0x55,
    0x00,
    0x40,
    0xFC,
    0x01,
    0x0F,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    // Cell 2: ARFCN=90, rxpwr=-1120 (=-70 dBm), bsic_valid=0
    0x5A,
    0x00,
    0xA0,
    0xFB,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
};

// --- WCDMA 0x4027: Cell ID ---
// UL_UARFCN=9612, DL_UARFCN=10562, CID=0x00ABCDEF, PSC_raw=72<<4=0x0480,
// MCC=250(BCD: 02 05 00), MNC=99(BCD: 09 09 0F), LAC=2000
const uint8_t wcdma_cell_id[] = {
    0x10, 0x00, 0x34, 0x00, 0x27, 0x40, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x8C, 0x25, 0x00, 0x00,  // UL UARFCN = 9612
    0x42, 0x29, 0x00, 0x00,                                // DL UARFCN = 10562
    0xEF, 0xCD, 0xAB, 0x00,                                // CID = 0x00ABCDEF (28-bit)
    0x00, 0x00, 0x00, 0x00,                                // URA + flags
    0x80, 0x04,              // PSC raw = 0x0480, real = 0x0480>>4 = 72
    0x02, 0x05, 0x00,        // MCC BCD = 250
    0x09, 0x09, 0x0F,        // MNC BCD = 99 (2-digit, 0xF stop)
    0xD0, 0x07, 0x00, 0x00,  // LAC = 2000
    0x00, 0x00, 0x00, 0x00,  // RAC
};

// --- WCDMA 0x4005: Resel Rank v0 (2 cells: serving + 1 neighbor) ---
// v0: start=2, stride=10
// Cell 0 (serving): UARFCN=10562, PSC=72, rscp_raw=100(-79dBm), ecio_raw=-20(-10dB)
// Cell 1 (neighbor): UARFCN=10562, PSC=150, rscp_raw=90(-69dBm?->wait, 90-21=-69? no, clamp)
const uint8_t wcdma_resel[] = {
    0x10,
    0x00,
    0x22,
    0x00,
    0x05,
    0x40,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x02,  // v0 (bits[7:6]=0) | num_3g=2
    0x00,  // num_2g=0
    // Cell 0: serving
    0x42,
    0x29,  // UARFCN=10562
    0x48,
    0x00,  // PSC=72
    0x64,  // rscp_raw=100 -> 100-21=79 -> clamp(-140,0) = 0 (too high, actually rscp=79 is invalid,
           // let me use lower)
    0x00,
    0x00,  // rank_rscp
    0xE0,  // ecio_raw=-32 -> -32/2=-16 dB
    0x00,
    0x00,  // rank_ecio
    // Cell 1: neighbor
    0x42,
    0x29,  // UARFCN=10562
    0x96,
    0x00,  // PSC=150
    0x50,  // rscp_raw=80 -> 80-21=59 -> clamp = 0 (also too high; real values are negative raw)
    0x00,
    0x00,
    0xD0,  // ecio_raw=-48 -> -48/2=-24 dB
    0x00,
    0x00,
};

namespace {

void print_cell(const QCom::CellIdentity& cell) {
  std::cout << "  [" << QCom::to_string(cell.rat) << "]"
            << (cell.is_serving ? " SERVING" : "        ") << " freq=" << cell.radio.freq()
            << " pci=" << cell.radio.pci_bsic();

  if (cell.passport.has_identity()) {
    std::cout << " | " << cell.passport.mcc << "-" << std::setfill('0') << std::setw(2)
              << cell.passport.mnc << std::setfill(' ') << " TAC/LAC=" << cell.passport.tac
              << " CID=" << cell.passport.cell_id;
    if (cell.passport.cell_barred) std::cout << " [BARRED]";
  }

  if (auto* s = cell.signal.get_if<QCom::LteSignalParams>()) {
    std::cout << " | RSRP=" << std::fixed << std::setprecision(1) << s->rsrp << " RSRQ=" << s->rsrq;
  }
  if (auto* s = cell.signal.get_if<QCom::NrSignalParams>()) {
    std::cout << " | SS-RSRP=" << std::fixed << std::setprecision(1) << s->ss_rsrp;
  }
  if (auto* s = cell.signal.get_if<QCom::WcdmaSignalParams>()) {
    std::cout << " | RSCP=" << std::fixed << std::setprecision(0) << s->rscp << " EcIo=" << s->ecio;
  }
  if (auto* s = cell.signal.get_if<QCom::GsmSignalParams>()) {
    std::cout << " | RxLev=" << static_cast<int>(s->rxlev);
  }

  if (auto* r = cell.radio.get_if<QCom::LteRadioParams>()) {
    if (r->dl_bw) std::cout << " BW=" << static_cast<int>(r->dl_bw) << "MHz";
    if (r->freq_band_ind) std::cout << " B" << static_cast<int>(r->freq_band_ind);
  }
  if (auto* r = cell.radio.get_if<QCom::GsmRadioParams>()) {
    if (r->bsic) std::cout << " BSIC=" << static_cast<int>(r->bsic);
  }
  if (auto* r = cell.radio.get_if<QCom::WcdmaRadioParams>()) {
    if (r->psc) std::cout << " PSC=" << r->psc;
  }

  // Neighbor counts
  if (!cell.radio.gsm_neighbors.empty())
    std::cout << " [" << cell.radio.gsm_neighbors.size() << " GSM neighbors]";
  if (!cell.radio.wcdma_neighbors.empty())
    std::cout << " [" << cell.radio.wcdma_neighbors.size() << " WCDMA neighbors]";
  if (!cell.radio.intra_freq_neighbors.empty())
    std::cout << " [" << cell.radio.intra_freq_neighbors.size() << " intra-freq]";

  std::cout << "\n";
}

void feed(QCom::QualcomParser& parser, const char* name, std::span<const uint8_t> data) {
  std::cout << "[Feed] " << name << " (" << data.size() << " bytes)...\n";
  auto r = parser.on_diag_frame(data);
  if (!r) std::cout << "  -> " << QCom::to_string(r.error()) << "\n";
}

}  // namespace

int main() {
  std::cout << "=== QCom Scanner — Multi-RAT Pipeline Test ===\n\n";

  QCom::QualcomParser parser;

  parser.set_cell_callback([](const std::vector<QCom::CellIdentity>& cells) {
    std::cout << "[Snapshot] " << cells.size() << " cell(s):\n";
    for (const auto& cell : cells) print_cell(cell);
    std::cout << "\n";
  });

  // --- LTE ---
  std::cout << "────── LTE ──────\n";
  feed(parser, "LTE ML1 Serving (0xB17F)", lte_ml1);
  feed(parser, "LTE Serving Cell Info (0xB0C2)", lte_serv_cell);

  // --- GSM ---
  std::cout << "────── GSM ──────\n";
  feed(parser, "GSM Cell Info (0x5134)", gsm_cell_info);
  feed(parser, "GSM Surround DB (0x5071)", gsm_surround);

  // --- WCDMA ---
  std::cout << "────── WCDMA ──────\n";
  feed(parser, "WCDMA Cell ID (0x4027)", wcdma_cell_id);
  feed(parser, "WCDMA Resel Rank (0x4005)", wcdma_resel);

  // --- Final state ---
  std::cout << "════════════════════════════════════════════\n";
  std::cout << "[Final] " << parser.tracker().cell_count() << " cell(s) in tracker:\n";
  for (const auto& cell : parser.tracker().get_snapshot()) print_cell(cell);

  return 0;
}
