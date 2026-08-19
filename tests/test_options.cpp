#include <catch2/catch_test_macros.hpp>

#include "observer/Options.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace Observer;

namespace {

struct MuteStdio {
  std::streambuf* out{std::cout.rdbuf()};
  std::streambuf* err{std::cerr.rdbuf()};
  std::ostringstream sink;
  MuteStdio() {
    std::cout.rdbuf(sink.rdbuf());
    std::cerr.rdbuf(sink.rdbuf());
  }
  ~MuteStdio() {
    std::cout.rdbuf(out);
    std::cerr.rdbuf(err);
  }
};

int parse(std::vector<std::string> args, Options& out) {
  MuteStdio mute;
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("live_scanner"));
  for (auto& a : args) argv.push_back(a.data());
  return parse_options(static_cast<int>(argv.size()), argv.data(), out);
}

}  // namespace

TEST_CASE("default is listen 30s", "[cli]") {
  Options o;
  REQUIRE(parse({}, o) == 0);
  CHECK(o.job == Job::Listen);
  CHECK(o.rats == Rats::Auto);
  CHECK(o.io.duration_sec == 30);
  CHECK_FALSE(o.recipe.ghost);
  CHECK_FALSE(o.surveying());
}

TEST_CASE("survey-mode lte matches GUI Start LTE", "[cli]") {
  Options o;
  REQUIRE(parse({"--survey-mode", "lte", "--search-period", "90", "--hop-dwell", "40",
                 "--hop-band-clip", "--live-json", "/tmp/qcom_live_towers.json", "--scanner-log",
                 "/tmp/qcom_live_scanner.log"},
                o) == 0);
  CHECK(o.job == Job::Survey);
  CHECK(o.rats == Rats::Lte);
  CHECK(o.io.duration_sec == 0);
  CHECK(o.recipe.ghost);
  CHECK(o.recipe.foreign_plmn);
  CHECK_FALSE(o.recipe.irat_wcdma);
  CHECK(o.recipe.pin_lte);
  CHECK(o.recipe.wipe_fplmn);
  CHECK_FALSE(o.recipe.cfun_recover);
  CHECK(o.recipe.band_clip);
  CHECK(o.recipe.enrich_serving);
  CHECK(o.recipe.scan_between_hops);
  CHECK(o.recipe.search_period_sec == 90);
  CHECK(o.recipe.hop_dwell_sec == 40);
  CHECK(o.recipe.cops_act == 7);
  CHECK(o.recipe.ghost_plmn.first == 999);
  CHECK(o.recipe.ghost_plmn.second == 99);
  CHECK(o.recipe.use_qmi);
  CHECK(o.io.live_json_path == "/tmp/qcom_live_towers.json");
  CHECK(o.io.scanner_log_path == "/tmp/qcom_live_scanner.log");
  CHECK(o.hop_lock());
}

TEST_CASE("--recover-cfun is opt-in airplane bounce", "[cli]") {
  Options o;
  REQUIRE(parse({"--survey-mode", "lte", "--recover-cfun"}, o) == 0);
  CHECK(o.recipe.cfun_recover);
}

TEST_CASE("ghost linger is 8s on select ERROR, full dwell on OK", "[cli]") {
  CHECK(ghost_linger_sec(true, 45) == 45);
  CHECK(ghost_linger_sec(false, 45) == 8);
  CHECK(ghost_linger_sec(false, 5) == 5);
}

TEST_CASE("survey-mode irat matches GUI Start IRAT", "[cli]") {
  Options o;
  REQUIRE(parse({"--survey-mode", "irat", "--search-period", "90", "--hop-dwell", "40",
                 "--hop-band-clip"},
                o) == 0);
  CHECK(o.job == Job::Survey);
  CHECK(o.rats == Rats::Irat);
  CHECK(o.recipe.ghost);
  CHECK(o.recipe.irat_wcdma);
  CHECK(o.recipe.pin_lte);
  CHECK(o.recipe.foreign_plmn);
  CHECK_FALSE(o.recipe.cfun_recover);
  CHECK(o.recipe.band_clip);
}

TEST_CASE("survey-mode wcdma matches GUI Start WCDMA", "[cli]") {
  Options o;
  REQUIRE(parse({"--survey-mode", "wcdma", "--hop-dwell", "40", "--wcdma-dwell", "40",
                 "--hop-band-clip"},
                o) == 0);
  CHECK(o.job == Job::Survey);
  CHECK(o.rats == Rats::Wcdma);
  CHECK(o.wcdma_only());
  CHECK_FALSE(o.recipe.ghost);
  CHECK_FALSE(o.recipe.pin_lte);
  CHECK(o.recipe.irat_wcdma);
  CHECK(o.recipe.foreign_plmn);
  CHECK(o.recipe.hop_max == 32);
  CHECK(o.recipe.cops_act == 2);
  CHECK(o.recipe.wcdma_dwell_sec == 40);
  CHECK_FALSE(o.recipe.cfun_recover);
}

TEST_CASE("--rats is an alias of --survey-mode", "[cli]") {
  Options o;
  REQUIRE(parse({"--rats", "lte"}, o) == 0);
  CHECK(o.job == Job::Survey);
  CHECK(o.rats == Rats::Lte);
}

TEST_CASE("--no-ghost turns off product ghost default", "[cli]") {
  Options o;
  REQUIRE(parse({"--survey-mode", "lte", "--no-ghost"}, o) == 0);
  CHECK_FALSE(o.recipe.ghost);
}

TEST_CASE("legacy --earfcn-hop is survey/irat without ghost", "[cli]") {
  Options o;
  REQUIRE(parse({"--earfcn-hop"}, o) == 0);
  CHECK(o.job == Job::Survey);
  CHECK(o.rats == Rats::Irat);
  CHECK_FALSE(o.recipe.ghost);
  CHECK(o.recipe.irat_wcdma);
  CHECK(o.recipe.foreign_plmn);
}

TEST_CASE("--cops-ghost-plmn without hop is Search", "[cli]") {
  Options o;
  REQUIRE(parse({"--cops-ghost-plmn"}, o) == 0);
  CHECK(o.job == Job::Search);
  CHECK(o.rats == Rats::Lte);
  CHECK(o.recipe.ghost);
  CHECK(o.recipe.ghost_plmn.first == 999);
  CHECK(o.recipe.ghost_plmn.second == 99);
  CHECK(o.io.duration_sec == 0);

  Options p;
  REQUIRE(parse({"--cops-ghost-plmn", "25001"}, p) == 0);
  CHECK(p.job == Job::Search);
  CHECK(p.recipe.ghost_plmn.first == 250);
  CHECK(p.recipe.ghost_plmn.second == 1);
}

TEST_CASE("--no-qmi is opt-out", "[cli]") {
  Options o;
  REQUIRE(parse({"--survey-mode", "lte", "--no-qmi"}, o) == 0);
  CHECK_FALSE(o.recipe.use_qmi);
}

TEST_CASE("invalid --survey-mode is an error", "[cli]") {
  Options o;
  CHECK(parse({"--survey-mode", "cdma"}, o) == 2);
}

TEST_CASE("--help and --list do not start a survey", "[cli]") {
  Options h;
  REQUIRE(parse({"--help"}, h) == 0);
  CHECK(h.help_only);

  Options l;
  REQUIRE(parse({"--list"}, l) == 0);
  CHECK(l.list_only);
  CHECK_FALSE(l.surveying());
}
