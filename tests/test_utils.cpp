#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <span>

#include <observer/model/Utils.h>

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
  SECTION("FTL SNR: raw=304 -> 10.4 dB") { CHECK_THAT(ml1_ftl_snr(304), WithinAbs(10.4, 0.01)); }
  SECTION("NR SINR: raw=320 -> 0 dB") { CHECK_THAT(ml1_nr_sinr(320), WithinAbs(0.0, 0.1)); }
}

TEST_CASE("ML1 PCI pack is decided by refutation", "[utils][pci]") {
  using P = LtePciPack;
  // scat PCI 72 << 7, low bits empty → MSB.
  CHECK(lte_pci_pack_from_word(0x2400) == P::Msb);
  CHECK(lte_unpack_pci_slp(0x2400).pci == 72);
  CHECK(lte_pci_from_meas_word(0x2400) == 72);

  // Same 9-bit PCI in low bits; high bits are flags. MSB reads as 31 / 7 (stuck).
  CHECK(lte_pci_pack_from_word(0x0FD2) == P::Lsb);
  CHECK(lte_unpack_pci_slp(0x0FD2).pci == 466);
  CHECK(lte_pci_pack_from_word(0x03D2) == P::Lsb);
  CHECK(lte_unpack_pci_slp(0x03D2).pci == 466);
  CHECK(lte_pci_lsb(0x0FD2) == lte_pci_lsb(0x03D2));

  // Android 0x0FEB: MSB 31 vs LSB 491.
  CHECK(lte_pci_pack_from_word(0x0FEB) == P::Lsb);
  CHECK(lte_unpack_pci_slp(0x0FEB).pci == 491);

  LtePciPackOrder order;
  for (uint16_t w : {uint16_t{0x0FD2}, uint16_t{0x03D2}, uint16_t{0x0FEB}}) order.observe(w);
  CHECK(order.locked == P::Lsb);
  CHECK(order.unpack(0x03D2).pci == 466);
}

TEST_CASE("Validity checks", "[utils]") {
  SECTION("LTE EARFCN") {
    CHECK(valid_lte_earfcn(2660));
    CHECK(valid_lte_earfcn(70645));
    CHECK(valid_lte_earfcn(262143));  // 18-bit EARFCN field max (TS 36.101)
    CHECK_FALSE(valid_lte_earfcn(0));
    CHECK_FALSE(valid_lte_earfcn(262144));
    CHECK_FALSE(valid_lte_earfcn(0xFFFFFFFF));  // modem padding
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
