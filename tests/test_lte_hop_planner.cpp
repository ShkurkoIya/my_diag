#include <catch2/catch_test_macros.hpp>
#include <set>
#include <vector>

#include <observer/model/CellIdentity.h>
#include <observer/lte/LteHopPlanner.h>

using namespace QCom;
using QCom::Lte::cell_is_full_lte;
using QCom::Lte::HopTarget;
using QCom::Lte::pick_full_walk_targets;
using QCom::Lte::pick_hop_targets;
using QCom::Lte::pick_neigh_targets;
using QCom::Lte::pick_seed_targets;
using QCom::Lte::hop_rsrp_measured;
using QCom::Lte::measured_intra_hop_keys;
using QCom::Lte::pending_neigh_hop_keys;
using QCom::Lte::serving_neigh_hop_keys;
using QCom::Lte::sib5_bare_earfcns;

namespace {

struct LteSpec {
  uint32_t earfcn{0};
  uint16_t pci{0};
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint64_t cid{0};
  uint16_t tac{0};
  float rsrp{-160.0f};
  bool ever_serving{false};
};

CellIdentity make_lte(const LteSpec& s) {
  CellIdentity c;
  c.rat = RatType::LTE;
  LteRadioParams r;
  r.earfcn = s.earfcn;
  r.pci = s.pci;
  c.radio.radio_data = r;
  c.passport.mcc = s.mcc;
  c.passport.mnc = s.mnc;
  c.passport.cell_id = s.cid;
  c.passport.tac = s.tac;
  if (s.rsrp > -159.0f) {
    LteSignalParams sig;
    sig.rsrp = s.rsrp;
    c.signal.signal_data = sig;
  }
  c.ever_serving = s.ever_serving;
  return c;
}

// A FULL LTE cell: valid ECI + TAC + PLMN + EARFCN|PCI.
CellIdentity make_full(uint32_t earfcn, uint16_t pci, uint16_t mcc, uint16_t mnc, uint64_t cid,
                       uint16_t tac, float rsrp, bool ever_serving) {
  return make_lte({.earfcn = earfcn,
                   .pci = pci,
                   .mcc = mcc,
                   .mnc = mnc,
                   .cid = cid,
                   .tac = tac,
                   .rsrp = rsrp,
                   .ever_serving = ever_serving});
}

}  // namespace

TEST_CASE("cell_is_full_lte recognises complete identity", "[lte][hop]") {
  CHECK(cell_is_full_lte(make_full(375, 156, 250, 1, 51830553, 17806, -80.0f, false)));
  // Missing CID → not FULL.
  CHECK_FALSE(cell_is_full_lte(make_lte({.earfcn = 375, .pci = 156, .mcc = 250, .mnc = 1})));
  // Missing PCI → not FULL.
  CHECK_FALSE(cell_is_full_lte(
      make_lte({.earfcn = 375, .pci = 0, .mcc = 250, .mnc = 1, .cid = 51830553, .tac = 17806})));
}

TEST_CASE("pick_hop_targets skips FULL cells and ranks by measured RSRP", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  cells.push_back(make_lte({.earfcn = 375, .pci = 200, .rsrp = -100.0f}));  // incomplete, weaker
  cells.push_back(make_lte({.earfcn = 375, .pci = 210, .rsrp = -80.0f}));   // incomplete, stronger
  cells.push_back(make_full(375, 156, 250, 1, 51830553, 17806, -70.0f, false));  // FULL → skip

  auto t = pick_hop_targets(cells, 10);
  REQUIRE(t.size() == 2);
  CHECK(t[0].pci == 210);  // stronger RSRP first
  CHECK(t[1].pci == 200);
  for (const auto& x : t) CHECK(x.pci != 156);  // FULL never a hop target
}

TEST_CASE("pick_hop_targets: measured RSRP beats SIB5 ghost seed", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  // A FULL serving cell that advertises an inter-freq neighbor (ghost seed −155).
  auto serving = make_full(375, 156, 250, 1, 51830553, 17806, -75.0f, true);
  InterFreqCarrier cf;
  cf.earfcn = 1850;
  cf.neigh_pcis = {42};
  serving.radio.inter_freq_carriers.push_back(cf);
  cells.push_back(serving);
  // A measured incomplete cell on another EARFCN.
  cells.push_back(make_lte({.earfcn = 6275, .pci = 88, .rsrp = -95.0f}));

  auto t = pick_hop_targets(cells, 10);
  REQUIRE(t.size() == 2);
  CHECK(t[0].earfcn == 6275);  // measured before ghost
  CHECK(t[0].pci == 88);
  CHECK(t[1].earfcn == 1850);  // ghost seed last
  CHECK(t[1].pci == 42);
}

TEST_CASE("pick_full_walk_targets prefers never-camped operator", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  // Camped FULL on 250-01 (excluded from targets, but seeds camped_per_plmn).
  cells.push_back(make_full(375, 156, 250, 1, 51830553, 17806, -70.0f, /*ever=*/true));
  // Incomplete candidate on already-camped PLMN 250-01.
  cells.push_back(make_lte({.earfcn = 375, .pci = 300, .mcc = 250, .mnc = 1, .rsrp = -85.0f}));
  // Incomplete candidate on an unseen PLMN (mcc=0 → plmn_camp_n 0).
  cells.push_back(make_lte({.earfcn = 6275, .pci = 88, .rsrp = -90.0f}));

  auto t = pick_full_walk_targets(cells, 10);
  REQUIRE(t.size() == 2);  // camped cell excluded
  CHECK(t[0].pci == 88);   // new operator (0 camps) first, despite weaker RSRP
  CHECK(t[1].pci == 300);
}

TEST_CASE("pick_full_walk_targets excludes already-camped keys", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  cells.push_back(make_lte({.earfcn = 375, .pci = 156, .rsrp = -70.0f, .ever_serving = true}));
  cells.push_back(make_lte({.earfcn = 375, .pci = 200, .rsrp = -90.0f}));

  auto t = pick_full_walk_targets(cells, 10);
  REQUIRE(t.size() == 1);
  CHECK(t[0].pci == 200);
}

TEST_CASE("pick_*_targets honour max_n", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  for (uint16_t pci = 1; pci <= 8; ++pci)
    cells.push_back(make_lte({.earfcn = 375, .pci = pci, .rsrp = -80.0f - pci}));

  CHECK(pick_hop_targets(cells, 3).size() == 3);
  CHECK(pick_full_walk_targets(cells, 5).size() == 5);
}

TEST_CASE("pick_hop_targets: ML1 neighbor PCIs are not seeded (classic hop uses SIB5 only)",
          "[lte][hop]") {
  // Classic hop only seeds from own row + inter-freq carriers, not meas_neighbors.
  std::vector<CellIdentity> cells;
  auto c = make_lte({.earfcn = 375, .pci = 156, .rsrp = -75.0f});
  NeighborMeasResult n;
  n.pci = 44;
  n.has_rsrp = true;
  n.rsrp_dbm = -88.0f;
  c.radio.meas_neighbors.push_back(n);
  cells.push_back(c);

  auto hop = pick_hop_targets(cells, 10);
  REQUIRE(hop.size() == 1);
  CHECK(hop[0].pci == 156);

  // Full walk DOES fold meas neighbors into candidates.
  auto walk = pick_full_walk_targets(cells, 10);
  bool saw_44 = false;
  for (const auto& t : walk)
    if (t.pci == 44) saw_44 = true;
  CHECK(saw_44);
}

TEST_CASE("pick_hop_targets drops EARFCN with no commercial band", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  cells.push_back(make_lte({.earfcn = 32768, .pci = 12, .rsrp = -70.0f}));  // 0x8000 DIAG ghost
  cells.push_back(make_lte({.earfcn = 15363, .pci = 6, .rsrp = -72.0f}));
  cells.push_back(make_lte({.earfcn = 3200, .pci = 130, .rsrp = -90.0f}));  // B7

  auto hop = pick_hop_targets(cells, 10);
  REQUIRE(hop.size() == 1);
  CHECK(hop[0].earfcn == 3200);
  CHECK(hop[0].pci == 130);

  auto walk = pick_full_walk_targets(cells, 10);
  REQUIRE(walk.size() == 1);
  CHECK(walk[0].earfcn == 3200);
}

TEST_CASE("pick_hop_targets ranks FDD before stronger TDD", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  cells.push_back(make_lte({.earfcn = 38750, .pci = 23, .rsrp = -65.0f}));  // B40 TDD
  cells.push_back(make_lte({.earfcn = 3200, .pci = 130, .rsrp = -95.0f}));  // B7 FDD
  cells.push_back(make_lte({.earfcn = 6200, .pci = 3, .rsrp = -100.0f}));   // B20 FDD

  auto t = pick_hop_targets(cells, 10);
  REQUIRE(t.size() == 3);
  CHECK(t[0].earfcn == 3200);
  CHECK(t[1].earfcn == 6200);
  CHECK(t[2].earfcn == 38750);
}

TEST_CASE("pick_hop_targets prefers a new carrier over extra PCI on a FULL EARFCN", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  cells.push_back(make_full(3200, 130, 250, 1, 51830530, 17806, -70.0f, true));
  cells.push_back(make_lte({.earfcn = 3200, .pci = 92, .rsrp = -75.0f}));  // same B7, stronger
  cells.push_back(make_lte({.earfcn = 6200, .pci = 3, .rsrp = -98.0f}));   // new B20

  auto t = pick_hop_targets(cells, 10);
  REQUIRE(t.size() == 2);
  CHECK(t[0].earfcn == 6200);
  CHECK(t[1].earfcn == 3200);
  CHECK(t[1].pci == 92);
}

TEST_CASE("full-walk: extra SIB1 PLMN on a FULL EARFCN loses to a new operator", "[lte][hop]") {
  // Live 21:07: camped MegaFon 1596/157 250-02, then chased 250-21 ghosts on 1596
  // instead of MTS/t2 on other carriers.
  std::vector<CellIdentity> cells;
  cells.push_back(make_full(1596, 157, 250, 2, 199815113, 7828, -80.0f, /*ever=*/true));
  cells.push_back(make_lte({.earfcn = 1596, .pci = 10, .mcc = 250, .mnc = 21, .rsrp = -70.0f}));
  cells.push_back(make_lte({.earfcn = 6200, .pci = 3, .mcc = 250, .mnc = 20, .rsrp = -95.0f}));
  cells.push_back(make_full(1721, 123, 250, 1, 51830533, 17806, -90.0f, /*ever=*/false));

  auto t = pick_full_walk_targets(cells, 10);
  REQUIRE(t.size() >= 3);
  CHECK(t[0].earfcn == 6200);  // new carrier, new PLMN
  CHECK(t[0].pci == 3);
  // Uncamped MTS FULL on a different EARFCN before MegaFon extra-PLMN ghosts.
  CHECK(t[1].earfcn == 1721);
  CHECK(t[1].pci == 123);
  CHECK(t.back().earfcn == 1596);
  CHECK(t.back().pci == 10);
}

TEST_CASE("full-walk: PCI with no PLMN on a camped EARFCN inherit the grid camp count",
          "[lte][hop]") {
  std::vector<CellIdentity> cells;
  cells.push_back(make_full(1596, 157, 250, 2, 199815113, 7828, -80.0f, /*ever=*/true));
  cells.push_back(make_lte({.earfcn = 1596, .pci = 256, .rsrp = -72.0f}));  // ghost, mcc=0
  cells.push_back(make_lte({.earfcn = 100, .pci = 174, .mcc = 250, .mnc = 20, .rsrp = -88.0f}));

  auto t = pick_full_walk_targets(cells, 10);
  REQUIRE(t.size() == 2);
  CHECK(t[0].earfcn == 100);
  CHECK(t[0].pci == 174);
  CHECK(t[1].earfcn == 1596);
  CHECK(t[1].pci == 256);
}

TEST_CASE("pick_seed_targets hops measured siblings, skips SSS on a FULL EARFCN", "[lte][hop]") {
  CHECK(hop_rsrp_measured(-90.0f));
  CHECK_FALSE(hop_rsrp_measured(-155.0f));
  CHECK_FALSE(hop_rsrp_measured(-160.0f));

  std::vector<CellIdentity> cells;
  cells.push_back(make_full(525, 182, 250, 99, 199992934, 678, -83.0f, true));
  cells.push_back(make_lte({.earfcn = 525, .pci = 2}));                      // SSS, no RSRP
  cells.push_back(make_lte({.earfcn = 525, .pci = 3, .rsrp = -91.0f}));      // QMI/ML1 intra
  cells.push_back(make_lte({.earfcn = 6200, .pci = 3, .mcc = 250, .mnc = 20, .rsrp = -95.0f}));

  auto t = pick_seed_targets(cells, 10, /*full_walk=*/true);
  REQUIRE(t.size() == 2);
  CHECK(t[0].earfcn == 6200);  // new carrier first
  CHECK(t[0].pci == 3);
  CHECK(t[1].earfcn == 525);
  CHECK(t[1].pci == 3);
  for (const auto& x : t) CHECK(x.pci != 2);
}

TEST_CASE("pick_seed_targets hops COPS=? RF without measured RSRP", "[lte][hop]") {
  // Live 182315: 23 RF, empty rxl, 3 FULL from B0C0 — measured-only seed was empty.
  std::vector<CellIdentity> cells;
  cells.push_back(make_full(525, 182, 250, 99, 199992934, 678, -160.0f, /*ever=*/false));
  cells.push_back(make_lte({.earfcn = 525, .pci = 2}));    // SSS sibling on FULL EARFCN
  cells.push_back(make_lte({.earfcn = 6350, .pci = 82}));  // 0-FULL, no RSRP
  cells.push_back(make_lte({.earfcn = 6350, .pci = 83}));
  cells.push_back(make_full(38100, 455, 250, 1, 256012345, 1234, -160.0f, /*ever=*/false));

  auto t = pick_seed_targets(cells, 10, /*full_walk=*/true);
  REQUIRE(t.size() == 4);
  CHECK(t[0].earfcn == 6350);  // new FDD carrier (COPS=? RF)
  CHECK(t[2].earfcn == 525);   // uncamped FULL after empty carriers
  CHECK(t[3].earfcn == 38100); // TDD last
  CHECK(t[3].pci == 455);
  for (const auto& x : t) CHECK(x.pci != 2);
}

TEST_CASE("pick_seed_targets new-carrier RF beats QMI FULL on another EARFCN", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  cells.push_back(make_full(375, 438, 250, 1, 51830999, 17806, -80.0f, /*ever=*/false));
  cells.push_back(make_lte({.earfcn = 1721, .pci = 123, .rsrp = -85.0f}));
  auto t = pick_seed_targets(cells, 10, /*full_walk=*/true);
  REQUIRE(t.size() == 2);
  CHECK(t[0].earfcn == 1721);
  CHECK(t[0].pci == 123);
  CHECK(t[1].earfcn == 375);
}

TEST_CASE("serving_neigh_hop_keys is the inspector list (intra meas + SIB5 PCI)", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  auto srv = make_full(1721, 123, 250, 1, 51830533, 17806, -76.0f, /*ever=*/true);
  NeighborMeasResult n;
  n.pci = 128;
  n.has_rsrp = true;
  n.rsrp_dbm = -84.2f;
  srv.radio.meas_neighbors.push_back(n);
  InterFreqCarrier cf;
  cf.earfcn = 3200;
  cf.neigh_pcis = {130};
  srv.radio.inter_freq_carriers.push_back(cf);
  InterFreqCarrier bare;
  bare.earfcn = 6275;  // SIB5 freq-only
  srv.radio.inter_freq_carriers.push_back(bare);
  cells.push_back(srv);
  cells.push_back(make_lte({.earfcn = 1721, .pci = 2}));  // SSS, not in serving lists

  auto keys = serving_neigh_hop_keys(cells);
  CHECK(keys.contains({1721, 128}));
  CHECK(keys.contains({3200, 130}));
  CHECK_FALSE(keys.contains({1721, 123}));  // serving excluded
  CHECK_FALSE(keys.contains({1721, 2}));    // SSS mill excluded

  auto bare_e = sib5_bare_earfcns(cells);
  REQUIRE(bare_e.size() == 1);
  CHECK(bare_e[0] == 6275);
}

TEST_CASE("pending_neigh_hop_keys is serving-neigh plus measured intra, not SSS", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  auto srv = make_full(1721, 123, 250, 1, 51830533, 17806, -76.0f, /*ever=*/true);
  NeighborMeasResult n;
  n.pci = 128;
  n.has_rsrp = true;
  n.rsrp_dbm = -84.2f;
  srv.radio.meas_neighbors.push_back(n);
  cells.push_back(srv);
  cells.push_back(make_lte({.earfcn = 1721, .pci = 2}));  // SSS
  cells.push_back(make_lte({.earfcn = 1721, .pci = 130, .rsrp = -90.0f}));  // QMI/ML1

  auto meas = measured_intra_hop_keys(cells);
  CHECK(meas.contains({1721, 130}));
  CHECK_FALSE(meas.contains({1721, 2}));
  CHECK_FALSE(meas.contains({1721, 123}));

  auto pending = pending_neigh_hop_keys(cells);
  CHECK(pending.contains({1721, 128}));
  CHECK(pending.contains({1721, 130}));
  CHECK_FALSE(pending.contains({1721, 123}));
  CHECK_FALSE(pending.contains({1721, 2}));
}

TEST_CASE("sib5_bare_earfcns skips carriers that already have RF", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  auto srv = make_full(1721, 123, 250, 1, 51830533, 17806, -76.0f, /*ever=*/true);
  InterFreqCarrier cf;
  cf.earfcn = 375;
  srv.radio.inter_freq_carriers.push_back(cf);
  cells.push_back(srv);
  cells.push_back(make_lte({.earfcn = 375, .pci = 156}));

  CHECK(sib5_bare_earfcns(cells).empty());
}

TEST_CASE("pick_neigh_targets is a CMGRMI whitelist, not the whole snapshot", "[lte][hop]") {
  std::vector<CellIdentity> cells;
  cells.push_back(make_full(525, 182, 250, 99, 199992934, 678, -83.0f, true));
  cells.push_back(make_lte({.earfcn = 525, .pci = 2, .rsrp = -88.0f}));  // SSS, not in allow
  cells.push_back(make_lte({.earfcn = 525, .pci = 130, .rsrp = -92.0f}));
  cells.push_back(make_lte({.earfcn = 1596, .pci = 447, .rsrp = -89.0f}));

  std::set<QCom::Lte::HopKey> allow{{525, 130}, {1596, 447}, {525, 182}};
  auto t = pick_neigh_targets(cells, allow, 10, /*full_walk=*/true);
  REQUIRE(t.size() == 2);  // 182 camped → skip; 2 not in allow
  CHECK(t[0].earfcn == 1596);
  CHECK(t[0].pci == 447);
  CHECK(t[1].earfcn == 525);
  CHECK(t[1].pci == 130);
}
