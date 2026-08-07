#include <catch2/catch_test_macros.hpp>

#include <algorithm>

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

  tracker.handle_rrc_event(
      Events::RrcEventEnvelope{.key = key1,
                               .rat = RatType::LTE,
                               .event_data = Events::ServingChangedEvent{.is_serving = true}});
  tracker.handle_rrc_event(
      Events::RrcEventEnvelope{.key = key2,
                               .rat = RatType::LTE,
                               .event_data = Events::ServingChangedEvent{.is_serving = true}});

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

  // Seed serving row first so SIB4 upsert has an EARFCN|PCI home.
  Events::RadioParamsEvent<LteRadioParams> radio;
  radio.data.earfcn = 2660;
  radio.data.pci = 72;
  tracker.handle_rrc_event(
      Events::RrcEventEnvelope{.key = key, .rat = RatType::LTE, .event_data = std::move(radio)});

  Events::IntraNeighborsEvent nev;
  nev.neighbors = {{.pci = 100, .q_offset = -3}, {.pci = 200, .q_offset = 0}};

  tracker.handle_rrc_event(
      Events::RrcEventEnvelope{.key = key, .rat = RatType::LTE, .event_data = std::move(nev)});

  auto snap = tracker.get_snapshot();
  // Serving + two SIB4 PCI rows.
  REQUIRE(snap.size() == 3);
  auto serving = std::find_if(snap.begin(), snap.end(),
                              [](const CellIdentity& c) { return c.radio.pci_bsic() == 72; });
  REQUIRE(serving != snap.end());
  CHECK(serving->radio.intra_freq_neighbors.size() == 2);
  CHECK(serving->radio.intra_freq_neighbors[0].pci == 100);

  CHECK(std::any_of(snap.begin(), snap.end(),
                    [](const CellIdentity& c) { return c.radio.pci_bsic() == 100; }));
  CHECK(std::any_of(snap.begin(), snap.end(),
                    [](const CellIdentity& c) { return c.radio.pci_bsic() == 200; }));
}

TEST_CASE("CellTracker: PLMN fans out to same-EARFCN PCI rows", "[tracker][lte-merge]") {
  CellTracker tracker;
  LocalCellKey a{.freq = 3400, .pci_bsic = 468};
  LocalCellKey b{.freq = 3400, .pci_bsic = 107};

  Events::RadioParamsEvent<LteRadioParams> ra;
  ra.data.earfcn = 3400;
  ra.data.pci = 468;
  tracker.handle_rrc_event(
      Events::RrcEventEnvelope{.key = a, .rat = RatType::LTE, .event_data = std::move(ra)});

  Events::RadioParamsEvent<LteRadioParams> rb;
  rb.data.earfcn = 3400;
  rb.data.pci = 107;
  tracker.handle_rrc_event(
      Events::RrcEventEnvelope{.key = b, .rat = RatType::LTE, .event_data = std::move(rb)});

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = a,
      .rat = RatType::LTE,
      .event_data =
          Events::PassportEvent{.passport = {.tac = 1, .cell_id = 9, .mcc = 250, .mnc = 20}},
  });

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 2);
  for (const auto& c : snap) {
    CHECK(c.passport.mcc == 250);
    CHECK(c.passport.mnc == 20);
  }
  auto with_cid = std::find_if(snap.begin(), snap.end(),
                               [](const CellIdentity& c) { return c.passport.cell_id == 9; });
  REQUIRE(with_cid != snap.end());
  auto neigh = std::find_if(snap.begin(), snap.end(),
                            [](const CellIdentity& c) { return c.radio.pci_bsic() == 107; });
  REQUIRE(neigh != snap.end());
  CHECK(neigh->passport.cell_id == 0);  // CID not copied
}

TEST_CASE("CellTracker: inter-freq carriers stored", "[tracker]") {
  CellTracker tracker;
  LocalCellKey key{.freq = 2660, .pci_bsic = 72};

  Events::InterFreqCarriersEvent ev;
  ev.carriers = {{.earfcn = 1300, .thresh_x_high = 10}, {.earfcn = 3500, .thresh_x_high = 8}};

  tracker.handle_rrc_event(
      Events::RrcEventEnvelope{.key = key, .rat = RatType::LTE, .event_data = std::move(ev)});

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].radio.inter_freq_carriers.size() == 2);
  CHECK(snap[0].radio.inter_freq_carriers[0].earfcn == 1300);
}

TEST_CASE("CellTracker: LTE promote EARFCN|0 PLMN into EARFCN|PCI", "[tracker][lte-merge]") {
  CellTracker tracker;
  LocalCellKey weak{.freq = 1850, .pci_bsic = 0};
  LocalCellKey strong{.freq = 1850, .pci_bsic = 42};

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = weak,
      .rat = RatType::LTE,
      .event_data = Events::PassportEvent{.passport = {.mcc = 250, .mnc = 20}},
  });

  Events::RadioParamsEvent<LteRadioParams> radio;
  radio.data.earfcn = 1850;
  radio.data.pci = 42;
  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = strong,
      .rat = RatType::LTE,
      .event_data = Events::RrcEvent{std::move(radio)},
  });

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].radio.freq() == 1850);
  CHECK(snap[0].radio.pci_bsic() == 42);
  CHECK(snap[0].passport.mcc == 250);
  CHECK(snap[0].passport.mnc == 20);
}

TEST_CASE("CellTracker: LTE SIB1 on PCI merges prior weak PLMN", "[tracker][lte-merge]") {
  CellTracker tracker;

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = {.freq = 1850, .pci_bsic = 0},
      .rat = RatType::LTE,
      .event_data = Events::PassportEvent{.passport = {.mcc = 250, .mnc = 2}},
  });

  Events::RadioParamsEvent<LteRadioParams> radio;
  radio.data.earfcn = 1850;
  radio.data.pci = 77;
  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = {.freq = 1850, .pci_bsic = 77},
      .rat = RatType::LTE,
      .event_data = Events::RrcEvent{std::move(radio)},
  });

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = {.freq = 1850, .pci_bsic = 77},
      .rat = RatType::LTE,
      .event_data =
          Events::PassportEvent{.passport = {.tac = 7828, .cell_id = 0x0BE8EFCC, .mcc = 250, .mnc = 2,
                                             .freq_band_ind = 3}},
  });

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].passport.cell_id == 0x0BE8EFCC);
  CHECK(snap[0].passport.tac == 7828);
  CHECK(snap[0].passport.mcc == 250);
  CHECK(snap[0].passport.freq_band_ind == 3);
}

TEST_CASE("CellTracker: LTE PLMN-only fan-out to multiple PCI on EARFCN", "[tracker][lte-merge]") {
  CellTracker tracker;

  Events::RadioParamsEvent<LteRadioParams> r1;
  r1.data.earfcn = 1850;
  r1.data.pci = 10;
  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = {.freq = 1850, .pci_bsic = 10},
      .rat = RatType::LTE,
      .event_data = Events::RrcEvent{std::move(r1)},
  });
  Events::RadioParamsEvent<LteRadioParams> r2;
  r2.data.earfcn = 1850;
  r2.data.pci = 20;
  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = {.freq = 1850, .pci_bsic = 20},
      .rat = RatType::LTE,
      .event_data = Events::RrcEvent{std::move(r2)},
  });

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = {.freq = 1850, .pci_bsic = 0},
      .rat = RatType::LTE,
      .event_data = Events::PassportEvent{.passport = {.mcc = 250, .mnc = 99}},
  });

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 2);
  for (const auto& c : snap) {
    CHECK(c.passport.mcc == 250);
    CHECK(c.passport.mnc == 99);
    CHECK(c.passport.cell_id == 0);
  }
}

TEST_CASE("CellTracker: SIB5 neigh PCIs upsert RADIO rows", "[tracker][sib5]") {
  CellTracker tracker;
  LocalCellKey key{.freq = 2660, .pci_bsic = 72};

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = key,
      .rat = RatType::LTE,
      .event_data = Events::PassportEvent{.passport = {.tac = 1, .mcc = 250, .mnc = 20}},
  });

  Events::InterFreqCarriersEvent ev;
  InterFreqCarrier car{.earfcn = 1300, .thresh_x_high = 10};
  car.neigh_pcis = {101, 202};
  ev.carriers.push_back(std::move(car));

  tracker.handle_rrc_event(
      Events::RrcEventEnvelope{.key = key, .rat = RatType::LTE, .event_data = std::move(ev)});

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 3);
  auto n1 = std::find_if(snap.begin(), snap.end(), [](const CellIdentity& c) {
    return c.radio.freq() == 1300 && c.radio.pci_bsic() == 101;
  });
  REQUIRE(n1 != snap.end());
  // Inter-freq must not inherit serving PLMN (other ops share SIB5 lists).
  CHECK(n1->passport.mcc == 0);
  CHECK(n1->passport.cell_id == 0);
}

TEST_CASE("CellTracker: passport merge is sticky on conflict", "[tracker][lte-merge]") {
  CellTracker tracker;
  LocalCellKey key{.freq = 3400, .pci_bsic = 100};

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = key,
      .rat = RatType::LTE,
      .event_data =
          Events::PassportEvent{.passport = {.tac = 111, .cell_id = 0x1111111, .mcc = 250, .mnc = 2}},
  });
  // Conflicting FULL from another source must not clobber.
  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = key,
      .rat = RatType::LTE,
      .event_data =
          Events::PassportEvent{.passport = {.tac = 222, .cell_id = 0x2222222, .mcc = 250, .mnc = 20}},
  });

  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].passport.cell_id == 0x1111111);
  CHECK(snap[0].passport.tac == 111);
  CHECK(snap[0].passport.mnc == 2);
}

TEST_CASE("CellTracker: MeasReport CGI merges onto neighbor PCI", "[tracker][cgi]") {
  CellTracker tracker;
  LocalCellKey serving{.freq = 1850, .pci_bsic = 10};

  Events::NeighborMeasEvent nev;
  NeighborMeasResult nr;
  nr.pci = 55;
  nr.has_cgi = true;
  nr.cgi = CellPassport{.tac = 99, .cell_id = 0x1234567, .mcc = 250, .mnc = 1};
  nev.neighbors.push_back(nr);

  tracker.handle_rrc_event(
      Events::RrcEventEnvelope{.key = serving, .rat = RatType::LTE, .event_data = std::move(nev)});

  auto snap = tracker.get_snapshot();
  auto it = std::find_if(snap.begin(), snap.end(),
                         [](const CellIdentity& c) { return c.radio.pci_bsic() == 55; });
  REQUIRE(it != snap.end());
  CHECK(it->passport.cell_id == 0x1234567);
  CHECK(it->passport.tac == 99);
  CHECK(it->passport.mcc == 250);
}

TEST_CASE("CellTracker: claim LTE ECI strips impostor PCI rows", "[tracker][eci]") {
  CellTracker tracker;
  // Fake CEREG stamp: same CID on two PCIs.
  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = {.freq = 3400, .pci_bsic = 135},
      .rat = RatType::LTE,
      .event_data =
          Events::PassportEvent{.passport = {.tac = 19719, .cell_id = 200468759, .mcc = 250, .mnc = 20}},
  });
  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = {.freq = 3400, .pci_bsic = 468},
      .rat = RatType::LTE,
      .event_data =
          Events::PassportEvent{.passport = {.tac = 19719, .cell_id = 200468759, .mcc = 250, .mnc = 20}},
  });

  auto snap = tracker.get_snapshot();
  auto owner = std::find_if(snap.begin(), snap.end(),
                            [](const CellIdentity& c) { return c.radio.pci_bsic() == 468; });
  auto impostor = std::find_if(snap.begin(), snap.end(),
                               [](const CellIdentity& c) { return c.radio.pci_bsic() == 135; });
  REQUIRE(owner != snap.end());
  REQUIRE(impostor != snap.end());
  CHECK(owner->passport.cell_id == 200468759);
  CHECK(impostor->passport.cell_id == 0);
  CHECK(impostor->passport.tac == 0);
  CHECK(impostor->passport.mcc == 250);  // PLMN kept
}

TEST_CASE("CellTracker: LTE EARFCN|0 rejects CID/TAC", "[tracker][eci]") {
  CellTracker tracker;
  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = {.freq = 3400, .pci_bsic = 0},
      .rat = RatType::LTE,
      .event_data =
          Events::PassportEvent{.passport = {.tac = 19719, .cell_id = 200468759, .mcc = 250, .mnc = 20}},
  });
  auto snap = tracker.get_snapshot();
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].passport.cell_id == 0);
  CHECK(snap[0].passport.tac == 0);
  CHECK(snap[0].passport.mcc == 250);
}

TEST_CASE("CellTracker: ever_serving requires Cell Identity", "[tracker][camped]") {
  CellTracker tracker;
  LocalCellKey key{.freq = 3400, .pci_bsic = 66};

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = key,
      .rat = RatType::LTE,
      .event_data = Events::ServingChangedEvent{.is_serving = true},
  });
  {
    auto snap = tracker.get_snapshot();
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].is_serving == true);
    CHECK(snap[0].ever_serving == false);
  }

  tracker.handle_rrc_event(Events::RrcEventEnvelope{
      .key = key,
      .rat = RatType::LTE,
      .event_data = Events::PassportEvent{
          .passport = {.tac = 10710, .cell_id = 200468759, .mcc = 250, .mnc = 20}},
  });
  {
    auto snap = tracker.get_snapshot();
    REQUIRE(snap.size() == 1);
    CHECK(snap[0].is_serving == true);
    CHECK(snap[0].ever_serving == true);
    CHECK(snap[0].passport.cell_id == 200468759);
  }
}
