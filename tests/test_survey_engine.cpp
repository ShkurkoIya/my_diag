#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <observer/model/CellIdentity.h>
#include <observer/model/Types.h>
#include <observer/engine/ModemControl.h>
#include <qcom/linux/SimcomAtControl.h>
#include <observer/engine/SurveyProjection.h>
#include <observer/engine/SurveySession.h>
#include <observer/engine/SurveyStrategy.h>

#include "TowerExport.h"

using namespace QCom;
using namespace QCom::Engine;

namespace {

struct LteSpec {
  uint32_t earfcn{0};
  uint16_t pci{0};
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint64_t cid{0};
  uint16_t tac{0};
  float rsrp{-160.0f};
  bool serving{false};
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
  c.is_serving = s.serving;
  c.ever_serving = s.serving;
  return c;
}

/// Records every intent the runner asked it to perform (full-caps modem).
struct FakeControl final : IModemControl {
  std::vector<SurveyIntent> applied;
  [[nodiscard]] ModemCaps caps() const noexcept override { return ModemCaps::full(); }
  [[nodiscard]] std::string_view name() const noexcept override { return "fake-full"; }
  ControlStatus apply(const SurveyIntent& i) override {
    applied.push_back(i);
    return ControlStatus::Ok;
  }
};

struct CollectSink final : ISurveySink {
  std::vector<Tower> identified;
  int results{0};
  void on_result(const SurveyResult&) override { ++results; }
  void on_tower_identified(const Tower& t) override { identified.push_back(t); }
};

}  // namespace

TEST_CASE("project_lte: honest counts, sites, operator grouping", "[engine][projection]") {
  std::vector<CellIdentity> cells;
  // Two FULL carriers on the SAME eNB (ECI>>8 equal) → 1 site, operator towers=2.
  cells.push_back(make_lte({.earfcn = 375,
                            .pci = 156,
                            .mcc = 250,
                            .mnc = 1,
                            .cid = 51830553,
                            .tac = 17806,
                            .rsrp = -80.0f,
                            .serving = true}));
  cells.push_back(make_lte({.earfcn = 6275,
                            .pci = 278,
                            .mcc = 250,
                            .mnc = 1,
                            .cid = 51830536,
                            .tac = 17806,
                            .rsrp = -95.0f}));
  // RF-only detection (no CID) — counts as RF, not a tower.
  cells.push_back(make_lte({.earfcn = 1850, .pci = 44, .rsrp = -100.0f}));

  const auto r = project_lte(cells);
  CHECK(r.stats.lte_rf_unique == 3);
  CHECK(r.stats.lte_full == 2);
  CHECK(r.stats.lte_sites == 1);  // same eNB 202463
  CHECK(r.stats.lte_serving == 1);
  REQUIRE(r.towers.size() == 2);
  CHECK(r.towers[0].earfcn == 375);  // sorted by earfcn
  CHECK(r.towers[1].earfcn == 6275);
  CHECK(r.towers[0].enb_id() == r.towers[1].enb_id());
  REQUIRE(r.operators.size() == 1);
  CHECK(r.operators[0].mcc == 250);
  CHECK(r.operators[0].towers == 2);
  CHECK(r.operators[0].sites == 1);
}

TEST_CASE("capability negotiation downgrades active walk on a read-only modem", "[engine][caps]") {
  CHECK(caps_satisfy(ModemCaps::full(), LteWalkStrategy{}.required_caps()));
  CHECK_FALSE(caps_satisfy(ModemCaps::none(), LteWalkStrategy{}.required_caps()));

  auto session = SurveySession::builder()
                     .control(std::make_unique<NullModemControl>())
                     .strategy(std::make_unique<LteWalkStrategy>())
                     .build();
  CHECK(session.downgraded_to_passive());
  CHECK(session.strategy_name() == "passive-monitor");
}

TEST_CASE("LteWalkStrategy locks best target, advances when it becomes FULL",
          "[engine][strategy]") {
  using namespace std::chrono_literals;
  LteWalkStrategy strat(LteWalkStrategy::Params{.dwell = 5s, .frontier = 8, .full_walk = true});
  const auto t0 = std::chrono::steady_clock::time_point{};

  // Snapshot 1: one incomplete measured cell → strategy locks it.
  std::vector<CellIdentity> snap = {make_lte({.earfcn = 375, .pci = 156, .rsrp = -80.0f})};
  auto i1 = strat.decide({.snapshot = snap, .now = t0});
  REQUIRE(i1.kind == SurveyIntent::Kind::LockCell);
  CHECK(i1.earfcn == 375);
  CHECK(i1.pci == 156);

  // Still camping, not yet FULL, within dwell → Idle (hold the lock).
  auto i2 = strat.decide({.snapshot = snap, .now = t0 + 1s});
  CHECK(i2.kind == SurveyIntent::Kind::Idle);

  // Cell becomes FULL → target satisfied, marked visited; nothing else to do.
  snap[0] = make_lte({.earfcn = 375,
                      .pci = 156,
                      .mcc = 250,
                      .mnc = 1,
                      .cid = 51830553,
                      .tac = 17806,
                      .rsrp = -80.0f});
  auto i3 = strat.decide({.snapshot = snap, .now = t0 + 2s});
  CHECK(i3.kind == SurveyIntent::Kind::Idle);
  CHECK(strat.visited_count() == 1);
}

TEST_CASE("LteWalkStrategy advances to next target after dwell expiry", "[engine][strategy]") {
  using namespace std::chrono_literals;
  LteWalkStrategy strat(LteWalkStrategy::Params{.dwell = 5s, .frontier = 8, .full_walk = true});
  const auto t0 = std::chrono::steady_clock::time_point{};

  std::vector<CellIdentity> snap = {
      make_lte({.earfcn = 375, .pci = 156, .rsrp = -70.0f}),  // stronger → locked first
      make_lte({.earfcn = 375, .pci = 200, .rsrp = -95.0f}),
  };
  auto first = strat.decide({.snapshot = snap, .now = t0});
  REQUIRE(first.kind == SurveyIntent::Kind::LockCell);
  CHECK(first.pci == 156);

  // Never becomes FULL; dwell expires → strategy abandons and locks the next.
  auto second = strat.decide({.snapshot = snap, .now = t0 + 6s});
  REQUIRE(second.kind == SurveyIntent::Kind::LockCell);
  CHECK(second.pci == 200);
  CHECK(strat.visited_count() == 1);
}

TEST_CASE("SurveySession: offline B0C2 ingest → tower projected + sink notified",
          "[engine][facade]") {
  // Real QXDM B0C2 v3 payload: PCI 156, EARFCN 375, CID 51830553, TAC 17806, 250-01.
  const std::vector<uint8_t> b0c2 = {0x03, 0x9C, 0x00, 0x77, 0x01, 0x00, 0x00, 0xC7, 0x47, 0x00,
                                     0x00, 0x4B, 0x4B, 0x19, 0xDF, 0x16, 0x03, 0x8E, 0x45, 0x01,
                                     0x00, 0x00, 0x00, 0xFA, 0x00, 0x02, 0x01, 0x00, 0x00};

  CollectSink sink;
  auto session = SurveySession::builder()
                     .control(std::make_unique<FakeControl>())
                     .strategy(std::make_unique<LteWalkStrategy>())
                     .build();
  session.add_sink(&sink);
  CHECK_FALSE(session.downgraded_to_passive());  // FakeControl has full caps

  session.ingest(QualcommPacketView{
      .log_code = 0xB0C2, .timestamp = 1, .payload = std::span<const uint8_t>{b0c2}});
  const auto& r = session.refresh();

  REQUIRE(r.towers.size() == 1);
  CHECK(r.towers[0].earfcn == 375);
  CHECK(r.towers[0].pci == 156);
  CHECK(r.towers[0].eci == 51830553u);
  CHECK(r.towers[0].mcc == 250);
  CHECK(r.stats.lte_full == 1);
  REQUIRE(sink.identified.size() == 1);
  CHECK(sink.identified[0].eci == 51830553u);
  CHECK(sink.results >= 1);

  // Re-projecting must NOT re-fire on_tower_identified for the same ECI.
  session.refresh();
  CHECK(sink.identified.size() == 1);
}

TEST_CASE("builder picks walk vs passive from caps+config", "[engine][facade]") {
  auto walk = SurveySession::builder()
                  .control(std::make_unique<FakeControl>())
                  .config({.active_walk = true})
                  .build();
  CHECK_FALSE(walk.downgraded_to_passive());
  CHECK(walk.strategy_name() == "lte-active-walk");

  auto pass = SurveySession::builder()
                  .control(std::make_unique<FakeControl>())
                  .config({.active_walk = false})
                  .build();
  CHECK(pass.strategy_name() == "passive-monitor");

  auto android = SurveySession::builder()
                     .control(std::make_unique<AndroidControl>())
                     .config({.active_walk = true})
                     .strategy(std::make_unique<LteWalkStrategy>())
                     .build();
  CHECK(android.downgraded_to_passive());
  CHECK(android.strategy_name() == "passive-monitor");
  CHECK(android.caps().diag_log_mask);
  CHECK_FALSE(android.caps().cell_lock);
}

TEST_CASE("SimcomAt dialect: dual-lock command order and COPS PLMN", "[engine][simcom]") {
  CHECK(SimcomAt::cmd_ccellcfg_lock(156, 375) == "AT+CCELLCFG=1,156,375");
  CHECK(SimcomAt::cmd_clecell_lock(375, 156) == "AT+CLECELL=375,156");
  CHECK(SimcomAt::cmd_clearfcn_lock(1, 375) == "AT+CLEARFCN=1,375");
  CHECK(SimcomAt::cmd_cops_manual(250, 1) == "AT+COPS=1,2,\"25001\"");
  CHECK(SimcomAt::cmd_cops_manual(310, 410) == "AT+COPS=1,2,\"310410\"");

  uint16_t pci = 0;
  uint32_t earfcn = 0;
  REQUIRE(SimcomAt::parse_ccellcfg("+CCELLCFG: 156,375\r\nOK", pci, earfcn));
  CHECK(pci == 156);
  CHECK(earfcn == 375);
  REQUIRE(SimcomAt::parse_clecell("+CLECELL: 375,156\r\nOK", earfcn, pci));
  CHECK(earfcn == 375);
  CHECK(pci == 156);

  std::vector<std::string> cmds;
  SimcomAtControl ctl([&](std::string_view cmd, int) -> std::optional<std::string> {
    cmds.emplace_back(cmd);
    if (cmd == "AT+CCELLCFG?") return std::string("+CCELLCFG: 156,375\r\nOK");
    if (cmd == "AT+CLECELL?") return std::string("+CLECELL: 375,156\r\nOK");
    return std::string("\r\nOK");
  });
  CHECK(ctl.name() == "simcom-at");
  CHECK(ctl.caps().can_active_walk());
  CHECK(ctl.apply(SurveyIntent::lock(375, 156)) == ControlStatus::Ok);
  REQUIRE(cmds.size() >= 5);
  CHECK(cmds[0] == "AT+CLEARFCN=1,375");
  CHECK(cmds[1] == "AT+CCELLCFG=1,156,375");
  CHECK(cmds[2] == "AT+CCELLCFG?");
  CHECK(cmds[3] == "AT+CLECELL=375,156");
  CHECK(cmds[4] == "AT+CLECELL?");

  cmds.clear();
  CHECK(ctl.apply(SurveyIntent::unlock()) == ControlStatus::Ok);
  REQUIRE(cmds.size() >= 3);
  CHECK(cmds[0] == "AT+CCELLCFG=0");
  CHECK(cmds[1] == "AT+CLECELL");
  CHECK(cmds[2] == "AT+CLEARFCN");

  cmds.clear();
  CHECK(ctl.apply(SurveyIntent::select_plmn(250, 1)) == ControlStatus::Ok);
  REQUIRE_FALSE(cmds.empty());
  CHECK(cmds[0] == "AT+COPS=1,2,\"25001\"");

  cmds.clear();
  CHECK(ctl.apply(SurveyIntent::set_rf(false)) == ControlStatus::Ok);
  REQUIRE_FALSE(cmds.empty());
  CHECK(cmds[0] == "AT+CFUN=4");
}

TEST_CASE("SimcomAtControl returns Failed when neither lock sticks", "[engine][simcom]") {
  SimcomAtControl ctl([](std::string_view cmd, int) -> std::optional<std::string> {
    if (cmd.ends_with("?")) return std::string("+CCELLCFG: 0,0\r\nOK");
    return std::string("\r\nERROR");
  });
  CHECK(ctl.apply(SurveyIntent::lock(375, 156)) == ControlStatus::Failed);
}

TEST_CASE("encode_document_survey empty does not throw glaze variant", "[json]") {
  const auto s = QCom::Tools::encode_document_survey({}, "test");
  REQUIRE(s.find("qcom.towers.v5") != std::string::npos);
  REQUIRE(s.find("\"lte\"") != std::string::npos);
}
