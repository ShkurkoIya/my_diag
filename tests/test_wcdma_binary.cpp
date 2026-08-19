#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <variant>
#include <vector>

#include <observer/model/Events.h>
#include <observer/model/Types.h>
#include "wcdma/WcdmaParser.h"

using namespace QCom;
using namespace QCom::Wcdma;

namespace {

std::vector<uint8_t> unhex(const char* hs) {
  std::vector<uint8_t> out;
  for (const char* p = hs; p[0] && p[1]; p += 2) {
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int a = nib(p[0]), b = nib(p[1]);
    if (a < 0 || b < 0) break;
    out.push_back(static_cast<uint8_t>((a << 4) | b));
  }
  return out;
}

template <typename Fn>
void for_each_event(const std::vector<Events::RrcEvent>& evs, Fn&& fn) {
  for (const auto& e : evs) std::visit(fn, e);
}

}  // namespace

TEST_CASE("0x4027 Cell ID — RAC/URA/PSC>>4", "[wcdma][binary]") {
  auto payload = unhex("f1250000a729000041852d0800000700d01802060200030f9d9c000001000000");
  WcdmaParser p;
  QualcommPacketView pkt{.log_code = WcdmaParser::WCDMA_CELL_ID, .payload = payload};
  auto evs = p.parse(pkt);
  REQUIRE(evs);
  REQUIRE_FALSE(evs->empty());

  bool got_pass = false, got_radio = false, got_srv = false;
  for_each_event(*evs, [&](const auto& v) {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_same_v<T, Events::PassportEvent>) {
      CHECK(v.passport.mcc == 262);
      CHECK(v.passport.mnc == 3);
      CHECK(v.passport.tac == 0x9c9d);
      CHECK(v.passport.rac == 1);
      CHECK(v.passport.cell_id == 0x082d8541u);
      got_pass = true;
    } else if constexpr (std::is_same_v<T, Events::GenericRadioParamsEvent>) {
      if (auto* wr = std::get_if<Events::RadioParamsEvent<WcdmaRadioParams>>(&v)) {
        CHECK(wr->data.dl_uarfcn == 10663);
        CHECK(wr->data.ul_uarfcn == 9713);
        CHECK(wr->data.psc == 397);
        CHECK(wr->data.flags == 0x07);
        got_radio = true;
      }
    } else if constexpr (std::is_same_v<T, Events::ServingChangedEvent>) {
      got_srv = v.is_serving;
    }
  });
  CHECK(got_pass);
  CHECK(got_radio);
  CHECK(got_srv);
}

TEST_CASE("0x4005 reselection — serving RADIO + neigh", "[wcdma][binary]") {
  auto payload = unhex("82000000000000f1293200b6a5fff1f5ff000000000000f1293100b39effdedeff040000008000");
  WcdmaParser p;
  QualcommPacketView pkt{.log_code = WcdmaParser::WCDMA_RESEL_RANK, .payload = payload};
  auto evs = p.parse(pkt);
  REQUIRE(evs);

  bool got_radio = false, got_sig = false, got_neigh = false;
  for_each_event(*evs, [&](const auto& v) {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_same_v<T, Events::GenericRadioParamsEvent>) {
      if (auto* wr = std::get_if<Events::RadioParamsEvent<WcdmaRadioParams>>(&v)) {
        CHECK(wr->data.dl_uarfcn == 10737);
        CHECK(wr->data.psc == 50);
        got_radio = true;
      }
    } else if constexpr (std::is_same_v<T, Events::SignalUpdateEvent>) {
      got_sig = true;
    } else if constexpr (std::is_same_v<T, Events::WcdmaNeighborsEvent>) {
      REQUIRE(v.neighbors.size() >= 1);
      CHECK(v.neighbors[0].uarfcn == 10737);
      CHECK(v.neighbors[0].psc == 49);
      got_neigh = true;
    }
  });
  CHECK(got_radio);
  CHECK(got_sig);
  CHECK(got_neigh);
}

TEST_CASE("0x412F RRC OTA header — channel/len", "[wcdma][binary]") {
  auto payload = unhex("84281f00a7298d01a143f686e52a22282f36928cc1852026d2519830afacda4a330614909b4944");
  WcdmaParser p;
  QualcommPacketView pkt{.log_code = WcdmaParser::WCDMA_RRC_OTA, .payload = payload};
  auto evs = p.parse(pkt);
  REQUIRE(evs);
  REQUIRE_FALSE(evs->empty());
  bool ok = false;
  for_each_event(*evs, [&](const auto& v) {
    using T = std::decay_t<decltype(v)>;
    if constexpr (std::is_same_v<T, Events::GenericRadioParamsEvent>) {
      if (auto* wr = std::get_if<Events::RadioParamsEvent<WcdmaRadioParams>>(&v)) {
        CHECK(wr->data.last_rrc_channel == 0x84);
        CHECK(wr->data.last_rrc_len == 0x001f);
        ok = true;
      }
    }
  });
  CHECK(ok);
}
