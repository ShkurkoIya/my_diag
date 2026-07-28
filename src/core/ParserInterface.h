#pragma once

#include <cstdint>
#include <cstdio>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "core/Events.h"

namespace QCom {

using LogCode = uint16_t;

struct QualcommPacketView {
  LogCode log_code{0};
  uint64_t timestamp{0};
  std::string_view payload;
};

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

enum class ParserError {
  PacketTooShort,
  WrongLogCode,
  NoAsn1Payload,
  UnknownChannelType,
  SrsranUnpackFailed,
  NotImplemented
};

inline std::string to_string(ParserError err) {
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

inline std::string to_string(LogCode code) {
  char buf[16];
  snprintf(buf, sizeof(buf), "0x%04X", code);
  return buf;
}

class IRatParser {
public:
  virtual ~IRatParser() = default;

  virtual std::expected<std::vector<Events::RrcEvent>, ParserError> parse(
      QualcommPacketView pkt) = 0;

  virtual std::vector<LogCode> get_supported_codes() const = 0;
  virtual std::string_view log_to_string(LogCode log_code) const noexcept = 0;
};

}  // namespace QCom
