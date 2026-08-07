#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Mirror of live_scanner CEREG/COPS helpers (keep in sync if formats change).

namespace {

struct CeregIdentity {
  uint32_t tac{0};
  uint32_t cell_id{0};
  bool ok{false};
};

CeregIdentity parse_cereg_or_creg(std::string_view resp) {
  CeregIdentity out;
  auto pos = resp.find("+CEREG:");
  size_t prefix = 7;
  if (pos == std::string_view::npos) {
    pos = resp.find("+CREG:");
    prefix = 6;
  }
  if (pos == std::string_view::npos) return out;
  std::string line(resp.substr(pos + prefix));
  if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);

  std::vector<std::string> toks;
  {
    std::string cur;
    for (char c : line) {
      if (c == ',') {
        toks.push_back(cur);
        cur.clear();
      } else if (c != ' ' && c != '"') {
        cur.push_back(c);
      }
    }
    toks.push_back(cur);
  }
  if (toks.size() < 4) return out;
  try {
    out.tac = static_cast<uint32_t>(std::stoul(toks[2], nullptr, 16));
    out.cell_id = static_cast<uint32_t>(std::stoul(toks[3], nullptr, 16));
    out.ok = (out.tac != 0 && out.cell_id != 0);
  } catch (...) {
    out.ok = false;
  }
  return out;
}

std::optional<std::pair<uint16_t, uint16_t>> parse_cops_numeric_plmn(std::string_view resp) {
  auto pos = resp.find("+COPS:");
  if (pos == std::string_view::npos) return std::nullopt;
  auto q1 = resp.find('"', pos);
  if (q1 == std::string_view::npos) return std::nullopt;
  auto q2 = resp.find('"', q1 + 1);
  if (q2 == std::string_view::npos || q2 <= q1 + 1) return std::nullopt;
  std::string plmn(resp.substr(q1 + 1, q2 - q1 - 1));
  if (plmn.size() < 5 || plmn.size() > 6) return std::nullopt;
  try {
    const uint16_t mcc = static_cast<uint16_t>(std::stoul(plmn.substr(0, 3)));
    const uint16_t mnc = static_cast<uint16_t>(std::stoul(plmn.substr(3)));
    if (mcc < 100 || mcc > 999) return std::nullopt;
    return std::make_pair(mcc, mnc);
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace

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

// Mirrors live_scanner parse_cops_plmn_list.
std::vector<std::pair<uint16_t, uint16_t>> parse_cops_plmn_list(std::string_view resp) {
  std::set<std::pair<uint16_t, uint16_t>> uniq;
  for (std::size_t i = 0; i < resp.size(); ++i) {
    if (resp[i] != '"') continue;
    const auto j = resp.find('"', i + 1);
    if (j == std::string_view::npos) break;
    std::string tok(resp.substr(i + 1, j - i - 1));
    tok.erase(std::remove(tok.begin(), tok.end(), '-'), tok.end());
    i = j;
    if (tok.size() < 5 || tok.size() > 6) continue;
    bool digits = true;
    for (char c : tok) {
      if (c < '0' || c > '9') {
        digits = false;
        break;
      }
    }
    if (!digits) continue;
    try {
      const uint16_t mcc = static_cast<uint16_t>(std::stoul(tok.substr(0, 3)));
      const uint16_t mnc = static_cast<uint16_t>(std::stoul(tok.substr(3)));
      if (mcc >= 100 && mcc <= 999) uniq.insert({mcc, mnc});
    } catch (...) {
    }
  }
  return {uniq.begin(), uniq.end()};
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

// Mirrors live_scanner parse_cpsi_lte (keep in sync).
struct CpsiServing {
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint32_t tac{0};
  uint32_t cell_id{0};
  uint16_t pci{0};
  uint32_t earfcn{0};
  float rsrp{-999.0f};
  bool ok{false};
};

CpsiServing parse_cpsi_lte(std::string_view resp) {
  CpsiServing best;
  for (std::size_t pos = 0; pos < resp.size();) {
    const auto at = resp.find("+CPSI:", pos);
    if (at == std::string_view::npos) break;
    pos = at + 6;
    std::string line(resp.substr(pos));
    if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);
    std::vector<std::string> toks;
    std::string cur;
    for (char c : line) {
      if (c == ',') {
        toks.push_back(cur);
        cur.clear();
      } else if (c != ' ' && c != '"') {
        cur.push_back(c);
      }
    }
    toks.push_back(cur);
    if (toks.size() < 8 || toks[0] != "LTE") continue;
    if (toks[1].find("Online") == std::string::npos &&
        toks[1].find("Limited") == std::string::npos)
      continue;
    CpsiServing out;
    try {
      auto dash = toks[2].find('-');
      if (dash == std::string::npos) continue;
      out.mcc = static_cast<uint16_t>(std::stoul(toks[2].substr(0, dash)));
      out.mnc = static_cast<uint16_t>(std::stoul(toks[2].substr(dash + 1)));
      std::string tac_s = toks[3];
      if (tac_s.size() > 2 && tac_s[0] == '0' && (tac_s[1] == 'x' || tac_s[1] == 'X'))
        tac_s = tac_s.substr(2);
      out.tac = static_cast<uint32_t>(std::stoul(tac_s, nullptr, 16));
      out.cell_id = static_cast<uint32_t>(std::stoul(toks[4], nullptr, 10));
      out.pci = static_cast<uint16_t>(std::stoul(toks[5], nullptr, 10));
      out.earfcn = static_cast<uint32_t>(std::stoul(toks[7], nullptr, 10));
      if (toks.size() > 11) out.rsrp = static_cast<float>(std::stoi(toks[11])) / 10.0f;
    } catch (...) {
      continue;
    }
    out.ok = out.earfcn > 0 && out.earfcn <= 262143 && out.pci >= 1 && out.pci <= 503 &&
             out.cell_id && out.tac && out.mcc >= 100;
    if (out.ok) best = out;
  }
  return best;
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
  CHECK(s.rsrp == Catch::Approx(-90.0f));
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
