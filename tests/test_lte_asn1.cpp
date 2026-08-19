#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <span>
#include <vector>

#include <observer/model/Events.h>
#include <observer/model/ParserInterface.h>
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
  CHECK(LteParser::kLogTable.size() >= 9);

  auto codes = LteParser().get_supported_codes();
  CHECK(codes.size() == LteParser::kLogTable.size());

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

TEST_CASE("LteParser: NAS integrity peel extracts TAI from Attach Accept", "[lte][nas]") {
  LteParser parser;
  // DIAG subhdr 8B + integrity NAS: sec_hdr=1, seq, MAC(4), plain Attach Accept with TAI list.
  // Plain: PD|0, type=0x42, EPS attach result, spare, TAI list len=6, list elem.
  // PLMN 250-01: 0x52 0xF0 0x10, TAC 0x00AB
  std::vector<uint8_t> payload(8, 0);
  const uint8_t nas[] = {
      0x17,                                // PD=7, sec_hdr=1
      0x01,                                // seq
      0x11, 0x22, 0x33, 0x44,              // MAC
      0x07,                                // plain PD|sec0
      0x42,                                // Attach Accept
      0x01,                                // EPS attach result
      0x00,                                // spare / T3412 placeholder nibble layout simplified
      0x06,                                // TAI list length
      0x00,                                // list type/num
      0x52, 0xF0, 0x10,                    // PLMN 250-01
      0x00, 0xAB,                          // TAC
  };
  payload.insert(payload.end(), nas, nas + sizeof(nas));

  auto result = parser.parse_lte_nas(payload);
  REQUIRE(result.has_value());
  REQUIRE(!result->empty());
  auto* pe = std::get_if<Events::PassportEvent>(&result->at(0));
  REQUIRE(pe);
  CHECK(pe->passport.mcc == 250);
  CHECK(pe->passport.mnc == 1);
  CHECK(pe->passport.tac == 0x00AB);
}

TEST_CASE("LteParser: B0C0 v30 segment 1–6 buffers, 7 joins", "[lte][asn1][segments]") {
  LteParser parser;
  // Minimal v30 header (24B) + 1-byte ASN stub. segment_id @23.
  auto make_seg = [](uint8_t seg_id, std::initializer_list<uint8_t> asn) {
    std::vector<uint8_t> p(24 + asn.size(), 0);
    p[0] = 30;  // version
    p[6] = 72;  // PCI=72
    p[8] = 0x64;
    p[9] = 0x0A;  // EARFCN 2660
    p[14] = 3;    // BCCH-DL-SCH for v19-style map (ver 30 uses map_v19)
    const uint16_t plen = static_cast<uint16_t>(asn.size());
    p[19] = static_cast<uint8_t>(plen & 0xFF);
    p[20] = static_cast<uint8_t>((plen >> 8) & 0xFF);
    p[23] = seg_id;
    std::copy(asn.begin(), asn.end(), p.begin() + 24);
    return p;
  };

  auto s1 = make_seg(1, {0xAA});
  auto r1 = parser.parse_rrc_ota(s1);
  REQUIRE(r1.has_value());
  // Mid-segment: radio only (no ASN events expected).
  REQUIRE(!r1->empty());

  auto s7 = make_seg(7, {0xBB});
  auto r7 = parser.parse_rrc_ota(s7);
  REQUIRE(r7.has_value());
  // Joined payload still won't unpack as SIB1 — but must not crash / drop key.
  bool has_radio = false;
  for (const auto& ev : *r7) {
    if (std::get_if<Events::GenericRadioParamsEvent>(&ev)) has_radio = true;
  }
  CHECK(has_radio);
}
