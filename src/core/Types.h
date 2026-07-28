/// @file Types.h
/// @brief Core types shared across all layers: LogCode, PacketView, ParserError.
///
/// This header has ZERO internal dependencies — only standard library.
/// It defines the vocabulary types that flow between parser, protocol, and transport.
#pragma once

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace QCom {

/// Qualcomm DIAG log code (e.g. 0xB0C0 for LTE RRC OTA)
using LogCode = uint16_t;

/// @brief Non-owning view of a Qualcomm DIAG log packet.
///
/// Created by the transport layer after HDLC deframing and DIAG header parsing.
/// Consumed by parsers. The payload span is valid only during the callback scope.
struct QualcommPacketView {
  LogCode log_code{0};
  uint64_t timestamp{0};
  std::span<const uint8_t> payload;
};

/// 3GPP logical channel type for RRC OTA messages
enum class ChannelType : uint8_t {
  BCCH_DL_SCH = 1,
  DL_CCCH = 2,
  DL_DCCH = 3,
  UL_CCCH = 4,
  UL_DCCH = 5,
  UNKNOWN = 0
};

[[nodiscard]] inline constexpr ChannelType to_channel_type(uint8_t raw) noexcept {
  if (raw >= 1 && raw <= 5) return static_cast<ChannelType>(raw);
  return ChannelType::UNKNOWN;
}

/// Parser error codes
enum class ParserError : uint8_t {
  PacketTooShort,
  WrongLogCode,
  NoAsn1Payload,
  UnknownChannelType,
  SrsranUnpackFailed,
  NotImplemented
};

[[nodiscard]] inline std::string_view to_string(ParserError err) noexcept {
  switch (err) {
    case ParserError::PacketTooShort: return "Packet too short";
    case ParserError::WrongLogCode: return "Wrong Qualcomm Log Code";
    case ParserError::NoAsn1Payload: return "No ASN1 payload found";
    case ParserError::UnknownChannelType: return "Unknown 3GPP channel type";
    case ParserError::SrsranUnpackFailed: return "srsRAN ASN1 unpack failed";
    case ParserError::NotImplemented: return "Not implemented";
  }
  return "Unknown error";
}

[[nodiscard]] inline std::string to_string(LogCode code) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%04X", code);
  return buf;
}

}  // namespace QCom
