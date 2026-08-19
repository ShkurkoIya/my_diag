#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <cstdio>
#include <span>
#include <vector>

#include "core/BinaryCursor.h"
#include <observer/model/CellIdentity.h>
#include <observer/model/Events.h>
#include <qcom/parser/QualcomParser.h>
#include "core/RevWordBits.h"
#include "lte/LteParser.h"
#include "lte/LteQcomLayouts.h"

using namespace QCom;
using namespace QCom::Lte;
using Catch::Matchers::WithinAbs;

namespace {

template <typename EventType>
const EventType* find_event(const std::vector<Events::RrcEvent>& events) {
  for (const auto& ev : events) {
    if (auto* ptr = std::get_if<EventType>(&ev)) return ptr;
  }
  return nullptr;
}

}  // namespace

TEST_CASE("BinaryCursor + B0c2/B193 wire SSOT", "[lte][binary][cursor]") {
  using QCom::Utils::BinaryCursor;
  using QCom::Utils::RevWordBits;

  const uint8_t raw[] = {0x12, 0x34, 0x78, 0x56, 0x34, 0x12};
  const BinaryCursor c{std::span{raw}};
  CHECK(c.le16(0) == 0x3412);
  CHECK(c.le32(2) == 0x12345678);
  CHECK(c.at(2).le16(0) == 0x5678);
  CHECK(c.slice(10, 2).empty());
  CHECK(c.le16_bits(0, 0, 8) == 0x12);

  const auto lay48 = QCom::Lte::Wire::B193::sp19_rev_layout(48);
  REQUIRE(lay48);
  CHECK(lay48->cell_stride == 140);
  CHECK(lay48->snr_off == 92);
  CHECK(lay48->le_meas);
  const auto lay36 = QCom::Lte::Wire::B193::sp19_rev_layout(36);
  REQUIRE(lay36);
  CHECK_FALSE(lay36->le_meas);
  CHECK_FALSE(QCom::Lte::Wire::B193::sp19_rev_layout(99).has_value());

  CHECK(QCom::Lte::Wire::B0c2::bw_raw_to_mhz(75) == 15);
  CHECK(QCom::Lte::Wire::B0c2::bw_raw_to_mhz(25) == 5);
  CHECK(QCom::Lte::Wire::B0c2::bw_raw_to_mhz(15) == 3);
  CHECK(QCom::Lte::Wire::B0c2::allowed_access_bool(0) == 1);
  CHECK(QCom::Lte::Wire::B0c2::allowed_access_bool(1) == 0);

  // RevWordBits identity: one LE word 0x00000100 → reversed BE bitstream slice.
  const uint8_t words[] = {0x00, 0x01, 0x00, 0x00};
  const RevWordBits rb(words, 1);
  CHECK(rb.slice(0, 8) == 0x00);
  CHECK(rb.nbytes == 4);
}

TEST_CASE("0xB0C2 v2 — serving cell identity", "[lte][binary][b0c2]") {
  LteParser parser;

  // Version 2: PCI=72, EARFCN=2660, UL_EARFCN=20660, DL_BW=15 MHz, UL_BW=10 MHz
  // CID=0x01234567, TAC=0x00AB, Band=7, MCC=250, MNC_digits=2, MNC=01, Full
  // BW on wire is NRB (same as live B0C2 v3); 15 alone is ambiguous (NRB→3 MHz).
  uint8_t payload[] = {
      0x02,                    // version = 2
      0x48, 0x00,              // PCI = 72
      0x64, 0x0A,              // DL EARFCN = 2660
      0xB4, 0x50,              // UL EARFCN = 20660
      75, 50,                  // NRB → 15 / 10 MHz
      0x67, 0x45, 0x23, 0x01,  // Cell ID = 0x01234567
      0xAB, 0x00,              // TAC = 0x00AB
      0x07, 0x00, 0x00, 0x00,  // Band = 7
      0xFA, 0x00,              // MCC = 250
      0x02,                    // MNC digits = 2
      0x01, 0x00,              // MNC = 01
      0x00,                    // Allowed Access wire = Full
  };

  auto wire = Wire::B0c2::decode(Utils::BinaryCursor{std::span{payload}});
  REQUIRE(wire);
  CHECK(wire->pci == 72);
  CHECK(wire->earfcn == 2660);
  CHECK(wire->ul_earfcn == 20660);
  CHECK(Wire::B0c2::bw_raw_to_mhz(wire->dl_bw_raw) == 15);
  CHECK(Wire::B0c2::bw_raw_to_mhz(wire->ul_bw_raw) == 10);
  CHECK(Wire::B0c2::allowed_access_bool(wire->allowed_raw) == 1);

  auto result = parser.parse_serv_cell_info(std::span{payload});
  REQUIRE(result.has_value());
  auto& events = result.value();
  REQUIRE(events.size() >= 2);

  auto* gen = find_event<Events::GenericRadioParamsEvent>(events);
  REQUIRE(gen);
  auto* radio = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(radio);
  CHECK(radio->data.pci == 72);
  CHECK(radio->data.earfcn == 2660);
  CHECK(radio->data.ul_earfcn == 20660);
  CHECK(radio->data.dl_bw == 15);
  CHECK(radio->data.ul_bw == 10);
  CHECK(radio->data.freq_band_ind == 7);
  CHECK(radio->data.allowed_access == 1);

  auto* passport_ev = find_event<Events::PassportEvent>(events);
  REQUIRE(passport_ev);
  CHECK(passport_ev->passport.cell_id == 0x01234567);
  CHECK(passport_ev->passport.tac == 0xAB);
  CHECK(passport_ev->passport.mcc == 250);
  CHECK(passport_ev->passport.mnc == 1);
  CHECK(passport_ev->passport.mnc_digits == 2);
  CHECK_FALSE(passport_ev->passport.plmn_soft);
}

TEST_CASE("0xB0C2 v2 — Limited access maps to allowed_access=0", "[lte][binary][b0c2]") {
  uint8_t payload[] = {
      0x02, 0x48, 0x00, 0x64, 0x0A, 0xB4, 0x50, 75, 50, 0x67, 0x45, 0x23, 0x01,
      0xAB, 0x00, 0x07, 0x00, 0x00, 0x00, 0xFA, 0x00, 0x02, 0x01, 0x00,
      0x01,  // Limited
  };
  CHECK(Wire::B0c2::allowed_access_bool(1) == 0);
  LteParser parser;
  auto result = parser.parse_serv_cell_info(std::span{payload});
  REQUIRE(result.has_value());
  auto* gen = find_event<Events::GenericRadioParamsEvent>(*result);
  REQUIRE(gen);
  auto* radio = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(radio);
  CHECK(radio->data.allowed_access == 0);
}

TEST_CASE("0xB0C2 v3 — synthetic NRB BW + identity", "[lte][binary][b0c2]") {
  // v3: EARFCN u32; BW as NRB 75 → 15 MHz.
  uint8_t payload[] = {
      0x03,                          // version
      0x9C, 0x00,                    // PCI = 156
      0x77, 0x01, 0x00, 0x00,        // DL = 375
      0xC7, 0x47, 0x00, 0x00,        // UL = 18375
      75, 75,                        // NRB → 15 MHz
      0x19, 0xDF, 0x16, 0x03,        // CID = 51830553
      0x8E, 0x45,                    // TAC = 17806
      0x01, 0x00, 0x00, 0x00,        // Band = 1
      0xFA, 0x00,                    // MCC = 250
      0x02,                          // MNC digits
      0x01, 0x00,                    // MNC = 1
      0x00,                          // Full
  };
  LteParser parser;
  auto result = parser.parse_serv_cell_info(std::span{payload});
  REQUIRE(result.has_value());
  auto* gen = find_event<Events::GenericRadioParamsEvent>(*result);
  REQUIRE(gen);
  auto* radio = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(radio);
  CHECK(radio->data.pci == 156);
  CHECK(radio->data.earfcn == 375);
  CHECK(radio->data.ul_earfcn == 18375);
  CHECK(radio->data.dl_bw == 15);
  CHECK(radio->data.ul_bw == 15);
  CHECK(radio->data.freq_band_ind == 1);
  CHECK(radio->data.allowed_access == 1);
  auto* pass = find_event<Events::PassportEvent>(*result);
  REQUIRE(pass);
  CHECK(pass->passport.cell_id == 51830553u);
  CHECK(pass->passport.tac == 17806);
  CHECK(pass->passport.mcc == 250);
  CHECK(pass->passport.mnc == 1);
  CHECK(pass->passport.mnc_digits == 2);
}

TEST_CASE("0xB0C2 v3 — real QXDM B1 EARFCN375 PCI156", "[lte][binary][b0c2][real]") {
  // QXDM Item View 0xB0C2 v3 (wrapper+ts stripped). Camp after COPS reattach.
  // PCI=156 DL=375 UL=18375 BW=15/15 CID=51830553 TAC=17806 Band=1 PLMN=250-01 Full
  const uint8_t payload[] = {0x03, 0x9C, 0x00, 0x77, 0x01, 0x00, 0x00, 0xC7, 0x47, 0x00,
                             0x00, 0x4B, 0x4B, 0x19, 0xDF, 0x16, 0x03, 0x8E, 0x45, 0x01,
                             0x00, 0x00, 0x00, 0xFA, 0x00, 0x02, 0x01, 0x00, 0x00};

  auto wire = Wire::B0c2::decode(Utils::BinaryCursor{std::span{payload}});
  REQUIRE(wire);
  CHECK(wire->pci == 156);
  CHECK(wire->earfcn == 375);
  CHECK(wire->ul_earfcn == 18375);
  CHECK(wire->dl_bw_raw == 75);
  CHECK(wire->ul_bw_raw == 75);
  CHECK(wire->cell_id == 51830553u);
  CHECK(wire->tac == 17806);
  CHECK(wire->band == 1);
  CHECK(wire->mcc == 250);
  CHECK(wire->mnc_digit == 2);
  CHECK(wire->mnc == 1);
  CHECK(wire->allowed_raw == 0);

  LteParser parser;
  auto result = parser.parse_serv_cell_info(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 2);

  auto* gen = find_event<Events::GenericRadioParamsEvent>(*result);
  REQUIRE(gen);
  auto* radio = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(radio);
  CHECK(radio->data.pci == 156);
  CHECK(radio->data.earfcn == 375);
  CHECK(radio->data.ul_earfcn == 18375);
  CHECK(radio->data.dl_bw == 15);
  CHECK(radio->data.ul_bw == 15);
  CHECK(radio->data.freq_band_ind == 1);
  CHECK(radio->data.allowed_access == 1);

  auto* pass = find_event<Events::PassportEvent>(*result);
  REQUIRE(pass);
  CHECK(pass->passport.cell_id == 51830553u);
  CHECK(pass->passport.tac == 17806);
  CHECK(pass->passport.mcc == 250);
  CHECK(pass->passport.mnc == 1);
  CHECK(pass->passport.mnc_digits == 2);
  CHECK_FALSE(pass->passport.plmn_soft);
}

TEST_CASE("0xB0C2 v3 — real QXDM B20 EARFCN6275 PCI278", "[lte][binary][b0c2][real]") {
  // Same eNB 202463, sector 8, Band 20 5 MHz. QXDM: CID=51830536 TAC=17806 250-01 Full
  const uint8_t payload[] = {0x03, 0x16, 0x01, 0x83, 0x18, 0x00, 0x00, 0xD3, 0x5E, 0x00,
                             0x00, 0x19, 0x19, 0x08, 0xDF, 0x16, 0x03, 0x8E, 0x45, 0x14,
                             0x00, 0x00, 0x00, 0xFA, 0x00, 0x02, 0x01, 0x00, 0x00};

  auto wire = Wire::B0c2::decode(Utils::BinaryCursor{std::span{payload}});
  REQUIRE(wire);
  CHECK(wire->pci == 278);
  CHECK(wire->earfcn == 6275);
  CHECK(wire->ul_earfcn == 24275);
  CHECK(wire->dl_bw_raw == 25);
  CHECK(wire->ul_bw_raw == 25);
  CHECK(wire->cell_id == 51830536u);
  CHECK(wire->tac == 17806);
  CHECK(wire->band == 20);
  CHECK(wire->mcc == 250);
  CHECK(wire->mnc == 1);
  CHECK(wire->allowed_raw == 0);
  CHECK((wire->cell_id >> 8) == 202463u);  // same eNB as B1 sibling

  LteParser parser;
  auto result = parser.parse_serv_cell_info(std::span{payload});
  REQUIRE(result.has_value());
  auto* gen = find_event<Events::GenericRadioParamsEvent>(*result);
  REQUIRE(gen);
  auto* radio = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(radio);
  CHECK(radio->data.pci == 278);
  CHECK(radio->data.earfcn == 6275);
  CHECK(radio->data.ul_earfcn == 24275);
  CHECK(radio->data.dl_bw == 5);
  CHECK(radio->data.ul_bw == 5);
  CHECK(radio->data.freq_band_ind == 20);
  CHECK(radio->data.allowed_access == 1);

  auto* pass = find_event<Events::PassportEvent>(*result);
  REQUIRE(pass);
  CHECK(pass->passport.cell_id == 51830536u);
  CHECK(pass->passport.tac == 17806);
  CHECK(pass->passport.mcc == 250);
  CHECK(pass->passport.mnc == 1);
  CHECK(pass->passport.mnc_digits == 2);
}

TEST_CASE("0xB0C2 — RF key without identity still emits RADIO", "[lte][binary][b0c2]") {
  LteParser parser;
  // Same v2 layout as identity test, but CID/TAC/MCC are padding (all-ones / 0).
  uint8_t payload[] = {
      0x02,                    // version = 2
      0x48, 0x00,              // PCI = 72
      0x64, 0x0A,              // DL EARFCN = 2660
      0xB4, 0x50,              // UL EARFCN
      75, 50,                  // NRB BW
      0xFF, 0xFF, 0xFF, 0xFF,  // Cell ID invalid
      0xFF, 0xFF,              // TAC invalid
      0x07, 0x00, 0x00, 0x00,  // Band
      0x00, 0x00,              // MCC = 0
      0x02,                    // MNC digits
      0x01, 0x00,              // MNC
      0x00,
  };
  auto result = parser.parse_serv_cell_info(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 2660);
  CHECK(lte->data.pci == 72);
  CHECK(lte->data.dl_bw == 15);
  CHECK(find_event<Events::PassportEvent>(*result) == nullptr);
}

TEST_CASE("0xB0C2 — unknown version returns empty", "[lte][binary][b0c2]") {
  LteParser parser;
  uint8_t payload[] = {0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  auto sv = std::span{payload};
  auto result = parser.parse_serv_cell_info(sv);
  REQUIRE(result.has_value());
  CHECK(result.value().empty());
}

TEST_CASE("0xB0C2 — too short returns error", "[lte][binary][b0c2]") {
  LteParser parser;
  const uint8_t x[] = {0x00};
  auto result = parser.parse_serv_cell_info(std::span{x});
  REQUIRE(!result.has_value());
  CHECK(result.error() == ParserError::PacketTooShort);
}

TEST_CASE("0xB0EE v2 — real QXDM REGISTERED + GUTI", "[lte][binary][b0ee][real]") {
  // QXDM Item View stripped. EMM_REGISTERED / NORMAL_SERVICE, PLMN 250-01,
  // MME group {54,129}=0x3681, code 224, M-TMSI 0xD8808813 (wire only).
  const uint8_t payload[] = {0x02, 0x03, 0x00, 0x00, 0x52, 0xF0, 0x10, 0x01, 0x06, 0x52,
                             0xF0, 0x10, 0x36, 0x81, 0xE0, 0x13, 0x88, 0x80, 0xD8};

  auto wire = Wire::B0ee::decode(Utils::BinaryCursor{std::span{payload}});
  REQUIRE(wire);
  CHECK(wire->emm_state == static_cast<uint8_t>(Wire::B0ee::EmmState::Registered));
  CHECK(wire->emm_substate == 0);
  CHECK(wire->plmn.mcc == 250);
  CHECK(wire->plmn.mnc == 1);
  CHECK(wire->plmn.mnc_digits == 2);
  CHECK(wire->guti_valid);
  CHECK(wire->guti_ue_id == 6);
  CHECK(wire->guti_plmn.mcc == 250);
  CHECK(wire->mme_group_id == 0x3681);
  CHECK(wire->mme_code == 224);
  CHECK(wire->m_tmsi == 0xD8808813u);
  CHECK(std::string(Wire::B0ee::emm_state_name(wire->emm_state)) == "REGISTERED");

  LteParser parser;
  auto result = parser.parse_emm_state(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* radio = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(radio);
  CHECK(radio->data.emm_state == 3);
  CHECK(radio->data.emm_substate == 0);
  CHECK(radio->data.emm_mcc == 250);
  CHECK(radio->data.emm_mnc == 1);
  CHECK(radio->data.emm_mnc_digits == 2);
  CHECK(radio->data.mme_present);
  CHECK(radio->data.mme_group_id == 0x3681);
  CHECK(radio->data.mme_code == 224);
  // No passport / no cell key fields from NAS state.
  CHECK(radio->data.earfcn == 0);
  CHECK(find_event<Events::PassportEvent>(*result) == nullptr);
}

TEST_CASE("0xB0EE v2 — real QXDM DEREGISTERED_INITIATED", "[lte][binary][b0ee][real]") {
  // Same GUTI/PLMN as REGISTERED sample; only emm_state byte differs (3→6).
  const uint8_t payload[] = {0x02, 0x06, 0x00, 0x00, 0x52, 0xF0, 0x10, 0x01, 0x06, 0x52,
                             0xF0, 0x10, 0x36, 0x81, 0xE0, 0x13, 0x88, 0x80, 0xD8};
  auto wire = Wire::B0ee::decode(Utils::BinaryCursor{std::span{payload}});
  REQUIRE(wire);
  CHECK(wire->emm_state ==
        static_cast<uint8_t>(Wire::B0ee::EmmState::DeregisteredInitiated));
  CHECK(wire->m_tmsi == 0xD8808813u);

  LteParser parser;
  auto result = parser.parse_emm_state(std::span{payload});
  REQUIRE(result.has_value());
  auto* gen = find_event<Events::GenericRadioParamsEvent>(*result);
  REQUIRE(gen);
  auto* radio = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(radio);
  CHECK(radio->data.emm_state == 6);
  CHECK(radio->data.emm_mcc == 250);
  CHECK(radio->data.mme_present);
  CHECK(radio->data.mme_group_id == 0x3681);
}

TEST_CASE("0xB0EE — unknown version / too short", "[lte][binary][b0ee]") {
  LteParser parser;
  uint8_t bad_ver[] = {0x99, 0x03, 0x00, 0x00, 0x52, 0xF0, 0x10, 0x01, 0x06, 0x52,
                       0xF0, 0x10, 0x36, 0x81, 0xE0, 0x13, 0x88, 0x80, 0xD8};
  auto r1 = parser.parse_emm_state(std::span{bad_ver});
  REQUIRE(r1.has_value());
  CHECK(r1->empty());

  const uint8_t shortp[] = {0x02};
  auto r2 = parser.parse_emm_state(std::span{shortp});
  REQUIRE(!r2.has_value());
  CHECK(r2.error() == ParserError::PacketTooShort);
}

TEST_CASE("Evt1606 — RRC state names + helpers", "[lte][binary][rrc_state]") {
  namespace E = Wire::Evt1606;
  CHECK(std::string(E::name(0)) == "Inactive");
  CHECK(std::string(E::name(1)) == "Idle Not Camped");
  CHECK(std::string(E::name(2)) == "Idle Camped");
  CHECK(std::string(E::name(3)) == "Connecting");
  CHECK(std::string(E::name(4)) == "Connected");
  CHECK(std::string(E::name(5)) == "Suspend");  // assumed
  CHECK(std::string(E::name(6)) == "IRAT To LTE Started");
  CHECK(std::string(E::name(7)) == "Closing");
  CHECK(E::is_idle_camped(2));
  CHECK(E::is_connected(4));
  CHECK(E::is_connecting_or_connected(3));
  CHECK(E::is_not_camped(1));
  CHECK_FALSE(E::known(8));
}

TEST_CASE("Evt1606 — journal event binds RRC state to serving", "[lte][binary][rrc_state]") {
  QualcomParser qp;
  // Mint serving LTE row first (empty-key events bind to serving).
  Events::RadioParamsEvent<LteRadioParams> radio;
  radio.data.earfcn = 375;
  radio.data.pci = 156;
  qp.tracker().handle_rrc_event(Events::RrcEventEnvelope{
      .key = {.freq = 375, .pci_bsic = 156},
      .rat = RatType::LTE,
      .event_data = Events::RrcEvent{std::move(radio)},
  });
  qp.tracker().handle_rrc_event(Events::RrcEventEnvelope{
      .key = {.freq = 375, .pci_bsic = 156},
      .rat = RatType::LTE,
      .event_data = Events::ServingChangedEvent{.is_serving = true},
  });

  // Journal 0x0000: [len LE16][eid 0x2646][8B ts][state]
  // Oracle states from QXDM Item View (Suspend=5 not in live dumps).
  const uint8_t states[] = {0, 1, 2, 3, 4, 6, 7};
  const char* names[] = {"Inactive",         "Idle Not Camped", "Idle Camped", "Connecting",
                         "Connected",        "IRAT To LTE Started", "Closing"};
  for (size_t i = 0; i < sizeof(states); ++i) {
    uint8_t ev[] = {0x0B, 0x00, 0x46, 0x26, 0, 0, 0, 0, 0, 0, 0, 0, states[i]};
    REQUIRE(qp.on_packet({.log_code = 0x0000, .timestamp = 1, .payload = ev}));
    auto snap = qp.tracker().get_snapshot();
    REQUIRE(snap.size() == 1);
    auto* lte = snap[0].radio_as_if<LteRadioParams>();
    REQUIRE(lte);
    CHECK(lte->rrc_state == states[i]);
    CHECK(std::string(Wire::Evt1606::name(static_cast<uint8_t>(lte->rrc_state))) == names[i]);
  }

  // Connecting from QXDM-wrapped body (state only — build same journal form).
  uint8_t connecting[] = {0x0B, 0x00, 0x46, 0x26, 0x8C, 0x5A, 0x46, 0x01,
                          0x6A, 0xEF, 0x11, 0x01, 0x03};
  REQUIRE(qp.on_packet({.log_code = 0x0000, .payload = connecting}));
  auto* lte = qp.tracker().get_snapshot()[0].radio_as_if<LteRadioParams>();
  REQUIRE(lte);
  CHECK(lte->rrc_state == 3);
}

TEST_CASE("0xB17F v4 — serving meas with valid RSRP", "[lte][binary]") {
  LteParser parser;

  // RSRP raw = 1600 -> -80 dBm (valid)
  // RSRQ raw = bits[22:] of word at +16
  // RSSI raw = bits[11:22] of word at +20
  uint8_t payload[28] = {};
  payload[0] = 4;  // version
  // EARFCN=2660 at +4
  payload[4] = 0x64;
  payload[5] = 0x0A;
  // PCI_SLP = 72<<7 = 0x2400 at +6
  payload[6] = 0x00;
  payload[7] = 0x24;
  // RSRP word at +8: raw=1600 in low 12 bits = 0x640
  payload[8] = 0x40;
  payload[9] = 0x06;
  payload[10] = 0x00;
  payload[11] = 0x00;
  // RSRQ word at +16: raw in top 10 bits = 160 << 22 = 0x28000000
  payload[16] = 0x00;
  payload[17] = 0x00;
  payload[18] = 0x00;
  payload[19] = 0x28;
  // RSSI word at +20: raw in bits [11:22] = 800 << 11 = 0x190000
  payload[20] = 0x00;
  payload[21] = 0x90;
  payload[22] = 0x01;
  payload[23] = 0x00;

  auto sv = std::span{payload};
  auto result = parser.parse_ml1_serving(sv);

  REQUIRE(result.has_value());
  auto& events = result.value();
  REQUIRE(!events.empty());

  auto* sig_ev = find_event<Events::SignalUpdateEvent>(events);
  REQUIRE(sig_ev);

  auto* lte_sig = sig_ev->signal.get_if<LteSignalParams>();
  REQUIRE(lte_sig);
  CHECK_THAT(lte_sig->rsrp, WithinAbs(-80.0, 0.5));

  auto* gen = find_event<Events::GenericRadioParamsEvent>(events);
  REQUIRE(gen);
  auto* radio = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(radio);
  CHECK(radio->data.earfcn == 2660);
  CHECK(radio->data.pci == 72);  // 0x2400 → MSB (low 9 bits empty)
}

TEST_CASE("0xB17F — invalid RSRP is rejected", "[lte][binary]") {
  LteParser parser;

  uint8_t payload[28] = {};
  payload[0] = 4;
  payload[4] = 0x64;
  payload[5] = 0x0A;
  // RSRP raw = 0xFFF -> ~-67.2 + noise = something invalid
  // Actually 0xFFF = 4095 -> 4095*0.0625-180 = 75.9 (way invalid, >-30)
  payload[8] = 0xFF;
  payload[9] = 0x0F;

  auto sv = std::span{payload};
  auto result = parser.parse_ml1_serving(sv);
  REQUIRE(result.has_value());
  CHECK(result.value().empty());
}

TEST_CASE("0xB0C4 v2 — PLMN search response (SIM8300 capture)", "[lte][binary][plmn]") {
  LteParser parser;
  // Truncated live capture: header + PLMN entries (type 02/03).
  const uint8_t payload[] = {
      0x02, 0x03, 0x01, 0x23, 0x07, 0x00, 0x00, 0x00,
      0x02, 0x52, 0xF0, 0x20, 0x00, 0x00, 0x00, 0x00,  // 250-02 no earfcn
      0x02, 0x52, 0xF0, 0x02, 0x00, 0x00, 0x00, 0x00,  // 250-20 no earfcn
      0x03, 0x52, 0xF0, 0x02, 0x38, 0x18, 0x00, 0x00,  // 250-20 @ EARFCN 6200
      0x03, 0x52, 0xF0, 0x20, 0x3C, 0x06, 0x00, 0x00,  // 250-02 @ 1596
      0x03, 0x52, 0xF0, 0x11, 0x3C, 0x06, 0x00, 0x00,  // 250-11 @ 1596
      0x03, 0x52, 0xF0, 0x10, 0x77, 0x01, 0x00, 0x00,  // 250-01 @ 375
      0x03, 0x52, 0xF0, 0x99, 0x86, 0x05, 0x00, 0x00,  // 250-99 @ 1414
      0x90, 0x01, 0x00, 0x00,                          // end marker
  };

  auto result = parser.parse_plmn_search_rsp(std::span{payload});
  REQUIRE(result.has_value());
  auto& events = result.value();
  // 5 type-03 rows → Radio + Passport each
  REQUIRE(events.size() == 10);

  auto* radio0 = std::get_if<Events::GenericRadioParamsEvent>(&events[0]);
  REQUIRE(radio0);
  auto* lte0 = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(radio0);
  REQUIRE(lte0);
  CHECK(lte0->data.earfcn == 6200);

  auto* pass0 = std::get_if<Events::PassportEvent>(&events[1]);
  REQUIRE(pass0);
  CHECK(pass0->passport.mcc == 250);
  CHECK(pass0->passport.mnc == 20);
}

TEST_CASE("0xB176 — initial acquisition EARFCN+PCI", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[] = {
      0x20, 0x00, 0x00, 0x00, 0xFB, 0x04, 0x00, 0x00, 0x02, 0x84, 0x40, 0x28,
      0x48, 0x00, 0x00, 0x00, 0xFF, 0x27, 0xD9, 0x0B, 0xAE, 0x00, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x8E, 0x3B, 0x00, 0x00, 0xFF, 0x27, 0xD9, 0x0B,
      0x00, 0x00, 0xC0, 0x8B,
  };
  auto result = parser.parse_initial_acq(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 1275);
  CHECK(lte->data.pci == 174);
}

TEST_CASE("0xB194 0x1D — ML1 neighbour search response", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[] = {
      0x01, 0x01, 0x10, 0xC4, 0x1D, 0x28, 0x2C, 0x00, 0x44, 0x00, 0x00, 0x00,
      0x50, 0x26, 0x56, 0x26, 0xC0, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x48, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0xEB, 0xA1, 0x94,
      0x74, 0x0E, 0x00, 0x00, 0xD4, 0x01, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_search_rr(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 3400);
  CHECK(lte->data.pci == 468);
}

TEST_CASE("0xB194 0x1C — search request ignored", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[] = {
      0x01, 0x01, 0xCE, 0xC5, 0x1C, 0x28, 0x30, 0x00, 0x20, 0x02, 0x85, 0x00,
      0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x48, 0x0D, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0xB6, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_search_rr(std::span{payload});
  REQUIRE(result.has_value());
  CHECK(result->empty());
}

TEST_CASE("0xB194 — two 0x1D responses emit two RADIO rows", "[lte][binary]") {
  LteParser parser;
  // Two back-to-back 0x1D (ver=0x28, size=0x28=40): EARFCN@body+16 PCI@body+28
  // Cell A: EARFCN 3400 PCI 468; Cell B: EARFCN 1275 PCI 174
  const uint8_t payload[] = {
      0x01, 0x02, 0x00, 0x00,
      // 0x1D #1 — 40 bytes
      0x1D, 0x28, 0x28, 0x00, 0x44, 0x00, 0x00, 0x00, 0x50, 0x26, 0x56, 0x26, 0xC0, 0x20, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0xEB,
      0xA1, 0x94, 0xD4, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      // 0x1D #2 — 40 bytes
      0x1D, 0x28, 0x28, 0x00, 0x44, 0x00, 0x00, 0x00, 0x50, 0x26, 0x56, 0x26, 0xC0, 0x20, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0xFB, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x19, 0xEB,
      0xA1, 0x94, 0xAE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_search_rr(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 2);

  auto cell_at = [&](size_t i) {
    auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(i));
    REQUIRE(gen);
    auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
    REQUIRE(lte);
    return lte->data;
  };
  CHECK(cell_at(0).earfcn == 3400);
  CHECK(cell_at(0).pci == 468);
  CHECK(cell_at(1).earfcn == 1275);
  CHECK(cell_at(1).pci == 174);
}

TEST_CASE("0xB194 SIM8300 — 0x1D not at offset 4", "[lte][binary]") {
  LteParser parser;
  // Live dump: 0x1D sits at offset 5, EARFCN@body+16 PCI@body+32
  const uint8_t payload[] = {
      0x01, 0x01, 0xF0, 0x7D, 0x5E, 0x1D, 0x28, 0x2C, 0x00, 0x44, 0x00, 0x00,
      0x00, 0xCA, 0x16, 0xD0, 0x16, 0xC0, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x48, 0x0D, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xCF, 0x57, 0x4C,
      0x88, 0x90, 0x0E, 0x00, 0x00, 0xD4, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_search_rr(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 3400);
  CHECK(lte->data.pci == 468);
}

namespace {

std::vector<uint8_t> hex_to_bytes(const char* hex) {
  std::vector<uint8_t> out;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    if (hex[i] == ' ' || hex[i] == '\n') {
      --i;
      continue;
    }
    unsigned v = 0;
    REQUIRE(std::sscanf(hex + i, "%2x", &v) == 1);
    out.push_back(static_cast<uint8_t>(v));
  }
  return out;
}

struct B193Expect {
  uint32_t earfcn{};
  uint16_t pci{};
  uint16_t sfn{};
  uint8_t subframe{};
  uint8_t valid_rx{};
  float rsrp_inst{};
  float rsrp_filt{};
  float rsrq_inst{};
  float rsrq_filt{};
  float rssi{};
  float snr0{};
  float snr1{};
};

void check_b193_payload(const std::vector<uint8_t>& payload, const B193Expect& exp) {
  LteParser parser;
  auto result = parser.parse_ml1_meas_resp(payload);
  REQUIRE(result.has_value());

  const Events::NeighborMeasEvent* nev = nullptr;
  const Events::ServingChangedEvent* srv = nullptr;
  const Events::SignalUpdateEvent* sig = nullptr;
  const Events::RadioParamsEvent<LteRadioParams>* radio_cell = nullptr;
  for (const auto& ev : *result) {
    if (auto* n = std::get_if<Events::NeighborMeasEvent>(&ev)) nev = n;
    if (auto* s = std::get_if<Events::ServingChangedEvent>(&ev)) srv = s;
    if (auto* s = std::get_if<Events::SignalUpdateEvent>(&ev)) sig = s;
    if (auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&ev)) {
      if (auto* r = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen)) {
        if (r->data.pci == exp.pci) radio_cell = r;
      }
    }
  }
  REQUIRE(nev);
  REQUIRE_FALSE(nev->neighbors.empty());
  const auto& n = nev->neighbors[0];
  CHECK(n.pci == exp.pci);
  REQUIRE(n.has_rsrp);
  CHECK_THAT(n.rsrp_dbm, WithinAbs(exp.rsrp_inst, 0.02));
  REQUIRE(n.has_rsrp_filt);
  CHECK_THAT(n.rsrp_filt, WithinAbs(exp.rsrp_filt, 0.02));
  REQUIRE(n.has_rsrq);
  CHECK_THAT(n.rsrq_db, WithinAbs(exp.rsrq_inst, 0.02));
  REQUIRE(n.has_rsrq_filt);
  CHECK_THAT(n.rsrq_filt, WithinAbs(exp.rsrq_filt, 0.02));
  REQUIRE(n.has_rssi);
  CHECK_THAT(n.rssi_dbm, WithinAbs(exp.rssi, 0.02));
  REQUIRE(n.has_sinr);
  CHECK_THAT(n.sinr_db, WithinAbs(std::max(exp.snr0, exp.snr1), 0.02));
  REQUIRE(n.has_sinr_per_rx);
  CHECK_THAT(n.sinr_rx0, WithinAbs(exp.snr0, 0.02));
  CHECK_THAT(n.sinr_rx1, WithinAbs(exp.snr1, 0.02));

  REQUIRE(sig);
  auto* lp = sig->signal.get_if<LteSignalParams>();
  REQUIRE(lp);
  CHECK_THAT(lp->rsrp, WithinAbs(exp.rsrp_inst, 0.02));
  REQUIRE(lp->has_rsrp_filt);
  CHECK_THAT(lp->rsrp_filt, WithinAbs(exp.rsrp_filt, 0.02));
  CHECK_THAT(lp->rsrq, WithinAbs(exp.rsrq_inst, 0.02));
  REQUIRE(lp->has_rsrq_filt);
  CHECK_THAT(lp->rsrq_filt, WithinAbs(exp.rsrq_filt, 0.02));
  REQUIRE(lp->has_rssi);
  CHECK_THAT(lp->rssi, WithinAbs(exp.rssi, 0.02));
  REQUIRE(lp->has_sinr);
  CHECK_THAT(lp->sinr, WithinAbs(std::max(exp.snr0, exp.snr1), 0.02));
  REQUIRE(lp->has_sinr_per_rx);
  CHECK_THAT(lp->sinr_rx0, WithinAbs(exp.snr0, 0.02));
  CHECK_THAT(lp->sinr_rx1, WithinAbs(exp.snr1, 0.02));

  REQUIRE(radio_cell);
  CHECK(radio_cell->data.earfcn == exp.earfcn);
  CHECK(radio_cell->data.pci == exp.pci);
  REQUIRE(radio_cell->data.has_sfn_sf);
  CHECK(radio_cell->data.sfn == exp.sfn);
  CHECK(radio_cell->data.subframe == exp.subframe);
  CHECK(radio_cell->data.valid_rx == exp.valid_rx);
  CHECK(radio_cell->data.serving_cell_index == 0);
  CHECK_FALSE(radio_cell->data.is_restricted);
  REQUIRE(srv);
}

}  // namespace

TEST_CASE("0xB193 v48 LE — synthetic EARFCN3400/PCI468 exact", "[lte][binary][b193][synthetic]") {
  // Historical SIM8300 dump; values recomputed from LE SSOT (not RevWordBits).
  const auto payload = hex_to_bytes(
      "0101010019309c00480d0000010003000001ffffd4110000c525000050b30500a8d9320e"
      "c6cd150073f554004f0500006009960000302f00733557002eb9f4100f0100002eb9e412"
      "d57a160000000000d5020000f7fff8ff0000000034003600000000000000ff7f47975f01"
      "17457700c731030000000000df3f010048860000c33b030000000000000000002e010000"
      "1b190000591500000000000000000000");
  check_b193_payload(payload, B193Expect{
                                  .earfcn = 3400,
                                  .pci = 468,
                                  .sfn = 453,
                                  .subframe = 9,
                                  .valid_rx = 3,
                                  .rsrp_inst = -92.8125f,
                                  .rsrp_filt = -92.8125f,
                                  .rsrq_inst = -11.125f,
                                  .rsrq_filt = -11.125f,
                                  .rssi = -64.6875f,
                                  .snr0 = 25.5f,
                                  .snr1 = 20.8f,
                              });
}

TEST_CASE("0xB193 v48 LE — real QXDM SFN544 dual-Rx", "[lte][binary][b193][real]") {
  // Live DIAG payload (QXDM wrapper stripped). Oracle: QXDM Item View.
  const auto payload = hex_to_bytes(
      "010133B819309C00"
      "D4940000010003000001FFFFC7110000"
      "20220000D87806006C3C0B11215A1400"
      "0A055700710500009F15580000002F00"
      "70155700AEB8920CC9000000C924330F"
      "283B1B000000000067030000F8FFF1FF"
      "000000001F002000000000000000FF7F"
      "7EBE0A0078E70400301D020000000000"
      "580E00001C0500003C1F020000000000"
      "69000000C9000000900B000018070000"
      "0000000000000000");
  check_b193_payload(payload, B193Expect{
                                  .earfcn = 38100,
                                  .pci = 455,
                                  .sfn = 544,
                                  .subframe = 8,
                                  .valid_rx = 3,
                                  .rsrp_inst = -93.00f,
                                  .rsrp_filt = -92.94f,
                                  .rsrq_inst = -17.44f,
                                  .rsrq_filt = -14.81f,
                                  .rssi = -55.56f,
                                  .snr0 = 10.40f,
                                  .snr1 = 7.00f,
                              });
}

TEST_CASE("0xB193 v48 LE — real QXDM SFN464 dual-Rx", "[lte][binary][b193][real]") {
  const auto payload = hex_to_bytes(
      "010133B819309C00"
      "D4940000010003000001FFFFC7110000"
      "D0210000DC7806006E3C8B0ED1491400"
      "217556006F0500009F15580000702E00"
      "67F55600CF3CC30CCC000000CF3CB30E"
      "03E31A00000000005C030000F6FFF1FF"
      "000000002A002000000000000000FF7F"
      "64840D008F5406003A33020000000000"
      "DC0C000033070000373D020000000000"
      "70000000CF000000130B000097080000"
      "0000000000000000");
  check_b193_payload(payload, B193Expect{
                                  .earfcn = 38100,
                                  .pci = 455,
                                  .sfn = 464,
                                  .subframe = 8,
                                  .valid_rx = 3,
                                  .rsrp_inst = -93.56f,
                                  .rsrp_filt = -93.06f,
                                  .rsrq_inst = -17.06f,
                                  .rsrq_filt = -15.31f,
                                  .rssi = -56.25f,
                                  .snr0 = 11.40f,
                                  .snr1 = 8.10f,
                              });
}

TEST_CASE("0xB193 v48 LE — real QXDM SFN282 dual-Rx (filt≠@24)", "[lte][binary][b193][real]") {
  // Proves Filtered RSRP is u32@36>>12, not the stale u16@24 lookalike.
  const auto payload = hex_to_bytes(
      "0101000019309C00"
      "D4940000010003000001FFFFC7110000"
      "1A0D0000DC7806006E3CD3081A951400"
      "268556006A0500009F15580000802E00"
      "68D55400CF3C030AA0000000CF3CA310"
      "16431C000000000088030000F5FFEFFF"
      "000000001F002000000000000000FF7F"
      "49B20600967C02001BE1010000000000"
      "B610000007070000433B020000000000"
      "8D000000CF0000003F0C00007C080000"
      "0000000000000000");
  check_b193_payload(payload, B193Expect{
                                  .earfcn = 38100,
                                  .pci = 455,
                                  .sfn = 282,
                                  .subframe = 3,
                                  .valid_rx = 3,
                                  .rsrp_inst = -93.50f,
                                  .rsrp_filt = -95.19f,
                                  .rsrq_inst = -17.06f,
                                  .rsrq_filt = -13.38f,
                                  .rssi = -53.50f,
                                  .snr0 = 8.30f,
                                  .snr1 = 4.00f,
                              });

  // Layout-level: Inst RSRP Rx0/Rx1 match QXDM per-antenna lines.
  const auto lay = QCom::Lte::Wire::B193::sp19_rev_layout(48);
  REQUIRE(lay);
  const QCom::Utils::BinaryCursor pkt{payload};
  const auto body = pkt.slice(8, payload.size() - 8);
  const auto cell = body.at(lay->cell_start);
  const auto meas = QCom::Lte::Wire::B193::decode_sp19_le_cell(cell);
  REQUIRE(meas);
  CHECK_THAT(meas->rsrp_rx0, WithinAbs(-97.69f, 0.02));
  CHECK_THAT(meas->rsrp_rx1, WithinAbs(-93.50f, 0.02));
  CHECK_THAT(meas->rsrq_rx0, WithinAbs(-17.06f, 0.02));
  CHECK_THAT(meas->rsrq_rx1, WithinAbs(-20.00f, 0.02));
}

TEST_CASE("0xB175 v48 histogram — empty (not fake MIB BW)", "[lte][binary][b175]") {
  LteParser parser;
  std::vector<uint8_t> payload(384, 0);
  payload[0] = 48;
  payload[7] = 0x02;  // would previously false-unpack as ASN.1 MIB
  auto result = parser.parse_mib_metrics(payload);
  REQUIRE(result.has_value());
  CHECK(result->empty());
}

TEST_CASE("0xB179 v4 — connected intra EARFCN/PCI + RSRP", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[] = {
      0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x0D, 0x00, 0x00,
      0xD4, 0x01, 0x77, 0x23, 0x6D, 0x05, 0x6D, 0x05, 0x5F, 0x01, 0x5F, 0x01,
      0x00, 0x00, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_conn_intra(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() >= 1);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 3400);
  CHECK(lte->data.pci == 468);
  const Events::SignalUpdateEvent* sig = nullptr;
  for (const auto& ev : *result) {
    if (auto* s = std::get_if<Events::SignalUpdateEvent>(&ev)) sig = s;
  }
  REQUIRE(sig);
  auto* lp = sig->signal.get_if<LteSignalParams>();
  REQUIRE(lp);
  CHECK_THAT(lp->rsrp, WithinAbs(-93.2, 1.0));
}

TEST_CASE("0xB181 — serving + TLV candidate EARFCNs", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[] = {
      0x01, 0x02, 0x1C, 0x3C, 0x0A, 0x02, 0x0C, 0x00, 0x48, 0x0D, 0x00, 0x00,
      0xD4, 0x03, 0x00, 0x00, 0x0B, 0x28, 0x5C, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x06, 0x00, 0x00, 0x00, 0x38, 0x18, 0x00, 0x00, 0x79, 0x03, 0x08, 0x08,
      0x5E, 0x97, 0x00, 0x00, 0x79, 0x07, 0x0E, 0x0E, 0x24, 0x98, 0x00, 0x00,
      0x79, 0x07, 0x0E, 0x0E, 0xEA, 0x98, 0x00, 0x00, 0x79, 0x07, 0x0E, 0x0E,
      0x48, 0x0D, 0x00, 0x00, 0x79, 0x05, 0x0C, 0x0C, 0xFB, 0x04, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_intra_resel(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() >= 2);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 3400);
  CHECK(lte->data.pci == 468);
  auto* car = std::get_if<Events::InterFreqCarriersEvent>(&result->at(1));
  REQUIRE(car);
  REQUIRE(car->carriers.size() >= 3);
  CHECK(car->carriers[0].earfcn == 38750);
  CHECK(car->carriers[1].earfcn == 38948);
}

TEST_CASE("0xB192 — idle neigh meas (MI 26v2+27v4)", "[lte][binary]") {
  LteParser parser;
  // Live SIM8300 dump: pkt v1, 2 subpkts — request 26v2 + result 27v4, PCI 327 @ EARFCN 3400.
  const uint8_t payload[] = {
      0x01, 0x02, 0x44, 0xB7, 0x1A, 0x02, 0x1C, 0x00, 0x48, 0x0D, 0x00, 0x00,
      0x21, 0x00, 0x00, 0x00, 0x47, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1B, 0x04, 0x40, 0x00,
      0x48, 0x0D, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x47, 0x01, 0x00, 0x00,
      0xA8, 0x83, 0x3A, 0x00, 0xD6, 0x63, 0x3D, 0x00, 0xD6, 0x63, 0x3D, 0x00,
      0x7B, 0xEC, 0xC1, 0x08, 0x8C, 0x30, 0xC2, 0x08, 0x3E, 0x01, 0x00, 0x00,
      0x5A, 0xD1, 0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x34, 0x00, 0x2A, 0x00,
      0x87, 0xE7, 0x02, 0x00, 0x87, 0xE7, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_neigh_req(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() >= 2);

  bool saw_radio = false;
  bool saw_meas = false;
  for (const auto& ev : *result) {
    if (auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&ev)) {
      auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
      REQUIRE(lte);
      CHECK(lte->data.earfcn == 3400);
      CHECK(lte->data.pci == 327);
      saw_radio = true;
    } else if (auto* nev = std::get_if<Events::NeighborMeasEvent>(&ev)) {
      REQUIRE_FALSE(nev->neighbors.empty());
      CHECK(nev->neighbors[0].pci == 327);
      CHECK(nev->neighbors[0].has_rsrp);
      // MI: (0x63D6 & 4095)*0.0625 - 180 = -118.625
      CHECK_THAT(nev->neighbors[0].rsrp_dbm, WithinAbs(-118.625f, 0.01f));
      saw_meas = true;
    }
  }
  CHECK(saw_radio);
  CHECK(saw_meas);
}

TEST_CASE("0xB195 — connected neigh meas (MI 31v4)", "[lte][binary]") {
  LteParser parser;
  // Synthetic: pkt v1, 1 subpkt id=31 ver=4 size=4+8+52=64, EARFCN 3400, PCI 327, RSRP raw 0x63D6.
  std::vector<uint8_t> payload(4 + 64, 0);
  payload[0] = 1;
  payload[1] = 1;
  payload[4] = 31;   // sid
  payload[5] = 4;    // sver
  payload[6] = 64;   // size lo
  payload[7] = 0;    // size hi
  // EARFCN 3400
  payload[8] = 0x48;
  payload[9] = 0x0D;
  // Num cells = 1 + pad
  payload[12] = 1;
  payload[13] = 0;
  payload[14] = 0;
  payload[15] = 0;
  // PCI 327 at cell+0
  payload[16] = 0x47;
  payload[17] = 0x01;
  // RSRP at cell+12
  payload[28] = 0xD6;
  payload[29] = 0x63;
  // RSRQ at cell+20: craft ((194)<<10) in low bits of a u32 → 194*0.0625-30 = -17.875
  const uint32_t rsrq_word = 194u << 10;
  payload[36] = static_cast<uint8_t>(rsrq_word & 0xFF);
  payload[37] = static_cast<uint8_t>((rsrq_word >> 8) & 0xFF);
  payload[38] = static_cast<uint8_t>((rsrq_word >> 16) & 0xFF);
  payload[39] = static_cast<uint8_t>((rsrq_word >> 24) & 0xFF);

  auto result = parser.parse_ml1_conn_neigh(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->empty());

  bool saw_meas = false;
  for (const auto& ev : *result) {
    if (auto* nev = std::get_if<Events::NeighborMeasEvent>(&ev)) {
      REQUIRE(nev->neighbors.size() == 1);
      CHECK(nev->neighbors[0].pci == 327);
      CHECK(nev->neighbors[0].has_rsrp);
      CHECK_THAT(nev->neighbors[0].rsrp_dbm, WithinAbs(-118.625f, 0.01f));
      saw_meas = true;
    }
  }
  CHECK(saw_meas);
}

TEST_CASE("0xB195 — SIM8300 subpkt 31 ver=40 wide", "[lte][binary][b195]") {
  LteParser parser;
  // Live dump B195[1]: n_sub=2, req 0x1E + result 0x1F ver=40, EARFCN 3400, PCI 468.
  const char* hex =
      "01029fff1e282000480d0000a1000000f1810000d4110000405200004052000000000000"
      "1f284000480d000001340802d40100001005510022255200222552003bedc4133cf1c413"
      "e6010000f6b10f000000000034003500405200004052000000000000";
  std::vector<uint8_t> payload;
  for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) {
    auto nib = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + c - 'a');
      if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + c - 'A');
      return 0;
    };
    payload.push_back(static_cast<uint8_t>((nib(hex[i]) << 4) | nib(hex[i + 1])));
  }
  auto result = parser.parse_ml1_conn_neigh(std::span{payload});
  REQUIRE(result.has_value());
  bool saw = false;
  for (const auto& ev : *result) {
    if (auto* nev = std::get_if<Events::NeighborMeasEvent>(&ev)) {
      REQUIRE_FALSE(nev->neighbors.empty());
      CHECK(nev->neighbors[0].pci == 468);
      CHECK(nev->neighbors[0].has_rsrp);
      saw = true;
    }
    if (auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&ev)) {
      auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
      REQUIRE(lte);
      CHECK(lte->data.earfcn == 3400);
      CHECK(lte->data.pci == 468);
    }
  }
  CHECK(saw);
}

TEST_CASE("0xB179 v1 shifted layout", "[lte][binary]") {
  LteParser parser;
  // Live: ver=1 earfcn 1275 @9, pci 174 @13
  const uint8_t payload[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xfb,
                             0x04, 0x00, 0x00, 0xae, 0x00, 0x77, 0x20, 0x01, 0x06, 0x01,
                             0x06, 0x68, 0x01, 0x68, 0x01, 0x00, 0x00, 0x00};
  auto result = parser.parse_ml1_conn_intra(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE_FALSE(result->empty());
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.earfcn == 1275);
  CHECK(lte->data.pci == 174);
}

TEST_CASE("0xB195 — connected neigh meas (MI 31v24)", "[lte][binary]") {
  LteParser parser;
  // Same as v4 layout; MI also documents subpkt ver=24 with Payload_31v4.
  std::vector<uint8_t> payload(4 + 64, 0);
  payload[0] = 1;
  payload[1] = 1;
  payload[4] = 31;
  payload[5] = 24;
  payload[6] = 64;
  payload[8] = 0x48;
  payload[9] = 0x0D;
  payload[12] = 1;
  payload[16] = 0x47;
  payload[17] = 0x01;
  payload[28] = 0xD6;
  payload[29] = 0x63;
  const uint32_t rsrq_word = 194u << 10;
  payload[36] = static_cast<uint8_t>(rsrq_word & 0xFF);
  payload[37] = static_cast<uint8_t>((rsrq_word >> 8) & 0xFF);
  payload[38] = static_cast<uint8_t>((rsrq_word >> 16) & 0xFF);
  payload[39] = static_cast<uint8_t>((rsrq_word >> 24) & 0xFF);

  auto result = parser.parse_ml1_conn_neigh(std::span{payload});
  REQUIRE(result.has_value());
  auto* nev = find_event<Events::NeighborMeasEvent>(*result);
  REQUIRE(nev);
  REQUIRE(nev->neighbors.size() == 1);
  CHECK(nev->neighbors[0].pci == 327);
  CHECK_THAT(nev->neighbors[0].rsrp_dbm, WithinAbs(-118.625f, 0.01f));
}

TEST_CASE("0xB115 — SSS EARFCN as inter-freq carrier", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[] = {0x7A, 0x00, 0x00, 0x00, 0x24, 0x98, 0x00, 0x00};
  auto result = parser.parse_ll1_sss(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 1);
  auto* car = std::get_if<Events::InterFreqCarriersEvent>(&result->at(0));
  REQUIRE(car);
  REQUIRE(car->carriers.size() == 1);
  CHECK(car->carriers[0].earfcn == 38948);
}

TEST_CASE("0xB115 v122 — detected cells mint RADIO rows", "[lte][binary]") {
  LteParser parser;
  // Live COPS dump: EARFCN 1596, PCIs 157/447/275/372 (low9 of u16 @record+2)
  const uint8_t payload[] = {
      0x7A, 0x80, 0x00, 0x00, 0x3C, 0x06, 0x00, 0x00, 0x67, 0x2E, 0x9D, 0x04,
      0x64, 0x00, 0xFF, 0xFF, 0xAC, 0x53, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x97, 0x28, 0xBF, 0x01, 0x08, 0x01, 0xFF, 0xFF, 0x5C, 0x3F, 0x02, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x1E, 0x1D, 0x13, 0x05, 0xEE, 0x01, 0xFF, 0xFF,
      0xBC, 0x53, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x74, 0x05,
      0x93, 0x01, 0xFF, 0xFF, 0xCC, 0x96, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  auto result = parser.parse_ll1_sss(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 4);
  const uint16_t expect_pci[] = {157, 447, 275, 372};
  for (size_t i = 0; i < 4; ++i) {
    auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(i));
    REQUIRE(gen);
    auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
    REQUIRE(lte);
    CHECK(lte->data.earfcn == 1596);
    CHECK(lte->data.pci == expect_pci[i]);
  }
}

TEST_CASE("0xB194 — multi-PCI list in one 0x1D body", "[lte][binary]") {
  LteParser parser;
  // Live COPS: EARFCN 1596, PCIs 157/447/275/372 in 16B records @body+24
  const uint8_t payload[] = {
      0x01, 0x01, 0x63, 0x00, 0x1D, 0x28, 0x5C, 0x00, 0x50, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x06, 0x00, 0x2A, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x3C, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xA8, 0x83, 0x2E, 0x00,
      0xFC, 0x0A, 0x00, 0x00, 0x9D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x68, 0x1F, 0x31, 0x00, 0xB0, 0x07, 0x00, 0x00, 0xBF, 0x01, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0xB8, 0x83, 0x2E, 0x00, 0xE7, 0x05, 0x00, 0x00,
      0x13, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xD8, 0xC6, 0x2D, 0x00,
      0x8D, 0x04, 0x00, 0x00, 0x74, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  auto result = parser.parse_ml1_search_rr(std::span{payload});
  REQUIRE(result.has_value());
  REQUIRE(result->size() == 4);
  const uint16_t expect_pci[] = {157, 447, 275, 372};
  for (size_t i = 0; i < 4; ++i) {
    auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&result->at(i));
    REQUIRE(gen);
    auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
    REQUIRE(lte);
    CHECK(lte->data.earfcn == 1596);
    CHECK(lte->data.pci == expect_pci[i]);
  }
}

TEST_CASE("0xB123 CER — no invented cells", "[lte][binary]") {
  LteParser parser;
  const uint8_t payload[160] = {0x29, 0x47, 0x01, 0x00};
  auto result = parser.parse_ll1_ncell_cer(std::span{payload});
  REQUIRE(result.has_value());
  CHECK(result->empty());
}

// ---------------------------------------------------------------------------
// 0xB114 — LL1 Serving Cell Frame Timing (QXDM v161, SIM8300 live)
// ---------------------------------------------------------------------------

TEST_CASE("0xB114 wire SSOT — TA packed >> 1 + SFN bitfield", "[lte][binary][b114]") {
  namespace W = QCom::Lte::Wire::B114;
  using QCom::Utils::BinaryCursor;

  // Live SIM8300 / QXDM: Version=161, TA=2, DL off=0, UL off=30688, nrec=20
  // Header only (records not required for TA).
  const uint8_t hdr_ta2[] = {
      0xA1, 0x14, 0x04, 0x00, 0x00, 0x00, 0xE0, 0x77, 0x00, 0x78, 0x00, 0x00, 0xE0, 0xEF, 0x00, 0x00,
  };
  auto h2 = W::decode_header(BinaryCursor{std::span{hdr_ta2}});
  REQUIRE(h2);
  CHECK(h2->version == 161);
  CHECK(h2->num_records == 20);
  CHECK(h2->timing_advance == 2);
  CHECK(h2->carrier_pcc);
  CHECK(h2->dl_frame_timing_off_ts == 0);
  CHECK(h2->ul_frame_timing_off_ts == 30688);
  CHECK(h2->dl_sf_mstmr == 30720);
  CHECK(h2->ul_sf_mstmr == 61408);
  CHECK_THAT(W::ta_meters(2), WithinAbs(156.25, 0.01));

  // Live after B7 lock: TA=1, DL off=13314, UL off=13293, nrec=20
  const uint8_t hdr_ta1[] = {
      0xA1, 0x14, 0x02, 0x00, 0x02, 0x34, 0xED, 0x33, 0x02, 0xCC, 0x6E, 0x08, 0xED, 0x43, 0x6F, 0x08,
  };
  auto h1 = W::decode_header(BinaryCursor{std::span{hdr_ta1}});
  REQUIRE(h1);
  CHECK(h1->timing_advance == 1);
  CHECK(h1->dl_frame_timing_off_ts == 13314);
  CHECK(h1->ul_frame_timing_off_ts == 13293);
  CHECK(h1->dl_sf_mstmr == 141478914);
  CHECK(h1->ul_sf_mstmr == 141509613);
  CHECK_THAT(W::ta_meters(1), WithinAbs(78.125, 0.01));

  // SFN|SF packing: live record word 0x1391 → SFN 913, SF 4
  CHECK(W::record_sfn(0x1391) == 913);
  CHECK(W::record_subframe(0x1391) == 4);
  CHECK(W::record_sfn(0x0400) == 0);  // early sample SF=1 frame=0
  CHECK(W::record_subframe(0x0400) == 1);

  // Fail-closed: wrong version
  const uint8_t bad_ver[] = {
      0xA0, 0x14, 0x04, 0x00, 0x00, 0x00, 0xE0, 0x77, 0x00, 0x78, 0x00, 0x00, 0xE0, 0xEF, 0x00, 0x00,
  };
  CHECK_FALSE(W::decode_header(BinaryCursor{std::span{bad_ver}}).has_value());
}

TEST_CASE("0xB114 parser — emit TA onto RadioParams (live hex)", "[lte][binary][b114]") {
  LteParser parser;

  // Full QXDM copy for TA=2 sample truncated to header+1 record (enough for parser).
  // Payload starts at version byte (LOG_F header already stripped).
  const uint8_t ta2_payload[] = {
      0xA1, 0x0A, 0x04, 0x00, 0xAC, 0x1D, 0x8C, 0x1D, 0xAC, 0x25, 0x22, 0x21, 0x8C, 0x9D, 0x22, 0x21,
      // Timing Adjust[0]: SFN=913 SF=4, ant=3, ustmr=10887677
      0x91, 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0xFD, 0x21, 0xA6, 0x00,
      0x00, 0x40, 0x1A, 0x40, 0x69, 0x35, 0x00, 0x00, 0xDB, 0xB3, 0xF8, 0xFF, 0xE1, 0x5A, 0xFF, 0xDF,
      0xA0, 0x89, 0x66, 0x66, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  };
  {
    using QCom::Utils::BinaryCursor;
    namespace W = QCom::Lte::Wire::B114;
    auto hdr = W::decode_header(BinaryCursor{std::span{ta2_payload}});
    REQUIRE(hdr);
    CHECK(hdr->timing_advance == 2);
    CHECK(hdr->num_records == 10);
    CHECK(hdr->dl_frame_timing_off_ts == 7596);
    CHECK(hdr->ul_frame_timing_off_ts == 7564);
    const uint16_t sfn_sf = QCom::Utils::Converter::read_le<uint16_t>(ta2_payload, 16);
    CHECK(W::record_sfn(sfn_sf) == 913);
    CHECK(W::record_subframe(sfn_sf) == 4);
  }

  auto r2 = parser.parse_ll1_frame_timing(std::span{ta2_payload});
  REQUIRE(r2.has_value());
  REQUIRE(r2->size() == 1);
  auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&r2->at(0));
  REQUIRE(gen);
  auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.timing_advance == 2);
  CHECK(lte->data.earfcn == 0);  // identity-less — binder uses serving key

  // TA=1 live header (B7 / EARFCN 3200 camp)
  const uint8_t ta1_hdr[] = {
      0xA1, 0x14, 0x02, 0x00, 0x02, 0x34, 0xED, 0x33, 0x02, 0xCC, 0x6E, 0x08, 0xED, 0x43, 0x6F, 0x08,
  };
  auto r1 = parser.parse_ll1_frame_timing(std::span{ta1_hdr});
  REQUIRE(r1.has_value());
  REQUIRE(r1->size() == 1);
  gen = std::get_if<Events::GenericRadioParamsEvent>(&r1->at(0));
  REQUIRE(gen);
  lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen);
  REQUIRE(lte);
  CHECK(lte->data.timing_advance == 1);

  // Unknown version → empty (fail-closed), not error
  const uint8_t unk[] = {
      0x99, 0x14, 0x04, 0x00, 0x00, 0x00, 0xE0, 0x77, 0x00, 0x78, 0x00, 0x00, 0xE0, 0xEF, 0x00, 0x00,
  };
  auto ru = parser.parse_ll1_frame_timing(std::span{unk});
  REQUIRE(ru.has_value());
  CHECK(ru->empty());

  // Too short
  const uint8_t tiny[] = {0xA1, 0x01};
  auto rs = parser.parse_ll1_frame_timing(std::span{tiny});
  REQUIRE_FALSE(rs.has_value());
}

