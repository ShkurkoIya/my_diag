#include <catch2/catch_test_macros.hpp>

#include "core/CellIdentity.h"
#include "core/CellTracker.h"
#include "core/Events.h"

using namespace QCom;

TEST_CASE("CellTracker creates cell on first event", "[tracker]") {
  CellTracker tracker;

  Events::RrcEventEnvelope env{
      .key = {.freq = 2660, .pci_bsic = 72},
      .rat = RatType::LTE,
      .timestamp = 1000,
      .event_data = Events::ServingChangedEvent{.is_serving = true},
  };

  tracker.handle_rrc_event(std::move(env));

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].rat == RatType::LTE);
  CHECK(snap[0].is_serving == true);
  CHECK(snap[0].radio.freq() == 2660);
  CHECK(snap[0].radio.pci_bsic() == 72);
}

TEST_CASE("CellTracker merges passport into existing cell", "[tracker]") {
  CellTracker tracker;

  LocalCellKey key{.freq = 2660, .pci_bsic = 72};

  // First: ML1 signal arrives
  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = key,
      .rat = RatType::LTE,
      .timestamp = 1000,
      .event_data = Events::ServingChangedEvent{.is_serving = true},
  });

  // Second: SIB1 passport arrives on same key
  CellPassport passport{.tac = 0xAB, .cell_id = 0x01234567, .mcc = 250, .mnc = 1};
  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = key,
      .rat = RatType::LTE,
      .timestamp = 2000,
      .event_data = Events::PassportEvent{.passport = passport},
  });

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].passport.cell_id == 0x01234567);
  CHECK(snap[0].passport.mcc == 250);
  CHECK(snap[0].is_serving == true);
}

TEST_CASE("CellTracker: serving change clears old serving", "[tracker]") {
  CellTracker tracker;

  LocalCellKey key1{.freq = 2660, .pci_bsic = 72};
  LocalCellKey key2{.freq = 2660, .pci_bsic = 100};

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = key1, .rat = RatType::LTE, .event_data = Events::ServingChangedEvent{true}});
  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = key2, .rat = RatType::LTE, .event_data = Events::ServingChangedEvent{true}});

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 2);

  int serving_count = 0;
  for (const auto& c : snap) {
    if (c.is_serving) ++serving_count;
  }
  CHECK(serving_count == 1);
}

TEST_CASE("CellTracker: signal update fills variant correctly", "[tracker]") {
  CellTracker tracker;

  LocalCellKey key{.freq = 2660, .pci_bsic = 72};

  CellSignal sig;
  sig.signal_data = LteSignalParams{.rsrp = -95.0f, .rsrq = -12.0f, .sinr = 15.0f};

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = key,
      .rat = RatType::LTE,
      .event_data = Events::SignalUpdateEvent{.signal = sig},
  });

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 1);

  auto* lte = snap[0].signal.get_if<LteSignalParams>();
  REQUIRE(lte);
  CHECK(lte->rsrp == -95.0f);
  CHECK(lte->rsrq == -12.0f);
}

TEST_CASE("CellTracker: intra-freq neighbors stored", "[tracker]") {
  CellTracker tracker;
  LocalCellKey key{.freq = 2660, .pci_bsic = 72};

  Events::IntraNeighborsEvent nev;
  nev.neighbors = {{.pci = 100, .q_offset = -3}, {.pci = 200, .q_offset = 0}};

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = key, .rat = RatType::LTE, .event_data = std::move(nev)});

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].radio.intra_freq_neighbors.size() == 2);
  CHECK(snap[0].radio.intra_freq_neighbors[0].pci == 100);
}

TEST_CASE("CellTracker: inter-freq carriers stored", "[tracker]") {
  CellTracker tracker;
  LocalCellKey key{.freq = 2660, .pci_bsic = 72};

  Events::InterFreqCarriersEvent ev;
  ev.carriers = {{.earfcn = 1300, .thresh_x_high = 10}, {.earfcn = 3500, .thresh_x_high = 8}};

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = key, .rat = RatType::LTE, .event_data = std::move(ev)});

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].radio.inter_freq_carriers.size() == 2);
  CHECK(snap[0].radio.inter_freq_carriers[0].earfcn == 1300);
}
