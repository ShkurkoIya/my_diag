/// @file DiagCommands.h
/// @brief Qualcomm DIAG protocol command builders for modem configuration.
///
/// Builds HDLC-framed command packets for:
///   - Disabling all log masks (ZeroLog)
///   - Setting per-equipment log masks (LTE, GSM, WCDMA, NR)
///   - Retrieving supported log ID ranges (activates radio on SIMCom)
///
/// All mask values are verified on SIMCom SIM8300 (Qualcomm X55) hardware.
#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <span>
#include <thread>
#include <vector>

#include "transport/HdlcCodec.h"

namespace QCom {

/// DIAG protocol opcodes (3GPP TS 27.007 + Qualcomm proprietary)
namespace DiagOpcode {
constexpr uint8_t LOG_CONFIG = 0x73;
constexpr uint8_t EXT_MSG_CONFIG = 0x7D;
}  // namespace DiagOpcode

/// DIAG log configuration sub-operations
namespace DiagLogOp {
constexpr uint32_t DISABLE_ALL = 0;
constexpr uint32_t RETRIEVE_ID_RANGES = 1;
constexpr uint32_t SET_MASK = 3;
}  // namespace DiagLogOp

/// DIAG equipment IDs for log mask targeting
namespace DiagEquip {
constexpr uint32_t WCDMA = 0x04;
constexpr uint32_t GSM = 0x05;
constexpr uint32_t UMTS = 0x07;
constexpr uint32_t LTE = 0x0B;
constexpr uint32_t TDSCDMA = 0x0D;
}  // namespace DiagEquip

/// @brief Builds and sends DIAG modem configuration commands.
///
/// Requires a send function that accepts a ready-to-transmit HDLC frame.
/// Typical usage:
/// @code
///   DiagSession session([&](auto frame) { serial.write(frame); });
///   session.init_modem();
/// @endcode
class DiagSession {
public:
  using SendFn = std::function<bool(std::span<const uint8_t> frame)>;

  explicit DiagSession(SendFn send) : m_send(std::move(send)) {}

  /// Full modem initialization sequence: zero masks, set LTE+GSM, commit.
  bool init_modem() {
    if (!zero_log()) return false;
    if (!set_lte_mask()) return false;
    if (!set_gsm_mask()) return false;
    if (!commit_masks()) return false;
    return true;
  }

  /// Disable all log masks and silence F3 debug message flood.
  bool zero_log() {
    // 1. LOG_CONFIG_DISABLE_OP — clear all masks
    if (!send_log_config_op(DiagLogOp::DISABLE_ALL)) return false;
    delay(10);

    // 2. EXT_MSG_CONFIG — set all runtime masks to MSG_LVL_NONE
    uint8_t msg_payload[8] = {};
    msg_payload[0] = 4;  // MSG_EXT_SUBCMD_SET_ALL_RT_MASKS
    if (!send_cmd(DiagOpcode::EXT_MSG_CONFIG, {msg_payload, 7})) return false;
    delay(10);

    // 3. RETRIEVE_ID_RANGES — activates radio module on SIMCom hardware
    if (!send_log_config_op(DiagLogOp::RETRIEVE_ID_RANGES)) return false;
    delay(20);

    return true;
  }

  /// Set LTE log mask — enables RRC OTA, serving cell info, ML1 measurements.
  ///
  /// Enabled log codes:
  ///   0xB0C0 (RRC OTA), 0xB0C1 (MIB), 0xB0C2 (Serving Cell Info)
  ///   0xB0CD (RRC signaling)
  ///   0xB17F (ML1 serving meas), 0xB193 (ML1 meas response)
  ///   0xB197 (ML1 serving info)
  bool set_lte_mask() {
    uint8_t buf[128] = {};
    size_t idx = 3;

    write_le32(buf, idx, DiagLogOp::SET_MASK);
    idx += 4;
    write_le32(buf, idx, DiagEquip::LTE);
    idx += 4;
    buf[idx++] = 0x01;
    buf[idx++] = 0x02;

    idx += 26;  // alignment padding

    write_le32(buf, idx, 0x01);
    idx += 4;
    buf[idx++] = 0x0C;
    buf[idx++] = 0xFF;

    size_t mask_start = idx;
    // Bit positions verified on SIM8300 hardware:
    buf[mask_start + 24] = 0x07;  // 0xB0C0, 0xB0C1, 0xB0C2
    buf[mask_start + 25] = 0x20;  // 0xB0CD
    buf[mask_start + 47] = 0x80;  // 0xB17F
    buf[mask_start + 50] = 0x08;  // 0xB193
    buf[mask_start + 51] = 0x80;  // 0xB197
    idx += 35;

    return send_cmd(DiagOpcode::LOG_CONFIG, {buf, idx}) && (delay(10), true);
  }

  /// Set GSM log mask — enables RR signaling and cell info.
  bool set_gsm_mask() {
    uint8_t buf[200] = {};
    size_t idx = 3;

    write_le32(buf, idx, DiagLogOp::SET_MASK);
    idx += 4;
    write_le32(buf, idx, DiagEquip::GSM);
    idx += 4;
    buf[idx++] = 0x20;  // g1
    buf[idx++] = 0x04;  // g2

    idx += 39;          // padding
    buf[idx++] = 0x80;  // g3
    idx += 30;          // padding
    buf[idx++] = 0x40;  // g4
    idx += 63;          // padding

    return send_cmd(DiagOpcode::LOG_CONFIG, {buf, idx}) && (delay(10), true);
  }

  /// Commit configured masks — applies changes on the modem.
  bool commit_masks() { return send_log_config_op(DiagLogOp::RETRIEVE_ID_RANGES); }

private:
  SendFn m_send;

  bool send_cmd(uint8_t opcode, std::span<const uint8_t> payload) {
    auto frame = HdlcCodec::serialize(opcode, payload);
    return m_send({frame.data(), frame.size()});
  }

  bool send_log_config_op(uint32_t op) {
    uint8_t buf[8] = {};
    write_le32(buf, 3, op);
    return send_cmd(DiagOpcode::LOG_CONFIG, {buf, 7});
  }

  static void write_le32(uint8_t* dst, size_t offset, uint32_t val) noexcept {
    std::memcpy(dst + offset, &val, 4);
  }

  static void delay(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
};

}  // namespace QCom
