#include <catch2/catch_test_macros.hpp>
#include <span>

#include "transport/Crc16.h"
#include "transport/HdlcCodec.h"

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
