/// @file DiagSerialDemux.h
/// @brief Demux mixed DIAG tty stream: HDLC command responses + length-prefixed logs.
///
/// On SIMCom USB DIAG (option/ttyUSB), mask ACKs are HDLC (`… 0x7E`), while unsolicited
/// LOG_F traffic is often:
///   [u32 le total_len][u32 le type=1][body…]
/// where body starts with LOG_F (0x10). Both must be handled for a live pipeline.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <vector>

#include <qcom/protocol/HdlcCodec.h>
#include <qcom/protocol/LogFrameAdapter.h>

namespace QCom {

class DiagSerialDemux {
public:
  static constexpr size_t npos = static_cast<size_t>(-1);

  using LogCallback = std::function<void(QualcommPacketView pkt)>;

  void set_log_callback(LogCallback cb) { m_on_log = std::move(cb); }

  void reset() noexcept {
    m_buf.clear();
    m_hdlc.reset();
    m_hdlc_cb_set = false;
    m_msgs_ok = 0;
    m_hdlc_ok = 0;
    m_hdlc_bad = 0;
    m_logs = 0;
  }

  /// Clear only the byte buffer (keep counters across stop/start diagnostics).
  void clear_buffer() noexcept {
    m_buf.clear();
    m_hdlc.reset();
    m_hdlc_cb_set = false;
  }

  void feed(std::span<const uint8_t> data) {
    m_buf.insert(m_buf.end(), data.begin(), data.end());
    pump();
  }

  [[nodiscard]] uint64_t messages_ok() const noexcept { return m_msgs_ok; }
  [[nodiscard]] uint64_t hdlc_ok() const noexcept { return m_hdlc_ok; }
  [[nodiscard]] uint64_t hdlc_bad_crc() const noexcept { return m_hdlc_bad; }
  [[nodiscard]] uint64_t logs_delivered() const noexcept { return m_logs; }

private:
  std::vector<uint8_t> m_buf;
  HdlcDeframer m_hdlc;
  LogCallback m_on_log;
  uint64_t m_msgs_ok{0};
  uint64_t m_hdlc_ok{0};
  uint64_t m_hdlc_bad{0};
  uint64_t m_logs{0};
  bool m_hdlc_cb_set{false};

  void ensure_hdlc_cb() {
    if (m_hdlc_cb_set) return;
    m_hdlc.set_callback([this](std::span<const uint8_t> payload) { emit_if_log(payload); });
    m_hdlc_cb_set = true;
  }

  void emit_if_log(std::span<const uint8_t> raw) {
    if (auto pkt = adapt_log_f_frame(raw)) {
      ++m_logs;
      if (m_on_log) m_on_log(*pkt);
    }
  }

  void pump() {
    ensure_hdlc_cb();
    size_t off = 0;
    while (off < m_buf.size()) {
      if (m_buf[off] == HdlcCodec::FLAG) {
        if (!consume_hdlc(off)) break;
        continue;
      }

      // Length-prefixed USB DIAG message: [u32 len][u32 type][body]
      if (m_buf.size() - off < 8) break;
      uint32_t total_len = 0;
      uint32_t type = 0;
      std::memcpy(&total_len, m_buf.data() + off, 4);
      std::memcpy(&type, m_buf.data() + off + 4, 4);

      const bool plausible =
          total_len >= 8 && total_len <= 65536 && type >= 1 && type <= 8;
      if (plausible) {
        if (m_buf.size() - off < total_len) break;
        auto body = std::span<const uint8_t>{m_buf.data() + off + 8, total_len - 8};
        emit_if_log(body);
        ++m_msgs_ok;
        off += total_len;
        continue;
      }

      // HDLC response without leading FLAG (opcode … CRC FLAG)
      if (is_diag_opcode(m_buf[off])) {
        const size_t end = find_flag(off + 1);
        if (end == npos) break;
        // Avoid eating length-prefixed payload: FLAG must be close
        if (end - off > 512) {
          ++off;
          continue;
        }
        const uint8_t lead = HdlcCodec::FLAG;
        const uint64_t ok0 = m_hdlc.frames_ok();
        const uint64_t bad0 = m_hdlc.frames_bad_crc();
        m_hdlc.feed({&lead, 1});
        m_hdlc.feed({m_buf.data() + off, end - off + 1});
        m_hdlc_ok += m_hdlc.frames_ok() - ok0;
        m_hdlc_bad += m_hdlc.frames_bad_crc() - bad0;
        off = end + 1;
        continue;
      }

      ++off;  // resync
    }

    if (off > 0) {
      m_buf.erase(m_buf.begin(), m_buf.begin() + static_cast<std::ptrdiff_t>(off));
    }
  }

  bool consume_hdlc(size_t& off) {
    const size_t end = find_flag(off + 1);
    if (end == npos) return false;
    const uint64_t ok0 = m_hdlc.frames_ok();
    const uint64_t bad0 = m_hdlc.frames_bad_crc();
    m_hdlc.feed({m_buf.data() + off, end - off + 1});
    m_hdlc_ok += m_hdlc.frames_ok() - ok0;
    m_hdlc_bad += m_hdlc.frames_bad_crc() - bad0;
    off = end + 1;
    return true;
  }

  [[nodiscard]] static bool is_diag_opcode(uint8_t op) noexcept {
    return op == 0x73 || op == 0x7D || op == 0x13 || op == 0x4B || op == 0x60;
  }

  [[nodiscard]] size_t find_flag(size_t from) const noexcept {
    for (size_t i = from; i < m_buf.size(); ++i) {
      if (m_buf[i] == HdlcCodec::FLAG) return i;
    }
    return npos;
  }
};

}  // namespace QCom
