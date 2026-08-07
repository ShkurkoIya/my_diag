#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cstdio>
#include <span>
#include <vector>

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

  auto sv = std::span{payload};
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
  auto sv = std::span{payload};
  auto result = parser.parse_serv_cell_info(sv);
  REQUIRE(result.has_value());
  CHECK(result.value().empty());
}

TEST_CASE("0xB0C2 — too short returns error", "[lte][binary]") {
  LteParser parser;
  const uint8_t x[] = {0x00};
  auto result = parser.parse_serv_cell_info(std::span{x});
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

  auto sv = std::span{payload};
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

  auto sv = std::span{payload};
  auto result = parser.parse_ml1_serving(sv);
  REQUIRE(result.has_value());
  CHECK(result.value().empty());
}

TEST_CASE("0xB0C4 v2 — PLMN search response (SIM8300 capture)", "[lte][binary][plmn]") {
  LteParser parser;
  // Truncated live capture: header + PLMN entries (type 02/03).
  const uint8_t payload[] = {
      0x02, 0x03, 0x01, 0x23, 0x07, 0x00, 0x00, 0x00,
      0x02, 0x52, 0xF0, 0x20, 0x00, 0x00, 0x00, 0x00,  // 250-02 no earfcn
      0x02, 0x52, 0xF0, 0x02, 0x00, 0x00, 0x00, 0x00,  // 250-20 no earfcn
      0x03, 0x52, 0xF0, 0x02, 0x38, 0x18, 0x00, 0x00,  // 250-20 @ EARFCN 6200
      0x03, 0x52, 0xF0, 0x20, 0x3C, 0x06, 0x00, 0x00,  // 250-02 @ 1596
      0x03, 0x52, 0xF0, 0x11, 0x3C, 0x06, 0x00, 0x00,  // 250-11 @ 1596
      0x03, 0x52, 0xF0, 0x10, 0x77, 0x01, 0x00, 0x00,  // 250-01 @ 375
      0x03, 0x52, 0xF0, 0x99, 0x86, 0x05, 0x00, 0x00,  // 250-99 @ 1414
      0x90, 0x01, 0x00, 0x00,                          // end marker
  };

  auto result = parser.parse_plmn_search_rsp(std::span{payload});
  REQUIRE(result.has_value());
  auto& events = result.value();
  // 5 type-03 rows → Radio + Passport each
  REQUIRE(events.size() == 10);

  auto* radio0 = std::get_if<Events::GenericRadioParamsEvent>(&events[0]);
  REQUIRE(radio0);
  auto* lte0 = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(radio0);
  REQUIRE(lte0);
  CHECK(lte0->data.earfcn == 6200);

  auto* pass0 = std::get_if<Events::PassportEvent>(&events[1]);
  REQUIRE(pass0);
  CHECK(pass0->passport.mcc == 250);
  CHECK(pass0->passport.mnc == 20);
}

TEST_CASE("0xB176 — initial acquisition EARFCN+PCI", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[] = {
      0x20, 0x00, 0x00, 0x00, 0xFB, 0x04, 0x00, 0x00, 0x02, 0x84, 0x40, 0x28,
      0x48, 0x00, 0x00, 0x00, 0xFF, 0x27, 0xD9, 0x0B, 0xAE, 0x00, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x8E, 0x3B, 0x00, 0x00, 0xFF, 0x27, 0xD9, 0x0B,
      0x00, 0x00, 0xC0, 0x8B,
  };
  auto result = parser.parse_initial_acq(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 1275);
  CHECK(lte->data.pci == 174);
}

TEST_CASE("0xB194 0x1D — ML1 neighbour search response", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[] = {
      0x01, 0x01, 0x10, 0xC4, 0x1D, 0x28, 0x2C, 0x00, 0x44, 0x00, 0x00, 0x00,
      0x50, 0x26, 0x56, 0x26, 0xC0, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x48, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0xEB, 0xA1, 0x94,
      0x74, 0x0E, 0x00, 0x00, 0xD4, 0x01, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_search_rr(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 3400);
  CHECK(lte->data.pci == 468);
}

TEST_CASE("0xB194 0x1C — search request ignored", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[] = {
      0x01, 0x01, 0xCE, 0xC5, 0x1C, 0x28, 0x30, 0x00, 0x20, 0x02, 0x85, 0x00,
      0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x48, 0x0D, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0xB6, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_search_rr(std::span{payload});
  REQUIRE(result.has_value());
  CHECK(result->empty());
}

TEST_CASE("0xB194 SIM8300 — 0x1D not at offset 4", "[lte][binary]") {
  LteParser parser;
  // Live dump: 0x1D sits at offset 5, EARFCN@body+16 PCI@body+32
  const uint8_t payload[] = {
      0x01, 0x01, 0xF0, 0x7D, 0x5E, 0x1D, 0x28, 0x2C, 0x00, 0x44, 0x00, 0x00,
      0x00, 0xCA, 0x16, 0xD0, 0x16, 0xC0, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x48, 0x0D, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xCF, 0x57, 0x4C,
      0x88, 0x90, 0x0E, 0x00, 0x00, 0xD4, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_search_rr(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 3400);
  CHECK(lte->data.pci == 468);
}

TEST_CASE("0xB193 v48 — SIM8300 PCI low9 + RevWordBits RSRP", "[lte][binary][b193]") {
  LteParser parser;
  // Live dump: EARFCN 3400, PCI 468 (0x11d4 low9), serving bit12 set.
  const char* hex =
      "0101010019309c00480d0000010003000001ffffd4110000c525000050b30500a8d9320e"
      "c6cd150073f554004f0500006009960000302f00733557002eb9f4100f0100002eb9e412"
      "d57a160000000000d5020000f7fff8ff0000000034003600000000000000ff7f47975f01"
      "17457700c731030000000000df3f010048860000c33b030000000000000000002e010000"
      "1b190000591500000000000000000000";
  std::vector<uint8_t> payload;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    unsigned v = 0;
    REQUIRE(std::sscanf(hex + i, "%2x", &v) == 1);
    payload.push_back(static_cast<uint8_t>(v));
  }
  auto result = parser.parse_ml1_meas_resp(payload);
  REQUIRE(result.has_value());
  const Events::NeighborMeasEvent* nev = nullptr;
  const Events::ServingChangedEvent* srv = nullptr;
  for (const auto& ev : *result) {
    if (auto* n = std::get_if<Events::NeighborMeasEvent>(&ev)) nev = n;
    if (auto* s = std::get_if<Events::ServingChangedEvent>(&ev)) srv = s;
  }
  REQUIRE(nev);
  REQUIRE_FALSE(nev->neighbors.empty());
  CHECK(nev->neighbors[0].pci == 468);
  CHECK(nev->neighbors[0].has_rsrp);
  CHECK(nev->neighbors[0].rsrp_dbm < -40.0f);
  CHECK(nev->neighbors[0].rsrp_dbm > -150.0f);
  // Old >>7 path would invent PCI 35 and drop RSRP as -30 — must not regress.
  for (const auto& n : nev->neighbors) CHECK(n.pci != 35);
  CHECK(srv);
}

TEST_CASE("0xB175 v48 histogram — empty (not fake MIB BW)", "[lte][binary][b175]") {
  LteParser parser;
  std::vector<uint8_t> payload(384, 0);
  payload[0] = 48;
  payload[7] = 0x02;  // would previously false-unpack as ASN.1 MIB
  auto result = parser.parse_mib_metrics(payload);
  REQUIRE(result.has_value());
  CHECK(result->empty());
}

TEST_CASE("0xB179 v4 — connected intra EARFCN/PCI", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[] = {
      0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x0D, 0x00, 0x00,
      0xD4, 0x01, 0x77, 0x23, 0x6D, 0x05, 0x6D, 0x05, 0x5F, 0x01, 0x5F, 0x01,
      0x00, 0x00, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_conn_intra(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 3400);
  CHECK(lte->data.pci == 468);
}

TEST_CASE("0xB181 — serving + TLV candidate EARFCNs", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[] = {
      0x01, 0x02, 0x1C, 0x3C, 0x0A, 0x02, 0x0C, 0x00, 0x48, 0x0D, 0x00, 0x00,
      0xD4, 0x03, 0x00, 0x00, 0x0B, 0x28, 0x5C, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x06, 0x00, 0x00, 0x00, 0x38, 0x18, 0x00, 0x00, 0x79, 0x03, 0x08, 0x08,
      0x5E, 0x97, 0x00, 0x00, 0x79, 0x07, 0x0E, 0x0E, 0x24, 0x98, 0x00, 0x00,
      0x79, 0x07, 0x0E, 0x0E, 0xEA, 0x98, 0x00, 0x00, 0x79, 0x07, 0x0E, 0x0E,
      0x48, 0x0D, 0x00, 0x00, 0x79, 0x05, 0x0C, 0x0C, 0xFB, 0x04, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_intra_resel(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() >= 2);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 3400);
  CHECK(lte->data.pci == 468);
  auto* car = std::get_if<Events::InterFreqCarriersEvent>(&result->at(1));
  REQUIRE(car);
  REQUIRE(car->carriers.size() >= 3);
  CHECK(car->carriers[0].earfcn == 38750);
  CHECK(car->carriers[1].earfcn == 38948);
}

TEST_CASE("0xB192 — idle neigh meas (MI 26v2+27v4)", "[lte][binary]") {
  LteParser parser;
  // Live SIM8300 dump: pkt v1, 2 subpkts — request 26v2 + result 27v4, PCI 327 @ EARFCN 3400.
  const uint8_t payload[] = {
      0x01, 0x02, 0x44, 0xB7, 0x1A, 0x02, 0x1C, 0x00, 0x48, 0x0D, 0x00, 0x00,
      0x21, 0x00, 0x00, 0x00, 0x47, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1B, 0x04, 0x40, 0x00,
      0x48, 0x0D, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x47, 0x01, 0x00, 0x00,
      0xA8, 0x83, 0x3A, 0x00, 0xD6, 0x63, 0x3D, 0x00, 0xD6, 0x63, 0x3D, 0x00,
      0x7B, 0xEC, 0xC1, 0x08, 0x8C, 0x30, 0xC2, 0x08, 0x3E, 0x01, 0x00, 0x00,
      0x5A, 0xD1, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x00, 0x2A, 0x00,
      0x87, 0xE7, 0x02, 0x00, 0x87, 0xE7, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_neigh_req(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() >= 2);

  bool saw_radio = false;
  bool saw_meas = false;
  for (const auto& ev : *result) {
    if (auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&ev)) {
      auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
      REQUIRE(lte);
      CHECK(lte->data.earfcn == 3400);
      CHECK(lte->data.pci == 327);
      saw_radio = true;
    } else if (auto* nev = std::get_if<Events::NeighborMeasEvent>(&ev)) {
      REQUIRE_FALSE(nev->neighbors.empty());
      CHECK(nev->neighbors[0].pci == 327);
      CHECK(nev->neighbors[0].has_rsrp);
      // MI: (0x63D6 & 4095)*0.0625 - 180 = -118.625
      CHECK_THAT(nev->neighbors[0].rsrp_dbm, WithinAbs(-118.625f, 0.01f));
      saw_meas = true;
    }
  }
  CHECK(saw_radio);
  CHECK(saw_meas);
}

TEST_CASE("0xB195 — connected neigh meas (MI 31v4)", "[lte][binary]") {
  LteParser parser;
  // Synthetic: pkt v1, 1 subpkt id=31 ver=4 size=4+8+52=64, EARFCN 3400, PCI 327, RSRP raw 0x63D6.
  std::vector<uint8_t> payload(4 + 64, 0);
  payload[0] = 1;
  payload[1] = 1;
  payload[4] = 31;   // sid
  payload[5] = 4;    // sver
  payload[6] = 64;   // size lo
  payload[7] = 0;    // size hi
  // EARFCN 3400
  payload[8] = 0x48;
  payload[9] = 0x0D;
  // Num cells = 1 + pad
  payload[12] = 1;
  payload[13] = 0;
  payload[14] = 0;
  payload[15] = 0;
  // PCI 327 at cell+0
  payload[16] = 0x47;
  payload[17] = 0x01;
  // RSRP at cell+12
  payload[28] = 0xD6;
  payload[29] = 0x63;
  // RSRQ at cell+20: craft ((194)<<10) in low bits of a u32 → 194*0.0625-30 = -17.875
  const uint32_t rsrq_word = 194u << 10;
  payload[36] = static_cast<uint8_t>(rsrq_word & 0xFF);
  payload[37] = static_cast<uint8_t>((rsrq_word >> 8) & 0xFF);
  payload[38] = static_cast<uint8_t>((rsrq_word >> 16) & 0xFF);
  payload[39] = static_cast<uint8_t>((rsrq_word >> 24) & 0xFF);

  auto result = parser.parse_ml1_conn_neigh(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->empty());

  bool saw_meas = false;
  for (const auto& ev : *result) {
    if (auto* nev = std::get_if<Events::NeighborMeasEvent>(&ev)) {
      REQUIRE(nev->neighbors.size() == 1);
      CHECK(nev->neighbors[0].pci == 327);
      CHECK(nev->neighbors[0].has_rsrp);
      CHECK_THAT(nev->neighbors[0].rsrp_dbm, WithinAbs(-118.625f, 0.01f));
      saw_meas = true;
    }
  }
  CHECK(saw_meas);
}

TEST_CASE("0xB195 — SIM8300 subpkt 31 ver=40 wide", "[lte][binary][b195]") {
  LteParser parser;
  // Live dump B195[1]: n_sub=2, req 0x1E + result 0x1F ver=40, EARFCN 3400, PCI 468.
  const char* hex =
      "01029fff1e282000480d0000a1000000f1810000d4110000405200004052000000000000"
      "1f284000480d000001340802d40100001005510022255200222552003bedc4133cf1c413"
      "e6010000f6b10f000000000034003500405200004052000000000000";
  std::vector<uint8_t> payload;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    auto nib = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + c - 'a');
      if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + c - 'A');
      return 0;
    };
    payload.push_back(static_cast<uint8_t>((nib(hex[i]) << 4) | nib(hex[i + 1])));
  }
  auto result = parser.parse_ml1_conn_neigh(std::span{payload});
  REQUIRE(result.has_value());
  bool saw = false;
  for (const auto& ev : *result) {
    if (auto* nev = std::get_if<Events::NeighborMeasEvent>(&ev)) {
      REQUIRE_FALSE(nev->neighbors.empty());
      CHECK(nev->neighbors[0].pci == 468);
      CHECK(nev->neighbors[0].has_rsrp);
      saw = true;
    }
    if (auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&ev)) {
      auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
      REQUIRE(lte);
      CHECK(lte->data.earfcn == 3400);
      CHECK(lte->data.pci == 468);
    }
  }
  CHECK(saw);
}

TEST_CASE("0xB179 v1 shifted layout", "[lte][binary]") {
  LteParser parser;
  // Live: ver=1 earfcn 1275 @9, pci 174 @13
  const uint8_t payload[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xfb,
                             0x04, 0x00, 0x00, 0xae, 0x00, 0x77, 0x20, 0x01, 0x06, 0x01,
                             0x06, 0x68, 0x01, 0x68, 0x01, 0x00, 0x00, 0x00};
  auto result = parser.parse_ml1_conn_intra(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->empty());
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 1275);
  CHECK(lte->data.pci == 174);
}

TEST_CASE("0xB195 — connected neigh meas (MI 31v24)", "[lte][binary]") {
  LteParser parser;
  // Same as v4 layout; MI also documents subpkt ver=24 with Payload_31v4.
  std::vector<uint8_t> payload(4 + 64, 0);
  payload[0] = 1;
  payload[1] = 1;
  payload[4] = 31;
  payload[5] = 24;
  payload[6] = 64;
  payload[8] = 0x48;
  payload[9] = 0x0D;
  payload[12] = 1;
  payload[16] = 0x47;
  payload[17] = 0x01;
  payload[28] = 0xD6;
  payload[29] = 0x63;
  const uint32_t rsrq_word = 194u << 10;
  payload[36] = static_cast<uint8_t>(rsrq_word & 0xFF);
  payload[37] = static_cast<uint8_t>((rsrq_word >> 8) & 0xFF);
  payload[38] = static_cast<uint8_t>((rsrq_word >> 16) & 0xFF);
  payload[39] = static_cast<uint8_t>((rsrq_word >> 24) & 0xFF);

  auto result = parser.parse_ml1_conn_neigh(std::span{payload});
  REQUIRE(result.has_value());
  auto* nev = find_event<Events::NeighborMeasEvent>(*result);
  REQUIRE(nev);
  REQUIRE(nev->neighbors.size() == 1);
  CHECK(nev->neighbors[0].pci == 327);
  CHECK_THAT(nev->neighbors[0].rsrp_dbm, WithinAbs(-118.625f, 0.01f));
}

TEST_CASE("0xB115 — SSS EARFCN as inter-freq carrier", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[] = {0x7A, 0x00, 0x00, 0x00, 0x24, 0x98, 0x00, 0x00};
  auto result = parser.parse_ll1_sss(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  auto* car = std::get_if<Events::InterFreqCarriersEvent>(&result->at(0));
  REQUIRE(car);
  REQUIRE(car->carriers.size() == 1);
  CHECK(car->carriers[0].earfcn == 38948);
}

TEST_CASE("0xB123 CER — no invented cells", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[160] = {0x29, 0x47, 0x01, 0x00};
  auto result = parser.parse_ll1_ncell_cer(std::span{payload});
  REQUIRE(result.has_value());
  CHECK(result->empty());
}

