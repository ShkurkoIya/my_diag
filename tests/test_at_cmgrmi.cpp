#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <qcom/at/Cmgrmi.h>

using Catch::Approx;

namespace {

constexpr const char* kSample = R"(
+CMGRMI: Main_Info,4,1,1,0,2,6883,0,4,0
+CMGRMI: Serving_Cell,375,250,01,17806,2,51830553,1,4,4,17,156,-112,-914,-599,0
+CMGRMI: CA_Scell,38100,455,38,5,0,455,-120,-991,-781,0,0,0
+CMGRMI: LTE_Intra,1,375,156,1
+CMGRMI: LTE_Intra_Cell1,156,-112,-914,-599,0
+CMGRMI: LTE_Inter,3,Freq1,38100,2,0,0,0,Freq2,3200,2,0,0,0
+CMGRMI: LTE_InterFreq1_Cell1,455,-120,-991,-781,0
+CMGRMI: LTE_InterFreq1_Cell2,391,-149,-1060,-781,0
+CMGRMI: LTE_InterFreq2_Cell1,130,-100,-946,-756,0
+CMGRMI: LTE_InterFreq2_Cell2,92,-186,-1032,-756,0
OK
)";

}  // namespace

TEST_CASE("CMGRMI=4 parse serving + neighbors", "[at][cmgrmi]") {
  auto snap = QCom::AtCmgrmi::parse_lte(kSample);
  REQUIRE(snap.serving.ok);
  CHECK(snap.serving.earfcn == 375);
  CHECK(snap.serving.pci == 156);
  CHECK(snap.serving.mcc == 250);
  CHECK(snap.serving.mnc == 1);
  CHECK(snap.serving.tac == 17806);  // 0x458E
  CHECK(snap.serving.timing_advance == 2);  // matches QXDM 0xB114 Starting UL TA
  CHECK(snap.serving.cell_id == 51830553);
  CHECK(snap.serving.dl_bw_mhz == 15);
  CHECK(snap.serving.rsrp == Approx(-91.4f).margin(0.05f));
  CHECK(snap.serving.rsrq == Approx(-11.2f).margin(0.05f));

  // Intra serving PCI may appear as Intra_Cell; CA + inter cells populate neighbors.
  REQUIRE(snap.neighbors.size() >= 4);
  bool saw_ca = false;
  bool saw_inter = false;
  for (const auto& n : snap.neighbors) {
    if (n.earfcn == 38100 && n.pci == 455) {
      saw_ca = true;
      CHECK(n.has_rsrp);
      CHECK(n.rsrp == Approx(-99.1f).margin(0.05f));
    }
    if (n.earfcn == 3200 && n.pci == 130) saw_inter = true;
  }
  CHECK(saw_ca);
  CHECK(saw_inter);
  REQUIRE(snap.inter_carriers.size() >= 2);
  bool ca_on_parent = false;
  for (const auto& c : snap.inter_carriers) {
    if (c.earfcn != 38100) continue;
    ca_on_parent = std::find(c.neigh_pcis.begin(), c.neigh_pcis.end(), 455) != c.neigh_pcis.end();
  }
  CHECK(ca_on_parent);

  auto envs = QCom::AtCmgrmi::to_envelopes(snap);
  REQUIRE(envs.size() >= 4);
  bool has_pass = false;
  bool has_inter = false;
  bool has_ta = false;
  for (const auto& e : envs) {
    if (std::holds_alternative<QCom::Events::PassportEvent>(e.event_data)) has_pass = true;
    if (std::holds_alternative<QCom::Events::InterFreqCarriersEvent>(e.event_data)) has_inter = true;
    if (auto* gen = std::get_if<QCom::Events::GenericRadioParamsEvent>(&e.event_data)) {
      if (auto* lte = std::get_if<QCom::Events::RadioParamsEvent<QCom::LteRadioParams>>(gen)) {
        if (lte->data.timing_advance == 2) has_ta = true;
      }
    }
  }
  CHECK(has_pass);
  CHECK(has_inter);
  CHECK(has_ta);
}

TEST_CASE("CMGRMI=4 Serving_Cell TA=1 (live B7 lock)", "[at][cmgrmi]") {
  // Live SIM8300 after CLECELL 3200/130 — TA dropped from 2 → 1 (same eNB, closer sector).
  constexpr const char* kTa1 = R"(
+CMGRMI: Serving_Cell,3200,250,01,17806,1,51830530,7,3,3,17,130,-85,-868,-613,37
OK
)";
  auto snap = QCom::AtCmgrmi::parse_lte(kTa1);
  REQUIRE(snap.serving.ok);
  CHECK(snap.serving.earfcn == 3200);
  CHECK(snap.serving.pci == 130);
  CHECK(snap.serving.timing_advance == 1);
  CHECK(snap.serving.cell_id == 51830530);
}

TEST_CASE("CMGRMI=4 empty / error", "[at][cmgrmi]") {
  auto snap = QCom::AtCmgrmi::parse_lte("ERROR\r\n");
  CHECK_FALSE(snap.serving.ok);
  CHECK(QCom::AtCmgrmi::to_envelopes(snap).empty());
}

TEST_CASE("CMGRMI=4 rejects transitional sentinels", "[at][cmgrmi]") {
  // Modem NO_SERVICE / mid-camp junk — must not mint GUI 65535 / EARFCN=0xFFFFFFFF blobs.
  constexpr const char* kJunk = R"(
+CMGRMI: Serving_Cell,4294967295,65535,65535,65535,2,51830553,1,4,4,17,156,-112,-914,-599,0
+CMGRMI: LTE_Inter,1,Freq1,4294967295,1,0,0,0
+CMGRMI: LTE_InterFreq1_Cell1,100,-100,-900,-700,0
OK
)";
  auto snap = QCom::AtCmgrmi::parse_lte(kJunk);
  CHECK_FALSE(snap.serving.ok);
  CHECK(snap.neighbors.empty());
  CHECK(snap.inter_carriers.empty());
  CHECK(QCom::AtCmgrmi::to_envelopes(snap).empty());
}

TEST_CASE("CMGRMI=4 neighbors without serving still mint RF", "[at][cmgrmi]") {
  constexpr const char* kNbOnly = R"(
+CMGRMI: Serving_Cell,4294967295,65535,65535,0,2,0,1,4,4,17,0,-112,-914,-599,0
+CMGRMI: LTE_Inter,1,Freq1,1275,1,0,0,0
+CMGRMI: LTE_InterFreq1_Cell1,174,-100,-900,-700,0
OK
)";
  auto snap = QCom::AtCmgrmi::parse_lte(kNbOnly);
  CHECK_FALSE(snap.serving.ok);
  REQUIRE(snap.neighbors.size() == 1);
  CHECK(snap.neighbors[0].earfcn == 1275);
  CHECK(snap.neighbors[0].pci == 174);
  auto envs = QCom::AtCmgrmi::to_envelopes(snap);
  REQUIRE_FALSE(envs.empty());
  bool has_rf = false;
  for (const auto& e : envs) {
    if (e.key.freq == 1275 && e.key.pci_bsic == 174) has_rf = true;
  }
  CHECK(has_rf);
}

TEST_CASE("CMGRMI=4 CA_Scell PCI lands on parent neigh_pcis", "[at][cmgrmi]") {
  constexpr const char* kCa = R"(
+CMGRMI: Serving_Cell,375,250,01,17806,2,51830553,1,4,4,17,156,-112,-914,-599,0
+CMGRMI: CA_Scell,38100,455,38,5,0,455,-120,-991,-781,0,0,0
OK
)";
  auto snap = QCom::AtCmgrmi::parse_lte(kCa);
  REQUIRE(snap.serving.ok);
  REQUIRE(snap.neighbors.size() == 1);
  CHECK(snap.neighbors[0].ca);
  REQUIRE(snap.inter_carriers.size() == 1);
  CHECK(snap.inter_carriers[0].earfcn == 38100);
  REQUIRE(snap.inter_carriers[0].neigh_pcis.size() == 1);
  CHECK(snap.inter_carriers[0].neigh_pcis[0] == 455);

  auto envs = QCom::AtCmgrmi::to_envelopes(snap);
  bool has_inter = false;
  bool has_ca_radio = false;
  for (const auto& e : envs) {
    if (std::holds_alternative<QCom::Events::InterFreqCarriersEvent>(e.event_data)) has_inter = true;
    if (e.key.freq == 38100 && e.key.pci_bsic == 455) has_ca_radio = true;
  }
  CHECK(has_inter);
  CHECK(has_ca_radio);
}

TEST_CASE("CMGRMI neighbor_hop_keys whitelist excludes serving", "[at][cmgrmi]") {
  auto snap = QCom::AtCmgrmi::parse_lte(kSample);
  auto keys = QCom::AtCmgrmi::neighbor_hop_keys(snap);
  CHECK_FALSE(keys.contains({375, 156}));
  CHECK(keys.contains({38100, 455}));
  CHECK(keys.contains({3200, 130}));
  CHECK(keys.contains({38100, 391}));
}
