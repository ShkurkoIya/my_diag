#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/CellIdentity.h"
#include "core/Events.h"
#include "lte/LteParser.h"

using namespace QCom;
using namespace QCom::Lte;
using Catch::Matchers::WithinAbs;

namespace {

template <typename EventType>
const EventType* find_event(const std::vector<Events::RrcEvent>& events) {
  for (const auto& ev : events) {
    if (auto* ptr = std::get_if<EventType>(&ev)) return ptr;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("0xB0C2 v2 — serving cell identity", "[lte][binary]") {
  LteParser parser;

  // Version 2: PCI=72, EARFCN=2660, UL_EARFCN=20660, DL_BW=15, UL_BW=10
  // CID=0x01234567, TAC=0x00AB, Band=7, MCC=250, MNC_digits=2, MNC=01
  uint8_t payload[] = {
      0x02,                    // version = 2
      0x48, 0x00,              // PCI = 72
      0x64, 0x0A,              // DL EARFCN = 2660
      0xB4, 0x50,              // UL EARFCN = 20660
      0x0F, 0x0A,              // DL_BW=15, UL_BW=10
      0x67, 0x45, 0x23, 0x01,  // Cell ID = 0x01234567
      0xAB, 0x00,              // TAC = 0x00AB
      0x07, 0x00, 0x00, 0x00,  // Band = 7
      0xFA, 0x00,              // MCC = 250
      0x02,                    // MNC digits = 2
      0x01, 0x00,              // MNC = 01
      0x00,                    // allowed_access
  };

  auto sv = std::string_view(reinterpret_cast<const char*>(payload), sizeof(payload));
  auto result = parser.parse_serv_cell_info(sv);

  REQUIRE(result.has_value());
  auto& events = result.value();
  REQUIRE(events.size() >= 2);

  auto* passport_ev = find_event<Events::PassportEvent>(events);
  REQUIRE(passport_ev);
  CHECK(passport_ev->passport.cell_id == 0x01234567);
  CHECK(passport_ev->passport.tac == 0xAB);
  CHECK(passport_ev->passport.mcc == 250);
  CHECK(passport_ev->passport.mnc == 1);
}

TEST_CASE("0xB0C2 — unknown version returns empty", "[lte][binary]") {
  LteParser parser;
  uint8_t payload[] = {0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  auto sv = std::string_view(reinterpret_cast<const char*>(payload), sizeof(payload));
  auto result = parser.parse_serv_cell_info(sv);
  REQUIRE(result.has_value());
  CHECK(result.value().empty());
}

TEST_CASE("0xB0C2 — too short returns error", "[lte][binary]") {
  LteParser parser;
  auto result = parser.parse_serv_cell_info("x");
  REQUIRE(!result.has_value());
  CHECK(result.error() == ParserError::PacketTooShort);
}

TEST_CASE("0xB17F v4 — serving meas with valid RSRP", "[lte][binary]") {
  LteParser parser;

  // RSRP raw = 1600 -> -80 dBm (valid)
  // RSRQ raw = bits[22:] of word at +16
  // RSSI raw = bits[11:22] of word at +20
  uint8_t payload[28] = {};
  payload[0] = 4;  // version
  // EARFCN=2660 at +4
  payload[4] = 0x64;
  payload[5] = 0x0A;
  // PCI_SLP = 72<<7 = 0x2400 at +6
  payload[6] = 0x00;
  payload[7] = 0x24;
  // RSRP word at +8: raw=1600 in low 12 bits = 0x640
  payload[8] = 0x40;
  payload[9] = 0x06;
  payload[10] = 0x00;
  payload[11] = 0x00;
  // RSRQ word at +16: raw in top 10 bits = 160 << 22 = 0x28000000
  payload[16] = 0x00;
  payload[17] = 0x00;
  payload[18] = 0x00;
  payload[19] = 0x28;
  // RSSI word at +20: raw in bits [11:22] = 800 << 11 = 0x190000
  payload[20] = 0x00;
  payload[21] = 0x90;
  payload[22] = 0x01;
  payload[23] = 0x00;

  auto sv = std::string_view(reinterpret_cast<const char*>(payload), sizeof(payload));
  auto result = parser.parse_ml1_serving(sv);

  REQUIRE(result.has_value());
  auto& events = result.value();
  REQUIRE(!events.empty());

  auto* sig_ev = find_event<Events::SignalUpdateEvent>(events);
  REQUIRE(sig_ev);

  auto* lte_sig = sig_ev->signal.get_if<LteSignalParams>();
  REQUIRE(lte_sig);
  CHECK_THAT(lte_sig->rsrp, WithinAbs(-80.0, 0.5));
}

TEST_CASE("0xB17F — invalid RSRP is rejected", "[lte][binary]") {
  LteParser parser;

  uint8_t payload[28] = {};
  payload[0] = 4;
  payload[4] = 0x64;
  payload[5] = 0x0A;
  // RSRP raw = 0xFFF -> ~-67.2 + noise = something invalid
  // Actually 0xFFF = 4095 -> 4095*0.0625-180 = 75.9 (way invalid, >-30)
  payload[8] = 0xFF;
  payload[9] = 0x0F;

  auto sv = std::string_view(reinterpret_cast<const char*>(payload), sizeof(payload));
  auto result = parser.parse_ml1_serving(sv);
  REQUIRE(result.has_value());
  CHECK(result.value().empty());
}
