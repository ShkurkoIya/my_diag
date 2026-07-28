#include <catch2/catch_test_macros.hpp>
#include <span>

#include "core/ParserInterface.h"
#include "lte/LteParser.h"

using namespace QCom;
using namespace QCom::Lte;

TEST_CASE("LteParser: RRC OTA with too-short payload returns error", "[lte][asn1]") {
  LteParser parser;
  const uint8_t short_data[] = {1, 2, 3, 4, 5};
  auto result = parser.parse_rrc_ota(std::span{short_data});
  REQUIRE(!result.has_value());
  CHECK(result.error() == ParserError::PacketTooShort);
}

TEST_CASE("LteParser: RRC OTA with unknown channel type returns error", "[lte][asn1]") {
  LteParser parser;
  // 7 bytes of metadata with channel_type = 0xFF (unknown)
  uint8_t payload[] = {0x00, 0x00, 0x64, 0x0A, 0x48, 0x00, 0xFF, 0x00, 0x01};
  auto sv = std::span{payload};
  auto result = parser.parse_rrc_ota(sv);
  REQUIRE(!result.has_value());
  CHECK(result.error() == ParserError::UnknownChannelType);
}

TEST_CASE("LteParser: parse_metadata extracts EARFCN and PCI", "[lte][asn1]") {
  LteParser parser;
  uint8_t meta[] = {0x01, 0x00, 0x64, 0x0A, 0x48, 0x00, 0x01};
  auto sv = std::span{meta};

  auto key = parser.parse_metadata(sv);
  REQUIRE(key.has_value());
  CHECK(key->freq == 2660);
  CHECK(key->pci_bsic == 72);
}

TEST_CASE("LteParser: parse_metadata too short returns nullopt", "[lte][asn1]") {
  LteParser parser;
  const uint8_t abc[] = {1, 2, 3};
  auto key = parser.parse_metadata(std::span{abc});
  CHECK(!key.has_value());
}

TEST_CASE("LteParser: MIB too short returns error", "[lte][asn1]") {
  LteParser parser;
  const uint8_t short_mib[] = {1, 2, 3, 4, 5};
  auto result = parser.parse_mib_metrics(std::span{short_mib});
  REQUIRE(!result.has_value());
  CHECK(result.error() == ParserError::PacketTooShort);
}

TEST_CASE("LteParser: kLogTable has all expected codes", "[lte]") {
  CHECK(LteParser::kLogTable.size() == 7);

  auto codes = LteParser().get_supported_codes();
  CHECK(codes.size() == 7);

  bool has_rrc = false, has_b0c2 = false, has_b17f = false, has_b180 = false;
  for (auto c : codes) {
    if (c == 0xB0C0) has_rrc = true;
    if (c == 0xB0C2) has_b0c2 = true;
    if (c == 0xB17F) has_b17f = true;
    if (c == 0xB180) has_b180 = true;
  }
  CHECK(has_rrc);
  CHECK(has_b0c2);
  CHECK(has_b17f);
  CHECK(has_b180);
}
