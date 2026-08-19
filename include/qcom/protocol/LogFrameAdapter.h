/// @file LogFrameAdapter.h
/// @brief Shared LOG_F / DCI payload → QualcommPacketView (no OS deps).
///
/// Linux: after HDLC deframe or USB length-prefix strip, body still has LOG_F header.
/// Android DCI: often delivers the same log body (cmd 0x10…) or already-stripped
/// log_code+payload — call the matching helper.
#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include <observer/model/Types.h>
#include <observer/model/Utils.h>

namespace QCom {

namespace DiagWire {
constexpr uint8_t kLogF = 0x10;
/// Journal / simplified dumps: cmd, pad, len, code, ts8 → payload @14
constexpr size_t kLogFHeaderShort = 14;
/// Classic DIAG: cmd, more, len, log_len, code, ts8 → payload @16
constexpr size_t kLogFHeaderClassic = 16;
}  // namespace DiagWire

/// Adapt a DIAG LOG_F buffer (short journal layout or classic duplicate-len layout).
[[nodiscard]] inline std::optional<QualcommPacketView> adapt_log_f_frame(
    std::span<const uint8_t> raw) noexcept {
  if (raw.size() < DiagWire::kLogFHeaderShort) return std::nullopt;
  if (raw[0] != DiagWire::kLogF) return std::nullopt;

  const uint16_t outer_len = Utils::Converter::read_le<uint16_t>(raw.data(), 2);
  const uint16_t field4 = Utils::Converter::read_le<uint16_t>(raw.data(), 4);

  // USB DIAG often pads the container (e.g. 400 bytes) while outer_len is the real
  // LOG_F size. Qualcomm `len` (offset 2) is the byte count AFTER cmd+more+len
  // (osmo-qcdiag diag_log_hdr) — i.e. total_frame = len + 4.
  // Some dumps encode len as the full frame size from byte 0; prefer +4 unless the
  // buffer is an exact len-sized frame (unit tests / pre-clipped bodies).
  auto clip = [&](size_t header) -> std::span<const uint8_t> {
    const size_t end_as_full = outer_len;      // len == size from byte 0
    const size_t end_as_data = outer_len + 4;  // len == size after 4-byte hdr
    size_t total = raw.size();
    const bool full_ok = end_as_full >= header && end_as_full <= raw.size();
    const bool data_ok = end_as_data >= header && end_as_data <= raw.size();
    if (full_ok && data_ok) {
      if (raw.size() == end_as_full)
        total = end_as_full;  // exact pre-sized frame
      else
        total = end_as_data;  // padded USB: avoid systematic 4-byte truncate
    } else if (data_ok) {
      total = end_as_data;
    } else if (full_ok) {
      total = end_as_full;
    }
    return raw.subspan(0, total).subspan(header);
  };

  // Classic wire: [len][len][code][ts8][payload] — field4 duplicates outer_len
  if (raw.size() >= DiagWire::kLogFHeaderClassic && field4 == outer_len) {
    const uint16_t log_code = Utils::Converter::read_le<uint16_t>(raw.data(), 6);
    const uint64_t timestamp = Utils::Converter::read_le<uint64_t>(raw.data(), 8);
    auto payload = clip(DiagWire::kLogFHeaderClassic);
    if (payload.empty()) return std::nullopt;
    return QualcommPacketView{
        .log_code = log_code,
        .timestamp = timestamp,
        .payload = payload,
    };
  }

  // Short / journal: [code][ts8][payload]
  const uint16_t log_code = field4;
  const uint64_t timestamp = Utils::Converter::read_le<uint64_t>(raw.data(), 6);
  auto payload = clip(DiagWire::kLogFHeaderShort);
  if (payload.empty()) return std::nullopt;
  return QualcommPacketView{
      .log_code = log_code,
      .timestamp = timestamp,
      .payload = payload,
  };
}

/// Adapt already-split DCI-style delivery (log_code + payload only).
[[nodiscard]] inline QualcommPacketView adapt_dci_log(LogCode log_code, uint64_t timestamp,
                                                      std::span<const uint8_t> payload) noexcept {
  return QualcommPacketView{
      .log_code = log_code,
      .timestamp = timestamp,
      .payload = payload,
  };
}

}  // namespace QCom
