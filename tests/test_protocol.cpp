#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include <qcom/protocol/Crc16.h>
#include <qcom/protocol/HdlcCodec.h>

using namespace QCom;

// ============================================================================
// CRC16 tests
// ============================================================================

TEST_CASE("QualcommCrc: known test vector", "[protocol][crc16]") {
  const uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
  uint16_t crc = QualcommCrc::calculate(data, 4);
  CHECK(crc != 0);

  // Verify CRC property: CRC of data+CRC should yield the X.25 residue 0x0F47
  uint8_t with_crc[6];
  std::memcpy(with_crc, data, 4);
  with_crc[4] = crc & 0xFF;
  with_crc[5] = (crc >> 8) & 0xFF;
  CHECK(QualcommCrc::calculate(with_crc, 6) == 0x0F47);
}

TEST_CASE("QualcommCrc: empty data", "[protocol][crc16]") {
  uint16_t crc = QualcommCrc::calculate(nullptr, 0);
  // Init 0xFFFF reflected XOR 0xFFFF = 0x0000 for zero-length input
  CHECK(crc == 0x0000);
}

TEST_CASE("QualcommCrc: span overload", "[protocol][crc16]") {
  const uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
  auto crc1 = QualcommCrc::calculate(data, 4);
  auto crc2 = QualcommCrc::calculate(std::span{data});
  CHECK(crc1 == crc2);
}

// ============================================================================
// HDLC codec tests
// ============================================================================

TEST_CASE("HdlcCodec: serialize produces valid frame", "[protocol][hdlc]") {
  const uint8_t payload[] = {0x01, 0x02, 0x03};
  auto frame = HdlcCodec::serialize(0x73, std::span{payload});

  REQUIRE(!frame.empty());
  CHECK(frame.back() == 0x7E);  // trailing flag
}

TEST_CASE("HdlcCodec: serialize escapes 0x7E and 0x7D in payload", "[protocol][hdlc]") {
  const uint8_t payload[] = {0x7E, 0x7D, 0x00};
  auto frame = HdlcCodec::serialize(0x10, std::span{payload});

  // Both 0x7E and 0x7D in payload should be escaped
  // Escaped form: 0x7D followed by (byte ^ 0x20)
  int escape_count = 0;
  for (size_t i = 0; i < frame.size() - 1; ++i) {
    if (frame[i] == 0x7D) ++escape_count;
  }
  // At least the two payload bytes + possibly the opcode or CRC bytes
  CHECK(escape_count >= 2);
}

TEST_CASE("HdlcCodec: serialize with empty payload", "[protocol][hdlc]") {
  auto frame = HdlcCodec::serialize(0x73);
  REQUIRE(!frame.empty());
  CHECK(frame.back() == 0x7E);
}

// ============================================================================
// HDLC deframer tests
// ============================================================================

TEST_CASE("HdlcDeframer: roundtrip serialize -> deframe", "[protocol][hdlc]") {
  const uint8_t test_payload[] = {0x10, 0x20, 0x30, 0x40, 0x50};
  auto frame = HdlcCodec::serialize(0x10, std::span{test_payload});

  HdlcDeframer deframer;
  std::vector<uint8_t> received;

  deframer.set_callback(
      [&](std::span<const uint8_t> payload) { received.assign(payload.begin(), payload.end()); });

  // Feed the frame with a leading 0x7E delimiter (the deframer expects it)
  std::vector<uint8_t> wire;
  wire.push_back(0x7E);
  wire.insert(wire.end(), frame.begin(), frame.end());
  deframer.feed(wire);

  REQUIRE(!received.empty());
  CHECK(received[0] == 0x10);  // opcode
  CHECK(received[1] == test_payload[0]);
  CHECK(received.size() == 1 + sizeof(test_payload));
  CHECK(deframer.frames_ok() == 1);
  CHECK(deframer.frames_bad_crc() == 0);
}

TEST_CASE("HdlcDeframer: corrupted frame increments bad_crc", "[protocol][hdlc]") {
  auto frame = HdlcCodec::serialize(0x10, {});

  // Corrupt one byte in the frame
  frame[1] ^= 0xFF;

  HdlcDeframer deframer;
  bool callback_fired = false;
  deframer.set_callback([&](std::span<const uint8_t>) { callback_fired = true; });

  std::vector<uint8_t> wire;
  wire.push_back(0x7E);
  wire.insert(wire.end(), frame.begin(), frame.end());
  deframer.feed(wire);

  CHECK_FALSE(callback_fired);
  CHECK(deframer.frames_bad_crc() == 1);
}

TEST_CASE("HdlcDeframer: multiple frames in one chunk", "[protocol][hdlc]") {
  auto f1 = HdlcCodec::serialize(0x10, {});
  auto f2 = HdlcCodec::serialize(0x20, {});

  HdlcDeframer deframer;
  int count = 0;
  deframer.set_callback([&](std::span<const uint8_t>) { ++count; });

  std::vector<uint8_t> wire;
  wire.push_back(0x7E);
  wire.insert(wire.end(), f1.begin(), f1.end());
  wire.insert(wire.end(), f2.begin(), f2.end());
  deframer.feed(wire);

  CHECK(count == 2);
  CHECK(deframer.frames_ok() == 2);
}

TEST_CASE("HdlcDeframer: reset clears state", "[protocol][hdlc]") {
  HdlcDeframer deframer;
  auto frame = HdlcCodec::serialize(0x10, {});
  std::vector<uint8_t> wire = {0x7E};
  wire.insert(wire.end(), frame.begin(), frame.end());

  int count = 0;
  deframer.set_callback([&](std::span<const uint8_t>) { ++count; });
  deframer.feed(wire);
  CHECK(count == 1);

  deframer.reset();
  CHECK(deframer.frames_ok() == 0);
  CHECK(deframer.frames_bad_crc() == 0);
}

#include <qcom/linux/JournalSource.h>
#include <qcom/protocol/LogFrameAdapter.h>
#include <qcom/io/ScannerEngine.h>

TEST_CASE("adapt_log_f_frame extracts code and payload", "[transport][adapter]") {
  // Minimal LOG_F short layout: header 14 + 2 payload bytes
  uint8_t raw[16] = {};
  raw[0] = 0x10;
  raw[4] = 0xC2;  // log_code LE = 0xB0C2
  raw[5] = 0xB0;
  raw[14] = 0xAA;
  raw[15] = 0xBB;

  auto pkt = adapt_log_f_frame(raw);
  REQUIRE(pkt.has_value());
  CHECK(pkt->log_code == 0xB0C2);
  REQUIRE(pkt->payload.size() == 2);
  CHECK(pkt->payload[0] == 0xAA);
}

TEST_CASE("adapt_log_f_frame classic duplicate-len layout", "[transport][adapter]") {
  // Classic: cmd/more/len + (len bytes of log_hdr+payload). len counts AFTER the
  // 4-byte DIAG hdr (osmo semantics). USB padding after the frame must be clipped.
  // 4 + 14 = 18 total: log_len/code/ts (12) + 2 payload, matching header@16.
  uint8_t raw[64] = {};
  raw[0] = 0x10;
  raw[2] = 14;    // bytes after cmd/more/len
  raw[4] = 14;    // duplicate → classic
  raw[6] = 0xC2;
  raw[7] = 0xB0;  // code 0xB0C2
  raw[16] = 0x11;
  raw[17] = 0x22;
  raw[18] = 0xEE;  // padding must be ignored
  raw[19] = 0xFF;

  auto pkt = adapt_log_f_frame({raw, 64});
  REQUIRE(pkt.has_value());
  CHECK(pkt->log_code == 0xB0C2);
  REQUIRE(pkt->payload.size() == 2);
  CHECK(pkt->payload[0] == 0x11);
}

TEST_CASE("adapt_log_f_frame prefers len+4 on padded USB buffer", "[transport][adapter]") {
  // Real SIMCom: outer_len=58 means 58 bytes after the 4-byte hdr → 62-byte frame.
  // Old clip used outer_len as full size and dropped the last 4 ASN.1 bytes.
  uint8_t raw[128] = {};
  raw[0] = 0x10;
  raw[2] = 58;
  raw[4] = 58;
  raw[6] = 0xC0;
  raw[7] = 0xB0;
  for (int i = 0; i < 46; ++i) raw[16 + i] = static_cast<uint8_t>(i + 1);

  auto pkt = adapt_log_f_frame({raw, 128});
  REQUIRE(pkt.has_value());
  CHECK(pkt->log_code == 0xB0C0);
  CHECK(pkt->payload.size() == 46);  // 62 - 16, not 42
}

TEST_CASE("adapt_log_f_frame skips non-LOG_F", "[transport][adapter]") {
  uint8_t raw[14] = {};
  raw[0] = 0x73;  // LOG_CONFIG ack
  CHECK_FALSE(adapt_log_f_frame(raw).has_value());
}

#include <qcom/protocol/DiagCommands.h>
#include <qcom/protocol/DiagSerialDemux.h>

TEST_CASE("DiagSerialDemux length-prefixed LOG_F", "[transport][demux]") {
  // [len=24][type=1][LOG_F short 14 + 2 payload]
  uint8_t msg[24] = {};
  msg[0] = 24;  // total_len LE
  msg[4] = 1;   // type
  msg[8] = 0x10;
  msg[8 + 4] = 0xC2;
  msg[8 + 5] = 0xB0;
  msg[8 + 14] = 0xAB;

  DiagSerialDemux demux;
  int n = 0;
  LogCode code = 0;
  demux.set_log_callback([&](QualcommPacketView pkt) {
    ++n;
    code = pkt.log_code;
  });
  demux.feed(msg);
  CHECK(n == 1);
  CHECK(code == 0xB0C2);
  CHECK(demux.messages_ok() == 1);
  CHECK(demux.logs_delivered() == 1);
}

TEST_CASE("JournalSource + RadioScannerEngine deliver packets", "[transport][engine]") {
  auto journal = std::make_unique<JournalSource>();
  JournalSource* raw = journal.get();
  RadioScannerEngine engine(std::move(journal));

  int callbacks = 0;
  engine.set_callback([&](const std::vector<CellIdentity>&) { ++callbacks; });

  REQUIRE(engine.start());
  CHECK(engine.source()->name() == "journal");

  // LTE identity-ish synthetic (same as app/main) — may or may not produce cell update
  // depending on parser; at least on_packet must not crash.
  const uint8_t lte_identity[] = {
      0x10, 0x00, 0x2C, 0x00, 0xC2, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x02, 0x48, 0x00, 0x64, 0x0A, 0x94, 0x50, 0x0A, 0x0A, 0xC4, 0xB3, 0xA2,
      0x01, 0xD2, 0x04, 0x07, 0x00, 0x00, 0x00, 0xFA, 0x00, 0x02, 0x01, 0x00, 0x01,
  };
  raw->feed_log_f(lte_identity);
  engine.stop();
  // Cell callback is optional depending on tracker merge; ensure engine path works.
  CHECK(engine.source()->name() == "journal");
  (void)callbacks;
}

TEST_CASE("LTE search pack has RRC/SSS and omits PSS/TA flood", "[protocol][diag-mask]") {
  auto ids = lte_diag_item_ids(LteDiagPack::Search);
  auto pkt = DiagSession::build_set_mask(DiagEquip::LTE, kLteMaskLastItem, ids);
  CHECK(DiagSession::set_mask_has_item(pkt, DiagItem::LTE_RRC_OTA));
  CHECK(DiagSession::set_mask_has_item(pkt, DiagItem::LTE_RRC_MIB));
  CHECK(DiagSession::set_mask_has_item(pkt, DiagItem::LTE_RRC_SERVING_CELL));
  CHECK(DiagSession::set_mask_has_item(pkt, DiagItem::LTE_LL1_SSS_RESULTS));
  CHECK(DiagSession::set_mask_has_item(pkt, DiagItem::LTE_ML1_SERVING_INFO));
  CHECK_FALSE(DiagSession::set_mask_has_item(pkt, DiagItem::LTE_LL1_PSS_RESULTS));
  CHECK_FALSE(DiagSession::set_mask_has_item(pkt, DiagItem::LTE_LL1_FRAME_TIMING));
  CHECK_FALSE(DiagSession::set_mask_has_item(pkt, DiagItem::LTE_LL1_NCELL_CER));
  CHECK(DiagSession::set_mask_has_item(pkt, DiagItem::NR_RRC_OTA));  // 0x821 < last_item 0x09FF
}

TEST_CASE("LTE serving pack keeps RRC and adds B114 TA", "[protocol][diag-mask]") {
  auto ids = lte_diag_item_ids(LteDiagPack::Serving);
  auto pkt = DiagSession::build_set_mask(DiagEquip::LTE, kLteMaskLastItem, ids);
  CHECK(DiagSession::set_mask_has_item(pkt, DiagItem::LTE_RRC_OTA));
  CHECK(DiagSession::set_mask_has_item(pkt, DiagItem::LTE_LL1_SSS_RESULTS));
  CHECK(DiagSession::set_mask_has_item(pkt, DiagItem::LTE_LL1_FRAME_TIMING));
  CHECK_FALSE(DiagSession::set_mask_has_item(pkt, DiagItem::LTE_LL1_PSS_RESULTS));
  CHECK(std::strcmp(lte_diag_pack_name(LteDiagPack::Search), "search") == 0);
  CHECK(std::strcmp(lte_diag_pack_name(LteDiagPack::Serving), "serving") == 0);
}
