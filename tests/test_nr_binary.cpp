#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <span>
#include <vector>

#include <observer/model/CellIdentity.h>
#include <observer/model/Events.h>
#include "nr/NrParser.h"

using namespace QCom;
using namespace QCom::Nr;

namespace {

std::vector<uint8_t> from_hex(const char* hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    unsigned v = 0;
    REQUIRE(std::sscanf(hex + i, "%2x", &v) == 1);
    out.push_back(static_cast<uint8_t>(v));
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

TEST_CASE("0xB823 v0.4 — NR serving cell (scat fixture)", "[nr][binary]") {
  NrParser parser;
  // scat test_parse_nr_rrc_scell_info version 0.4
  auto payload = from_hex(
      "040000009d02e0ca0900d6c609005a005a0000127df204000000060102010001297900004e00");
  auto result = parser.parse_serv_cell_info(payload);
  REQUIRE(result.has_value());
  REQUIRE(result->size() >= 2);

  auto* gen = find_event<Events::GenericRadioParamsEvent>(*result);
  REQUIRE(gen);
  auto* radio = std::get_if<Events::RadioParamsEvent<NrRadioParams>>(gen);
  REQUIRE(radio);
  CHECK(radio->data.pci == 669);
  CHECK(radio->data.nrarfcn == 641760);
  CHECK(radio->data.ul_nrarfcn == 640726);
  CHECK(radio->data.dl_bw == 90);
  CHECK(radio->data.band == 78);

  auto* pass = find_event<Events::PassportEvent>(*result);
  REQUIRE(pass);
  CHECK(pass->passport.mcc == 262);
  CHECK(pass->passport.mnc == 1);
  CHECK(pass->passport.mnc_digits == 2);
  CHECK(pass->passport.tac == 0x7929);
  CHECK(pass->passport.cell_id == 0x4f27d1200ull);
}

TEST_CASE("0xB822 v2.0 — NR MIB (scat fixture)", "[nr][binary]") {
  NrParser parser;
  auto payload = from_hex("0000020050010eb005001e036a1b0c");
  auto result = parser.parse_rrc_mib(payload);
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* radio = std::get_if<Events::RadioParamsEvent<NrRadioParams>>(gen);
  REQUIRE(radio);
  CHECK(radio->data.pci == 336);
  CHECK(radio->data.nrarfcn == 372750);
  CHECK(radio->data.sfn == 798);
  CHECK(radio->data.scs_khz == 15);
}
