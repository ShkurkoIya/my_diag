#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <span>

#include "core/Utils.h"

using namespace QCom::Utils;
using Catch::Matchers::WithinAbs;

TEST_CASE("LE reader extracts values correctly", "[utils]") {
  const uint8_t data[] = {0x64, 0x0A, 0x48, 0x00, 0xFF};
  auto sv = std::span{data};

  CHECK(Converter::read_le<uint16_t>(sv, 0) == 0x0A64);  // 2660
  CHECK(Converter::read_le<uint16_t>(sv, 2) == 0x0048);  // 72
  CHECK(Converter::read_le<uint8_t>(sv, 4) == 0xFF);
}

TEST_CASE("LE reader returns zero on out-of-bounds", "[utils]") {
  const uint8_t ab_data[] = {0x41, 0x42};
  auto sv = std::span{ab_data};
  CHECK(Converter::read_le<uint32_t>(sv, 0) == 0);
  CHECK(Converter::read_le<uint16_t>(sv, 2) == 0);
}

TEST_CASE("bits() extracts bitfields", "[utils]") {
  CHECK(bits(0xDEADBEEF, 0, 8) == 0xEF);
  CHECK(bits(0xDEADBEEF, 8, 8) == 0xBE);
  CHECK(bits(0xDEADBEEF, 4, 12) == 0xBEE);
  CHECK(bits(0x00002400, 7, 9) == 72);  // PCI extraction: (0x2400 >> 7) & 0x1FF
}

TEST_CASE("ML1 conversion formulas", "[utils]") {
  SECTION("RSRP: raw=1600 -> -80 dBm") { CHECK_THAT(ml1_rsrp(1600), WithinAbs(-80.0, 0.1)); }
  SECTION("RSRQ: raw=160 -> -20 dB") { CHECK_THAT(ml1_rsrq(160), WithinAbs(-20.0, 0.1)); }
  SECTION("RSSI: raw=800 -> -60 dBm") { CHECK_THAT(ml1_rssi(800), WithinAbs(-60.0, 0.1)); }
  SECTION("NR SINR: raw=320 -> 0 dB") { CHECK_THAT(ml1_nr_sinr(320), WithinAbs(0.0, 0.1)); }
}

TEST_CASE("Validity checks", "[utils]") {
  SECTION("LTE EARFCN") {
    CHECK(valid_lte_earfcn(2660));
    CHECK(valid_lte_earfcn(70645));
    CHECK_FALSE(valid_lte_earfcn(0));
    CHECK_FALSE(valid_lte_earfcn(70646));
  }
  SECTION("LTE PCI") {
    CHECK(valid_lte_pci(0));
    CHECK(valid_lte_pci(503));
    CHECK_FALSE(valid_lte_pci(504));
  }
  SECTION("LTE RSRP") {
    CHECK(valid_lte_rsrp(-100.0f));
    CHECK(valid_lte_rsrp(-140.0f));
    CHECK(valid_lte_rsrp(-150.0f));
    CHECK_FALSE(valid_lte_rsrp(-39.0f));
    CHECK_FALSE(valid_lte_rsrp(-30.0f));
    CHECK_FALSE(valid_lte_rsrp(-151.0f));
    CHECK_FALSE(valid_lte_rsrp(-181.0f));
  }
  SECTION("NR") {
    CHECK(valid_nr_arfcn(620000));
    CHECK(valid_nr_pci(1007));
    CHECK_FALSE(valid_nr_pci(1008));
  }
}
