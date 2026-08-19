#include <qcom/qmi/bridge.hpp>
#include <qcom/qmi/health.hpp>
#include <qcom/qmi/types.hpp>

#include <catch2/catch_test_macros.hpp>

#include <variant>

using namespace QCom::Qmi;

TEST_CASE("evaluate_health: offline needs recover", "[qmi][health]") {
  HealthProbe p;
  p.session_open = true;
  p.device_node_present = true;
  p.operating_mode = OperatingModeKind::Offline;
  const auto h = evaluate_health(p);
  REQUIRE(h.phase == ModemPhase::OfflineRf);
  REQUIRE(h.needs_recover);
  REQUIRE_FALSE(h.can_force_search);
  REQUIRE_FALSE(h.can_snapshot);
}

TEST_CASE("evaluate_health: denied still camped", "[qmi][health]") {
  HealthProbe p;
  p.session_open = true;
  p.device_node_present = true;
  p.operating_mode = OperatingModeKind::Online;
  p.registration = RegistrationKind::Denied;
  p.radio = Rat::Wcdma;
  const auto h = evaluate_health(p);
  REQUIRE(h.phase == ModemPhase::Camped);
  REQUIRE(h.can_snapshot);
  REQUIRE_FALSE(h.needs_recover);
}

TEST_CASE("evaluate_health: searching", "[qmi][health]") {
  HealthProbe p;
  p.session_open = true;
  p.device_node_present = true;
  p.operating_mode = OperatingModeKind::Online;
  p.registration = RegistrationKind::Searching;
  p.last_cell_location_no_network = true;
  const auto h = evaluate_health(p);
  REQUIRE(h.phase == ModemPhase::Searching);
  REQUIRE(h.can_snapshot);
}

TEST_CASE("mode_preference_covers", "[qmi][health]") {
  REQUIRE(mode_preference_covers({Rat::Lte, Rat::Wcdma}, {Rat::Lte, Rat::Wcdma}));
  REQUIRE(mode_preference_covers({Rat::Lte, Rat::Wcdma}, {Rat::Lte}));
  REQUIRE_FALSE(mode_preference_covers({Rat::Wcdma}, {Rat::Lte, Rat::Wcdma}));
}

TEST_CASE("merge_snapshot dedups by key", "[qmi][types]") {
  AggregatedCells agg;
  CellSnapshot a;
  a.cells.push_back(CellObservation{
      .rat = Rat::Wcdma,
      .cell_id = 4487261ull,
      .rf_channel = 10563u,
      .phy_id = 38,
      .serving = true,
  });
  CellSnapshot b = a;
  b.cells[0].rsrp_dbm = -59.f;
  merge_snapshot(agg, a);
  merge_snapshot(agg, b);
  REQUIRE(agg.cells.size() == 1);
}

TEST_CASE("to_rrc_envelopes emits passport+signal+serving", "[qmi][bridge]") {
  CellSnapshot snap;
  snap.cells.push_back(CellObservation{
      .rat = Rat::Wcdma,
      .plmn = Plmn{.mcc = 250, .mnc = 20, .mnc_digits = 2},
      .lac_or_tac = 19719u,
      .cell_id = 4487261ull,
      .rf_channel = 10563u,
      .phy_id = 38,
      .rsrp_dbm = -59.f,
      .rsrq_db = -2.f,
      .serving = true,
  });

  const auto envs = to_rrc_envelopes(snap, 42);
  REQUIRE(envs.size() == 4);
  REQUIRE(envs[0].key.freq == 10563);
  REQUIRE(envs[0].key.pci_bsic == 38);
  REQUIRE(envs[0].rat == QCom::RatType::WCDMA);
  REQUIRE(std::holds_alternative<QCom::Events::GenericRadioParamsEvent>(envs[0].event_data));
  REQUIRE(std::holds_alternative<QCom::Events::PassportEvent>(envs[1].event_data));
  REQUIRE(std::holds_alternative<QCom::Events::SignalUpdateEvent>(envs[2].event_data));
  REQUIRE(std::holds_alternative<QCom::Events::ServingChangedEvent>(envs[3].event_data));
}

TEST_CASE("to_rrc_envelopes LTE neighbors attach to serving", "[qmi][bridge]") {
  CellSnapshot snap;
  snap.cells.push_back(CellObservation{
      .rat = Rat::Lte,
      .plmn = Plmn{.mcc = 250, .mnc = 1, .mnc_digits = 2},
      .lac_or_tac = 17806u,
      .cell_id = 51830533ull,
      .rf_channel = 1721u,
      .phy_id = 123,
      .rsrp_dbm = -76.f,
      .serving = true,
  });
  snap.cells.push_back(CellObservation{
      .rat = Rat::Lte,
      .rf_channel = 1721u,
      .phy_id = 128,
      .rsrp_dbm = -84.f,
      .serving = false,
  });
  snap.cells.push_back(CellObservation{
      .rat = Rat::Lte,
      .rf_channel = 3200u,
      .phy_id = 130,
      .rsrp_dbm = -92.f,
      .serving = false,
  });

  const auto envs = to_rrc_envelopes(snap, 1);
  bool saw_meas = false;
  bool saw_inter = false;
  for (const auto& e : envs) {
    if (auto* m = std::get_if<QCom::Events::NeighborMeasEvent>(&e.event_data)) {
      saw_meas = true;
      REQUIRE(e.key.freq == 1721);
      REQUIRE(e.key.pci_bsic == 123);
      REQUIRE(m->neighbors.size() == 1);
      CHECK(m->neighbors[0].pci == 128);
    }
    if (auto* c = std::get_if<QCom::Events::InterFreqCarriersEvent>(&e.event_data)) {
      saw_inter = true;
      REQUIRE(c->carriers.size() == 1);
      CHECK(c->carriers[0].earfcn == 3200);
      REQUIRE(c->carriers[0].neigh_pcis.size() == 1);
      CHECK(c->carriers[0].neigh_pcis[0] == 130);
    }
  }
  CHECK(saw_meas);
  CHECK(saw_inter);
}
