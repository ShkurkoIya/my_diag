#include <iomanip>
#include <iostream>
#include <span>

#include "core/QualcomParser.h"

// ============================================================================
// Comprehensive synthetic DIAG packets — every RAT, every field populated.
// Order: identity first -> signal -> neighbors (so merge works correctly)
// ============================================================================

// === LTE ===

// 0xB0C2 v2: PCI=72, EARFCN=2660, BW=10/10, CID=0x01A2B3C4, TAC=1234, Band=7, 250-01
const uint8_t lte_identity[] = {
    0x10, 0x00, 0x2C, 0x00, 0xC2, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x02, 0x48, 0x00, 0x64, 0x0A, 0x94, 0x50, 0x0A, 0x0A, 0xC4, 0xB3, 0xA2,
    0x01, 0xD2, 0x04, 0x07, 0x00, 0x00, 0x00, 0xFA, 0x00, 0x02, 0x01, 0x00, 0x01,
};

// 0xB17F v4: EARFCN=2660, PCI=72, RSRP=-80, RSRQ=-20, RSSI=-60
const uint8_t lte_signal[] = {
    0x10, 0x00, 0x26, 0x00, 0x7F, 0xB1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x04, 0x00, 0x00, 0x00, 0x64, 0x0A, 0x00, 0x24, 0x40, 0x06, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x90, 0x01, 0x00,
};

// === NR ===

// 0xB992: NRARFCN=620000, PCI=101, SS-RSRP=-100, SS-RSRQ=-15, SS-SINR=10
const uint8_t nr_signal[] = {
    0x10, 0x00, 0x2C, 0x00, 0x92, 0xB9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02,
    0x01, 0x00, 0x00, 0x01, 0x1C, 0x00, 0x40, 0x76, 0x09, 0x00, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0xF0, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// === GSM ===

// 0x5134: ARFCN=100, BCC=3, NCC=5, CID=4660, 250-01, LAC=5000
const uint8_t gsm_identity[] = {
    0x10, 0x00, 0x1B, 0x00, 0x34, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x64, 0x00, 0x03, 0x05, 0x34, 0x12, 0x52, 0xF0, 0x10, 0x13, 0x88, 0x00, 0xFF,
};

// 0x506C: Burst Metrics, serving rxpwr=-1280 (raw) -> -80 dBm
const uint8_t gsm_signal[] = {
    0x10, 0x00, 0x22, 0x00, 0x6C, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFB,  // rxpwr = -1280
                                                                                   // (int16 LE) ->
                                                                                   // -1280*0.0625 =
                                                                                   // -80
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// 0x5071: 3 GSM neighbors
const uint8_t gsm_neighbors[] = {
    0x10, 0x00, 0x32, 0x00, 0x71, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x03, 0x50, 0x00, 0xE0, 0xFC, 0x01, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x55, 0x00, 0x40, 0xFC, 0x01, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x5A, 0x00, 0xA0, 0xFB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// === WCDMA ===

// 0x4027: 250-99, CID=11259375, LAC=2000, DL=10562, UL=9612, PSC=72
const uint8_t wcdma_identity[] = {
    0x10, 0x00, 0x34, 0x00, 0x27, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8C, 0x25,
    0x00, 0x00, 0x42, 0x29, 0x00, 0x00, 0xEF, 0xCD, 0xAB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x04,
    0x02, 0x05, 0x00, 0x09, 0x09, 0x0F, 0xD0, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// 0x4005 v1: serving (UARFCN=10562, PSC=72, rscp_raw=-100 -> -121dBm, ecio_raw=-32 -> -16dB)
//            + 1 neighbor (PSC=150, rscp_raw=-90 -> -111dBm, ecio_raw=-48 -> -24dB)
const uint8_t wcdma_signal[] = {
    0x10,
    0x00,
    0x24,
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
    0x42,
    0x00,  // v1 (bits[7:6]=1) | num_3g=2
    // Cell 0 (serving): stride=11
    0x42,
    0x29,  // UARFCN=10562
    0x48,
    0x00,  // PSC=72
    0x9C,  // rscp_raw=-100 (signed) -> -100-21=-121 -> clamp=-121
    0x00,
    0x00,  // rank_rscp
    0xE0,  // ecio_raw=-32 -> -32/2=-16
    0x00,
    0x00,  // rank_ecio
    0x00,  // resel_status (v1)
    // Cell 1 (neighbor): stride=11
    0x42,
    0x29,  // UARFCN=10562
    0x96,
    0x00,  // PSC=150
    0xA6,  // rscp_raw=-90 -> -90-21=-111 -> clamp=-111
    0x00,
    0x00,
    0xD0,  // ecio_raw=-48 -> -48/2=-24
    0x00,
    0x00,
    0x00,
};

namespace {

void print_cell(const QCom::CellIdentity& cell) {
  std::cout << "  [" << QCom::to_string(cell.rat) << "]"
            << (cell.is_serving ? " SERVING" : "        ") << " freq=" << cell.radio.freq()
            << " id=" << cell.radio.pci_bsic();

  // --- Identity ---
  if (cell.passport.has_identity()) {
    std::cout << " | " << cell.passport.mcc << "-" << std::setfill('0') << std::setw(2)
              << cell.passport.mnc << std::setfill(' ') << " TAC/LAC=" << cell.passport.tac
              << " CID=" << cell.passport.cell_id;
    if (cell.passport.cell_barred) std::cout << " [BARRED]";
    if (cell.passport.csg_ind) std::cout << " [CSG:" << cell.passport.csg_id << "]";
    if (!cell.passport.intra_freq_reselection_allowed) std::cout << " [NO_RESEL]";
    if (cell.passport.q_rx_lev_min)
      std::cout << " q_min=" << static_cast<int>(cell.passport.q_rx_lev_min);
    if (cell.passport.freq_band_ind)
      std::cout << " B" << static_cast<int>(cell.passport.freq_band_ind);
  }

  // --- Signal ---
  if (auto* s = cell.signal.get_if<QCom::LteSignalParams>()) {
    std::cout << std::fixed << std::setprecision(1) << "\n         signal: RSRP=" << s->rsrp
              << " RSRQ=" << s->rsrq;
    if (s->sinr != 0) std::cout << " SINR=" << s->sinr;
    if (s->rssi != 0) std::cout << " RSSI=" << s->rssi;
  }
  if (auto* s = cell.signal.get_if<QCom::NrSignalParams>()) {
    std::cout << std::fixed << std::setprecision(1) << "\n         signal: SS-RSRP=" << s->ss_rsrp
              << " SS-RSRQ=" << s->ss_rsrq << " SS-SINR=" << s->ss_sinr;
  }
  if (auto* s = cell.signal.get_if<QCom::WcdmaSignalParams>()) {
    std::cout << std::fixed << std::setprecision(0) << "\n         signal: RSCP=" << s->rscp
              << " EcIo=" << s->ecio;
  }
  if (auto* s = cell.signal.get_if<QCom::GsmSignalParams>()) {
    std::cout << "\n         signal: RxLev=" << static_cast<int>(s->rxlev) << " dBm"
              << " RxQual=" << static_cast<int>(s->rxqual);
  }

  // --- Radio params ---
  if (auto* r = cell.radio.get_if<QCom::LteRadioParams>()) {
    std::cout << "\n         radio:";
    if (r->dl_bw) std::cout << " DL_BW=" << static_cast<int>(r->dl_bw) << "MHz";
    if (r->ul_bw) std::cout << " UL_BW=" << static_cast<int>(r->ul_bw);
    if (r->freq_band_ind) std::cout << " Band=" << static_cast<int>(r->freq_band_ind);
    if (r->q_hyst) std::cout << " q_hyst=" << static_cast<int>(r->q_hyst);
    if (r->t_resel_eutra) std::cout << " t_resel=" << static_cast<int>(r->t_resel_eutra);
    if (r->sfn) std::cout << " SFN=" << r->sfn;
    if (r->ac_barr_emergency) std::cout << " [AC_BARR_EMERG]";
  }
  if (auto* r = cell.radio.get_if<QCom::GsmRadioParams>()) {
    std::cout << "\n         radio: BSIC=" << static_cast<int>(r->bsic)
              << " NCC=" << static_cast<int>(r->ncc) << " BCC=" << static_cast<int>(r->bcc);
    if (r->rxlev_access_min) std::cout << " rxlev_min=" << static_cast<int>(r->rxlev_access_min);
    if (r->ms_txpwr_max_cch) std::cout << " txpwr_max=" << static_cast<int>(r->ms_txpwr_max_cch);
    if (r->cell_reselect_offset) std::cout << " CRO=" << static_cast<int>(r->cell_reselect_offset);
    if (r->penalty_time) std::cout << " penalty=" << static_cast<int>(r->penalty_time);
    if (r->ncc_permitted != 0xFF)
      std::cout << " NCC_perm=0x" << std::hex << static_cast<int>(r->ncc_permitted) << std::dec;
  }
  if (auto* r = cell.radio.get_if<QCom::WcdmaRadioParams>()) {
    std::cout << "\n         radio: PSC=" << r->psc << " DL_UARFCN=" << r->dl_uarfcn
              << " UL_UARFCN=" << r->ul_uarfcn;
  }
  if (auto* r = cell.radio.get_if<QCom::NrRadioParams>()) {
    std::cout << "\n         radio: NRARFCN=" << r->nrarfcn << " PCI=" << r->pci;
    if (r->q_rx_lev_min) std::cout << " q_rx_lev_min=" << static_cast<int>(r->q_rx_lev_min);
    if (r->q_qual_min) std::cout << " q_qual_min=" << static_cast<int>(r->q_qual_min);
    if (r->ranac) std::cout << " RANAC=" << r->ranac;
  }

  std::cout << "\n";

  // --- Neighbors ---
  for (const auto& n : cell.radio.gsm_neighbors) {
    std::cout << "    └─ GSM  ARFCN=" << n.arfcn << " RxLev=" << n.rxlev << "dBm";
    if (n.bsic_valid) std::cout << " BSIC=" << static_cast<int>(n.bsic);
    std::cout << "\n";
  }
  for (const auto& n : cell.radio.wcdma_neighbors) {
    std::cout << "    └─ WCDMA UARFCN=" << n.uarfcn << " PSC=" << n.psc;
    if (n.rscp) std::cout << " RSCP=" << n.rscp << "dBm";
    if (n.ecio) std::cout << " EcIo=" << n.ecio / 2 << "dB";
    std::cout << "\n";
  }
  for (const auto& n : cell.radio.intra_freq_neighbors)
    std::cout << "    └─ LTE intra PCI=" << n.pci << " q_offset=" << static_cast<int>(n.q_offset)
              << "\n";
  for (const auto& n : cell.radio.inter_freq_carriers)
    std::cout << "    └─ LTE inter EARFCN=" << n.earfcn
              << " prio=" << static_cast<int>(n.cell_resel_prio)
              << " thresh_hi=" << static_cast<int>(n.thresh_x_high)
              << " thresh_lo=" << static_cast<int>(n.thresh_x_low) << "\n";
  for (const auto& n : cell.radio.utra_neighbors)
    std::cout << "    └─ UTRA UARFCN=" << n.uarfcn << " q_min=" << static_cast<int>(n.q_rx_lev_min)
              << "\n";
  for (const auto& n : cell.radio.geran_neighbors)
    std::cout << "    └─ GERAN ARFCN=" << n.arfcn_start
              << " thresh_hi=" << static_cast<int>(n.thresh_x_high) << "\n";
  for (const auto& n : cell.radio.meas_neighbors) {
    std::cout << "    └─ MeasReport PCI=" << n.pci;
    if (n.has_rsrp) std::cout << " RSRP=" << n.rsrp_dbm << " dBm";
    if (n.has_rsrq) std::cout << " RSRQ=" << n.rsrq_db << " dB";
    std::cout << "\n";
  }
}

void feed(QCom::QualcomParser& p, const char* name, std::span<const uint8_t> data) {
  auto r = p.on_diag_frame(data);
  std::cout << "  " << (r ? "OK" : "SKIP") << "  " << name << "\n";
}

}  // namespace

int main() {
  std::cout << "=== QCom Scanner — Full Multi-RAT Test ===\n\n";

  QCom::QualcomParser parser;

  // Feed in correct order: identity -> signal -> neighbors per RAT

  std::cout << "[LTE]\n";
  feed(parser, "0xB0C2 identity (250-01 TAC=1234 CID=27440068 B7)", lte_identity);
  feed(parser, "0xB17F ML1 serving (RSRP=-80 RSRQ=-20)", lte_signal);

  std::cout << "[NR]\n";
  feed(parser, "0xB992 ML1 serving (SS-RSRP=-100 SS-RSRQ=-15 SINR=10)", nr_signal);

  std::cout << "[GSM]\n";
  feed(parser, "0x5134 cell info (ARFCN=100 BSIC=43 250-01 LAC=5000)", gsm_identity);
  feed(parser, "0x506C burst metrics (RxLev=-80)", gsm_signal);
  feed(parser, "0x5071 surround DB (3 neighbors)", gsm_neighbors);

  std::cout << "[WCDMA]\n";
  feed(parser, "0x4027 cell ID (250-99 LAC=2000 PSC=72 DL=10562)", wcdma_identity);
  feed(parser, "0x4005 resel rank (serving + 1 neighbor)", wcdma_signal);

  std::cout << "\n═══════════════════════════════════════════════════\n";
  std::cout << "[Result] " << parser.tracker().cell_count() << " cell(s):\n\n";
  for (const auto& cell : parser.tracker().get_snapshot()) print_cell(cell);

  return 0;
}
