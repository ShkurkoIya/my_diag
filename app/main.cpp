#include <iomanip>
#include <iostream>
#include <span>

#include "core/QualcomParser.h"

// ============================================================================
// Synthetic DIAG LOG_F test packets for all 4 RATs
// Format: [14-byte DIAG header] [RAT-specific payload]
// ============================================================================

// --- LTE 0xB17F: ML1 Serving v4, EARFCN=2660, PCI=72, RSRP=-80, RSRQ=-20 ---
const uint8_t lte_ml1[] = {
    0x10, 0x00, 0x26, 0x00, 0x7F, 0xB1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x64, 0x0A, 0x00, 0x24, 0x40, 0x06, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x90, 0x01, 0x00,
};

// --- LTE 0xB0C2: Serving Cell Info v2, MCC=250, MNC=01, TAC=1234, CID=27440068 ---
const uint8_t lte_serv[] = {
    0x10, 0x00, 0x2C, 0x00, 0xC2, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x48, 0x00, 0x64, 0x0A, 0x94, 0x50, 0x0A, 0x0A, 0xC4, 0xB3, 0xA2,
    0x01, 0xD2, 0x04, 0x07, 0x00, 0x00, 0x00, 0xFA, 0x00, 0x02, 0x01, 0x00, 0x01,
};

// --- NR 0xB992: ML1 Serving Cell, 1 subpacket ---
// Container: minor=1, major=2, num_subpkts=1, reserved=0
// Subpkt: id=0, ver=1, size=28 (4 header + 24 data)
// Data: NRARFCN=620000 (0x097640, 24 bits), PCI=101 (10 bits at +4)
//       RSRP raw=1280 at +12 (12 bits) -> 1280*0.0625-180 = -100 dBm
//       RSRQ raw=240 at +16 (12 bits) -> 240*0.0625-30 = -15 dB
//       SINR raw=480 at +16 bits[12:21] (10 bits) -> 480*0.0625-20 = 10 dB
const uint8_t nr_ml1_serving[] = {
    0x10,
    0x00,
    0x2C,
    0x00,
    0x92,
    0xB9,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    0x00,
    // Container header
    0x01,
    0x02,
    0x01,
    0x00,
    // Subpacket header: id=0, ver=1, size=28
    0x00,
    0x01,
    0x1C,
    0x00,
    // sp_data[0..3]: NRARFCN=620000 in low 24 bits = 0x40, 0x76, 0x09, 0x00
    0x40,
    0x76,
    0x09,
    0x00,
    // sp_data[4..7]: PCI=101 in low 10 bits = 0x65, 0x00, 0x00, 0x00
    0x65,
    0x00,
    0x00,
    0x00,
    // sp_data[8..11]: padding
    0x00,
    0x00,
    0x00,
    0x00,
    // sp_data[12..15]: RSRP=1280 in low 12 bits = 0x00, 0x05, 0x00, 0x00
    0x00,
    0x05,
    0x00,
    0x00,
    // sp_data[16..19]: RSRQ=240 in bits[0:11], SINR=480 in bits[12:21]
    // 240 = 0x0F0, 480 = 0x1E0. Combined: (480 << 12) | 240 = 0x1E00F0
    0xF0,
    0x00,
    0x1E,
    0x00,
    // sp_data[20..23]: padding
    0x00,
    0x00,
    0x00,
    0x00,
};

// --- GSM 0x5134: Cell Info, ARFCN=100, BSIC=43, MCC=250, MNC=01, LAC=5000, CID=4660 ---
const uint8_t gsm_cell[] = {
    0x10, 0x00, 0x1B, 0x00, 0x34, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x64, 0x00, 0x03, 0x05, 0x34, 0x12, 0x52, 0xF0, 0x10, 0x13, 0x88, 0x00, 0xFF,
};

// --- GSM 0x5071: Surround DB, 3 neighbor cells ---
const uint8_t gsm_surround[] = {
    0x10, 0x00, 0x32, 0x00, 0x71, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x03, 0x50, 0x00, 0xE0, 0xFC, 0x01, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x55, 0x00, 0x40, 0xFC, 0x01, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x5A, 0x00, 0xA0, 0xFB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// --- WCDMA 0x4027: Cell ID, MCC=250, MNC=99, LAC=2000, CID=11259375, PSC=72 ---
const uint8_t wcdma_cell[] = {
    0x10, 0x00, 0x34, 0x00, 0x27, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8C, 0x25,
    0x00, 0x00, 0x42, 0x29, 0x00, 0x00, 0xEF, 0xCD, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x04,
    0x02, 0x05, 0x00, 0x09, 0x09, 0x0F, 0xD0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// --- WCDMA 0x4005: Resel Rank v0, serving + 1 neighbor ---
const uint8_t wcdma_resel[] = {
    0x10, 0x00, 0x22, 0x00, 0x05, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x42, 0x29, 0x48, 0x00, 0x64, 0x00, 0x00, 0xE0,
    0x00, 0x00, 0x42, 0x29, 0x96, 0x00, 0x50, 0x00, 0x00, 0xD0, 0x00, 0x00,
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
    std::cout << std::fixed << std::setprecision(1) << " | RSRP=" << s->rsrp << " RSRQ=" << s->rsrq;
    if (s->sinr != 0) std::cout << " SINR=" << s->sinr;
    if (s->rssi != 0) std::cout << " RSSI=" << s->rssi;
  }
  if (auto* s = cell.signal.get_if<QCom::NrSignalParams>()) {
    std::cout << std::fixed << std::setprecision(1) << " | SS-RSRP=" << s->ss_rsrp
              << " SS-RSRQ=" << s->ss_rsrq << " SS-SINR=" << s->ss_sinr;
  }
  if (auto* s = cell.signal.get_if<QCom::WcdmaSignalParams>()) {
    std::cout << std::fixed << std::setprecision(0) << " | RSCP=" << s->rscp << " EcIo=" << s->ecio;
  }
  if (auto* s = cell.signal.get_if<QCom::GsmSignalParams>()) {
    std::cout << " | RxLev=" << static_cast<int>(s->rxlev) << " dBm";
  }

  if (auto* r = cell.radio.get_if<QCom::LteRadioParams>()) {
    if (r->dl_bw) std::cout << " BW=" << static_cast<int>(r->dl_bw) << "MHz";
    if (r->freq_band_ind) std::cout << " B" << static_cast<int>(r->freq_band_ind);
  }
  if (auto* r = cell.radio.get_if<QCom::GsmRadioParams>()) {
    if (r->bsic) std::cout << " BSIC=" << static_cast<int>(r->bsic);
    if (r->rxlev_access_min) std::cout << " rxlev_min=" << static_cast<int>(r->rxlev_access_min);
    if (r->cell_reselect_offset) std::cout << " CRO=" << static_cast<int>(r->cell_reselect_offset);
    if (r->ncc_permitted != 0xFF)
      std::cout << " NCC_perm=0x" << std::hex << static_cast<int>(r->ncc_permitted) << std::dec;
  }
  if (auto* r = cell.radio.get_if<QCom::WcdmaRadioParams>()) {
    if (r->psc) std::cout << " PSC=" << r->psc;
    if (r->dl_uarfcn) std::cout << " DL=" << r->dl_uarfcn;
    if (r->ul_uarfcn) std::cout << " UL=" << r->ul_uarfcn;
  }
  if (auto* r = cell.radio.get_if<QCom::NrRadioParams>()) {
    if (r->q_rx_lev_min) std::cout << " q_rx_lev_min=" << static_cast<int>(r->q_rx_lev_min);
    if (r->ranac) std::cout << " RANAC=" << r->ranac;
  }

  // Passport extra fields
  if (cell.passport.csg_ind) std::cout << " [CSG:" << cell.passport.csg_id << "]";
  if (!cell.passport.intra_freq_reselection_allowed) std::cout << " [NO_RESEL]";
  if (cell.passport.q_rx_lev_min)
    std::cout << " q_min=" << static_cast<int>(cell.passport.q_rx_lev_min);

  std::cout << "\n";

  // --- Neighbor details ---
  for (const auto& n : cell.radio.gsm_neighbors) {
    std::cout << "    └ GSM neigh: ARFCN=" << n.arfcn << " RxLev=" << n.rxlev << " dBm";
    if (n.bsic_valid) std::cout << " BSIC=" << static_cast<int>(n.bsic);
    std::cout << "\n";
  }
  for (const auto& n : cell.radio.wcdma_neighbors) {
    std::cout << "    └ WCDMA neigh: UARFCN=" << n.uarfcn << " PSC=" << n.psc;
    if (n.rscp) std::cout << " RSCP=" << n.rscp;
    if (n.ecio) std::cout << " EcIo=" << n.ecio;
    std::cout << "\n";
  }
  for (const auto& n : cell.radio.intra_freq_neighbors) {
    std::cout << "    └ LTE intra: PCI=" << n.pci << " q_offset=" << static_cast<int>(n.q_offset)
              << "\n";
  }
  for (const auto& n : cell.radio.inter_freq_carriers) {
    std::cout << "    └ LTE inter: EARFCN=" << n.earfcn
              << " thresh_high=" << static_cast<int>(n.thresh_x_high)
              << " thresh_low=" << static_cast<int>(n.thresh_x_low) << "\n";
  }
  for (const auto& n : cell.radio.utra_neighbors) {
    std::cout << "    └ UTRA neigh: UARFCN=" << n.uarfcn
              << " q_rx_lev_min=" << static_cast<int>(n.q_rx_lev_min) << "\n";
  }
  for (const auto& n : cell.radio.geran_neighbors) {
    std::cout << "    └ GERAN neigh: ARFCN=" << n.arfcn_start
              << " thresh_high=" << static_cast<int>(n.thresh_x_high) << "\n";
  }
  for (const auto& n : cell.radio.meas_neighbors) {
    std::cout << "    └ Meas neigh: PCI=" << n.pci << " RSRP_idx=" << static_cast<int>(n.rsrp)
              << " RSRQ_idx=" << static_cast<int>(n.rsrq) << "\n";
  }
}

void feed(QCom::QualcomParser& p, const char* name, std::span<const uint8_t> data) {
  std::cout << "[Feed] " << name << "\n";
  auto r = p.on_diag_frame(data);
  if (!r) std::cout << "  -> " << QCom::to_string(r.error()) << "\n";
}

}  // namespace

int main() {
  std::cout << "=== QCom Scanner — Multi-RAT Pipeline Test ===\n\n";

  QCom::QualcomParser parser;

  std::cout << "────── LTE ──────\n";
  feed(parser, "LTE ML1 Serving 0xB17F (RSRP=-80, RSRQ=-20)", lte_ml1);
  feed(parser, "LTE Serving Cell 0xB0C2 (250-01, TAC=1234, B7)", lte_serv);

  std::cout << "\n────── NR ──────\n";
  feed(parser, "NR ML1 Serving 0xB992 (NRARFCN=620000, PCI=101)", nr_ml1_serving);

  std::cout << "\n────── GSM ──────\n";
  feed(parser, "GSM Cell Info 0x5134 (ARFCN=100, BSIC=43, 250-01)", gsm_cell);
  feed(parser, "GSM Surround DB 0x5071 (3 neighbors)", gsm_surround);

  std::cout << "\n────── WCDMA ──────\n";
  feed(parser, "WCDMA Cell ID 0x4027 (250-99, PSC=72)", wcdma_cell);
  feed(parser, "WCDMA Resel Rank 0x4005 (serving + 1 neighbor)", wcdma_resel);

  std::cout << "\n════════════════════════════════════════════\n";
  std::cout << "[Final] " << parser.tracker().cell_count() << " cell(s):\n\n";
  for (const auto& cell : parser.tracker().get_snapshot()) print_cell(cell);

  return 0;
}
