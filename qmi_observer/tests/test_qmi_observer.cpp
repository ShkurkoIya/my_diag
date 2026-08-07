#include <qmi_observer/bridge.hpp>
#include <qmi_observer/health.hpp>
#include <qmi_observer/types.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace qmi_observer;

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
  REQUIRE(envs.size() == 3);
  REQUIRE(envs[0].key.freq == 10563);
  REQUIRE(envs[0].key.pci_bsic == 38);
  REQUIRE(envs[0].rat == QCom::RatType::WCDMA);
  REQUIRE(std::holds_alternative<QCom::Events::PassportEvent>(envs[0].event_data));
  REQUIRE(std::holds_alternative<QCom::Events::SignalUpdateEvent>(envs[1].event_data));
  REQUIRE(std::holds_alternative<QCom::Events::ServingChangedEvent>(envs[2].event_data));
}
