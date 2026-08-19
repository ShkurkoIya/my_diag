#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "observer/AtParse.h"

using Catch::Approx;
using namespace Observer;

TEST_CASE("CEREG parse TAC/CID hex", "[at][cereg]") {
  auto id = parse_cereg_or_creg("+CEREG: 2,1,4D07,BF2E917,7\r\nOK\r\n");
  REQUIRE(id.ok);
  CHECK(id.tac == 0x4D07);
  CHECK(id.cell_id == 0xBF2E917);
}

TEST_CASE("CREG parse LAC/CID", "[at][cereg]") {
  auto id = parse_cereg_or_creg("+CREG: 2,1,4D07,BF2E917\r\nOK\r\n");
  REQUIRE(id.ok);
  CHECK(id.tac == 0x4D07);
  CHECK(id.cell_id == 0xBF2E917);
}

TEST_CASE("COPS numeric PLMN", "[at][cereg]") {
  auto plmn = parse_cops_numeric_plmn("+COPS: 0,2,\"25020\",7\r\nOK\r\n");
  REQUIRE(plmn);
  CHECK(plmn->first == 250);
  CHECK(plmn->second == 20);
}

TEST_CASE("COPS=? list extracts numeric PLMNs", "[at][cereg]") {
  const char* raw =
      "+COPS: (2,\"Tele2\",\"Tele2\",\"25020\",7),"
      "(1,\"MTS RUS\",\"MTS\",\"25001\",7),"
      "(1,\"YOTA\",\"YOTA\",\"25011\",7)\r\nOK\r\n";
  auto xs = parse_cops_plmn_list(raw);
  REQUIRE(xs.size() == 3);
  CHECK(xs[0] == std::make_pair<uint16_t, uint16_t>(250, 1));
  CHECK(xs[1] == std::make_pair<uint16_t, uint16_t>(250, 11));
  CHECK(xs[2] == std::make_pair<uint16_t, uint16_t>(250, 20));
}

TEST_CASE("CPSI LTE serving FULL fields", "[at][cereg][cpsi]") {
  auto s = parse_cpsi_lte(
      "+CPSI: LTE,Online,250-20,0x4D07,200468759,468,EUTRAN-BAND7,3400,3,3,-69,-900,-661,20\r\nOK\r\n");
  REQUIRE(s.ok);
  CHECK(s.mcc == 250);
  CHECK(s.mnc == 20);
  CHECK(s.tac == 0x4D07);
  CHECK(s.cell_id == 200468759);
  CHECK(s.pci == 468);
  CHECK(s.earfcn == 3400);
  CHECK(s.rsrp == Approx(-90.0f));
}

TEST_CASE("CPSI picks last LTE Online among URC mix", "[at][cereg][cpsi]") {
  auto s = parse_cpsi_lte(
      "+CPSI: WCDMA,Online,250-20,0x4D07,4487261,WCDMA IMT 2000,38,10563,0,4.0,69,32,46,500\r\n"
      "+CPSI: LTE,Online,250-20,0x4D07,200468759,468,EUTRAN-BAND7,3400,3,3,-69,-900,-661,20\r\n"
      "OK\r\n");
  REQUIRE(s.ok);
  CHECK(s.earfcn == 3400);
  CHECK(s.pci == 468);
}

TEST_CASE("CPSI rejects transitional LTE garbage EARFCN", "[at][cereg][cpsi]") {
  auto s = parse_cpsi_lte(
      "+CPSI: LTE,Online,250-20,0x4D07,200468789,0,EUTRAN-BAND0,4294967295,0,0,0,0,0,0\r\n"
      "+CPSI: WCDMA,Online,250-20,0x4D07,4487261,WCDMA IMT 2000,38,10563,0,3.0,67,34,48,500\r\n"
      "OK\r\n");
  REQUIRE_FALSE(s.ok);
}

TEST_CASE("CPSI WCDMA serving FULL fields", "[at][cereg][cpsi][wcdma]") {
  auto s = parse_cpsi_wcdma(
      "+CPSI: WCDMA,Online,250-20,0x4D07,4487261,WCDMA IMT 2000,38,10563,0,4.0,69,32,46,500\r\n"
      "OK\r\n");
  REQUIRE(s.ok);
  CHECK(s.mcc == 250);
  CHECK(s.mnc == 20);
  CHECK(s.lac == 0x4D07);
  CHECK(s.cell_id == 4487261);
  CHECK(s.psc == 38);
  CHECK(s.uarfcn == 10563);
  CHECK(s.ecio == Approx(-4.0f));
  CHECK(s.rscp == Approx(-47.0f));
}

TEST_CASE("CPSI WCDMA ignored when LTE Online present last", "[at][cereg][cpsi][wcdma]") {
  auto s = parse_cpsi_wcdma(
      "+CPSI: WCDMA,Online,250-20,0x4D07,4487261,WCDMA IMT 2000,38,10563,0,4.0,69,32,46,500\r\n"
      "+CPSI: LTE,Online,250-20,0x4D07,200468759,468,EUTRAN-BAND7,3400,3,3,-69,-900,-661,20\r\n"
      "OK\r\n");
  // Parser still returns last WCDMA Online — LTE is a separate parse.
  REQUIRE(s.ok);
  CHECK(s.uarfcn == 10563);
}

TEST_CASE("CMGRMI skip when CPSI is NO SERVICE", "[at][cpsi][cmgrmi]") {
  CHECK_FALSE(cpsi_cmgrmi_ready("+CPSI: NO SERVICE,Online\r\nOK\r\n"));
  CHECK_FALSE(cpsi_cmgrmi_ready(
      "+CPSI: WCDMA,Online,250-20,0x4D07,4487261,WCDMA IMT 2000,38,10563,0,4.0,69,32,46,500\r\nOK\r\n"));
  CHECK(cpsi_cmgrmi_ready(""));
  CHECK(cpsi_cmgrmi_ready(
      "+CPSI: LTE,Online,250-20,0x4D07,200468759,468,EUTRAN-BAND7,3400,3,3,-69,-900,-661,20\r\nOK\r\n"));
  // Transitional LTE Online (no valid CID yet) — still worth one CMGRMI try.
  CHECK(cpsi_cmgrmi_ready(
      "+CPSI: LTE,Online,250-20,0x4D07,200468789,0,EUTRAN-BAND0,4294967295,0,0,0,0,0,0\r\nOK\r\n"));
}
