/// Fixtures from scan_dumps/android_vlad_20260729 observer journal (2026-07-29).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <vector>

#include "core/QualcomParser.h"
#include "gsm/GsmParser.h"
#include "lte/LteParser.h"

using namespace QCom;
using Catch::Matchers::WithinAbs;

namespace {

std::vector<uint8_t> hex(const char* s) {
  std::vector<uint8_t> out;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
    int hi = nibble(s[i]), lo = nibble(s[i + 1]);
    if (hi < 0 || lo < 0) continue;
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return out;
}

template <typename EventType>
const EventType* find_event(const std::vector<Events::RrcEvent>& events) {
  for (const auto& ev : events) {
    if (auto* ptr = std::get_if<EventType>(&ev)) return ptr;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("dump 0xB197 v2 — EARFCN/PCI key", "[dump][lte]") {
  Lte::LteParser parser;
  auto payload = hex("02650000220B00006F010000000000006A0F0000000000000000000000000000C5FE0300");
  auto result = parser.parse({.log_code = 0xB197, .timestamp = 0, .payload = payload});
  REQUIRE(result.has_value());
  auto* serving = find_event<Events::ServingChangedEvent>(result.value());
  REQUIRE(serving);
  CHECK(serving->is_serving);
  auto* radio = find_event<Events::GenericRadioParamsEvent>(result.value());
  REQUIRE(radio);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(radio);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 2850);
  CHECK(lte->data.pci == 2);
}

TEST_CASE("dump 0xB17F v5 — RSRP after B197 PCI correction", "[dump][lte]") {
  QualcomParser qp;

  auto b197 = hex("0232F401C8000000D2030000000000004A3100007534CD28000000000000F46100000000");
  REQUIRE(qp.on_packet({.log_code = 0xB197, .timestamp = 1, .payload = b197}));

  auto b17f = hex("05010000C8000000D20F00009DD559009DD559000001041000B90C004AAD7E020000000010130000");
  REQUIRE(qp.on_packet({.log_code = 0xB17F, .timestamp = 2, .payload = b17f}));

  auto snap = qp.tracker().get_snapshot();
  REQUIRE_FALSE(snap.empty());

  const CellIdentity* serving = nullptr;
  for (const auto& c : snap) {
    if (c.is_serving && c.rat == RatType::LTE) serving = &c;
  }
  REQUIRE(serving);
  auto* radio = serving->radio_as_if<LteRadioParams>();
  REQUIRE(radio);
  CHECK(radio->earfcn == 200);
  CHECK(radio->pci == 7);  // corrected from unreliable B17F PCI

  auto* sig = serving->signal_as_if<LteSignalParams>();
  REQUIRE(sig);
  CHECK_THAT(sig->rsrp, WithinAbs(-90.0f, 1.5f));
}

TEST_CASE("dump 0x512F SI-3 — CID/LAI", "[dump][gsm]") {
  Gsm::GsmParser parser;
  auto payload = hex("49061B032D52F0024D07D004C8176040BD00008000051B");
  auto result = parser.parse({.log_code = 0x512F, .timestamp = 0, .payload = payload});
  REQUIRE(result.has_value());
  auto* pass = find_event<Events::PassportEvent>(result.value());
  REQUIRE(pass);
  CHECK(pass->passport.cell_id == 813);
  CHECK(pass->passport.mcc == 250);
  CHECK(pass->passport.mnc == 20);
  CHECK(pass->passport.tac == 19719);
}

TEST_CASE("dump 0x5B34 DSDS cell info — radio_id strip", "[dump][gsm]") {
  Gsm::GsmParser parser;
  auto payload = hex("015C9304062D0352F0024D0700FF");
  auto result = parser.parse({.log_code = 0x5B34, .timestamp = 0, .payload = payload});
  REQUIRE(result.has_value());

  auto* pass = find_event<Events::PassportEvent>(result.value());
  REQUIRE(pass);
  CHECK(pass->passport.cell_id == 813);
  CHECK(pass->passport.mcc == 250);
  CHECK(pass->passport.mnc == 20);
  CHECK(pass->passport.tac == 19719);

  auto* radio = find_event<Events::GenericRadioParamsEvent>(result.value());
  REQUIRE(radio);
  auto* gsm = std::get_if<Events::RadioParamsEvent<GsmRadioParams>>(radio);
  REQUIRE(gsm);
  CHECK(gsm->data.arfcn == 860);
  CHECK(gsm->data.ncc == 6);
  CHECK(gsm->data.bcc == 4);
}

TEST_CASE("dump 0x5A7A DSDS serving aux — rxlev", "[dump][gsm]") {
  Gsm::GsmParser parser;
  auto payload = hex("01C1FC00");
  auto result = parser.parse({.log_code = 0x5A7A, .timestamp = 0, .payload = payload});
  REQUIRE(result.has_value());
  auto* sig_ev = find_event<Events::SignalUpdateEvent>(result.value());
  REQUIRE(sig_ev);
  auto* gsm = sig_ev->signal.get_if<GsmSignalParams>();
  REQUIRE(gsm);
  CHECK(gsm->rxlev == static_cast<int8_t>(-831 * 0.0625));
}

TEST_CASE("dump QualcomParser wires GSM DSDS cell into tracker", "[dump][gsm]") {
  QualcomParser qp;
  auto payload = hex("015C9304062D0352F0024D0700FF");
  REQUIRE(qp.on_packet({.log_code = 0x5B34, .timestamp = 0, .payload = payload}));

  auto snap = qp.tracker().get_snapshot();
  REQUIRE(snap.size() == 1);
  CHECK(snap[0].rat == RatType::GSM);
  CHECK(snap[0].is_serving);
  CHECK(snap[0].passport.cell_id == 813);
  auto* radio = snap[0].radio_as_if<GsmRadioParams>();
  REQUIRE(radio);
  CHECK(radio->arfcn == 860);
}
