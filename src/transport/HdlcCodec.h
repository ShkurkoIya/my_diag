/// @file HdlcCodec.h
/// @brief HDLC framing codec: serialize (TX) and stream-deframe (RX) with CRC16 verification.
///
/// HDLC wire format:
///   [0x7E] [escaped payload] [CRC16-LE] [0x7E]
///   Escape: 0x7D followed by (byte ^ 0x20) for bytes 0x7E and 0x7D in payload.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "transport/Crc16.h"

namespace QCom {

/// @brief HDLC frame serializer and byte-level deserializer.
///
/// Serialize: build a complete HDLC frame for transmission.
/// Deserialize: feed raw bytes one-by-one, emit complete deframed payloads via callback.
class HdlcCodec {
public:
  static constexpr uint8_t FLAG = 0x7E;
  static constexpr uint8_t ESCAPE = 0x7D;
  static constexpr uint8_t ESCAPE_XOR = 0x20;

  /// Build an HDLC frame: [opcode][payload] -> CRC -> escape -> delimit.
  [[nodiscard]] static std::vector<uint8_t> serialize(uint8_t opcode,
                                                      std::span<const uint8_t> payload = {}) {
    std::vector<uint8_t> raw;
    raw.reserve(1 + payload.size() + 2);
    raw.push_back(opcode);
    raw.insert(raw.end(), payload.begin(), payload.end());

    uint16_t crc = QualcommCrc::calculate(raw.data(), raw.size());
    raw.push_back(static_cast<uint8_t>(crc & 0xFF));
    raw.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));

    std::vector<uint8_t> frame;
    frame.reserve(raw.size() * 2 + 1);
    for (uint8_t byte : raw) {
      if (byte == FLAG || byte == ESCAPE) {
        frame.push_back(ESCAPE);
        frame.push_back(byte ^ ESCAPE_XOR);
      } else {
        frame.push_back(byte);
      }
    }
    frame.push_back(FLAG);
    return frame;
  }

private:
  friend class HdlcDeframer;
};

/// @brief Streaming HDLC deframer with CRC16 verification.
///
/// Feed raw byte chunks via feed(). When a complete frame is deframed,
/// the callback fires with the clean payload (CRC stripped and verified).
class HdlcDeframer {
public:
  using FrameCallback = std::function<void(std::span<const uint8_t> payload)>;

  explicit HdlcDeframer(size_t max_frame_size = 8192) : m_buf(max_frame_size) {}

  void set_callback(FrameCallback cb) { m_callback = std::move(cb); }

  /// Feed a chunk of raw bytes from the serial port.
  void feed(std::span<const uint8_t> data) {
    for (uint8_t byte : data) feed_byte(byte);
  }

  /// Reset deframer state (e.g. after port reconnect).
  void reset() noexcept {
    m_idx = 0;
    m_inside = false;
    m_escape = false;
    m_frames_ok = 0;
    m_frames_bad_crc = 0;
  }

  [[nodiscard]] uint64_t frames_ok() const noexcept { return m_frames_ok; }
  [[nodiscard]] uint64_t frames_bad_crc() const noexcept { return m_frames_bad_crc; }

private:
  FrameCallback m_callback;
  std::vector<uint8_t> m_buf;
  size_t m_idx{0};
  bool m_inside{false};
  bool m_escape{false};
  uint64_t m_frames_ok{0};
  uint64_t m_frames_bad_crc{0};

  void feed_byte(uint8_t byte) {
    if (byte == HdlcCodec::FLAG) {
      if (m_inside && m_idx > 2) {
        // Frame complete — verify CRC16 on the full payload including CRC bytes
        uint16_t computed = QualcommCrc::calculate(m_buf.data(), m_idx);
        if (computed == 0x0F47) {
          // CRC valid (residue of CRC-16/X.25 is always 0x0F47)
          // Strip 2-byte CRC tail and deliver clean payload
          if (m_callback) m_callback({m_buf.data(), m_idx - 2});
          ++m_frames_ok;
        } else {
          ++m_frames_bad_crc;
        }
      }
      m_inside = true;
      m_idx = 0;
      m_escape = false;
      return;
    }

    if (!m_inside) return;

    if (byte == HdlcCodec::ESCAPE) {
      m_escape = true;
      return;
    }

    if (m_escape) {
      byte ^= HdlcCodec::ESCAPE_XOR;
      m_escape = false;
    }

    if (m_idx < m_buf.size()) { m_buf[m_idx++] = byte; }
  }
};

}  // namespace QCom
