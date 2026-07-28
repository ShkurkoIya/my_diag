#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include "Events.h"

namespace QCommParser {

using LogCode = uint16_t;

struct alignas(8) QualcommPacketView {
  LogCode log_code{0};
  uint64_t timestamp{0};
  std::string_view payload;
};

enum class ChannelType : uint8_t {
  BCCH_DL_SCH = 1,  // Системная информация / SIB
  DL_CCCH = 2,      // Контроль соты (Downlink)
  DL_DCCH = 3,      // Выделенные команды (Handover, Reconfig)
  UL_CCCH = 4,      // Контроль соты (Uplink)
  UL_DCCH = 5,      // Выделенные отчеты модема (MeasurementReport)
  UNKNOWN = 0
};

inline ChannelType to_channel_type(uint8_t raw) {
  if (raw >= 1 && raw <= 5) { return static_cast<ChannelType>(raw); }
  return ChannelType::UNKNOWN;
}

enum class ParserError {
  PacketTooShort,
  WrongLogCode,
  NoAsn1Payload,
  UnknownChannelType,
  SrsranUnpackFailed
};

inline std::string to_string(ParserError err) {
  switch (err) {
    case ParserError::PacketTooShort: return "Packet too short";
    case ParserError::WrongLogCode: return "Wrong Qualcomm Log Code";
    case ParserError::NoAsn1Payload: return "No ASN1 payload found";
    case ParserError::UnknownChannelType: return "Unknown 3GPP channel type";
    case ParserError::SrsranUnpackFailed: return "srsRAN ASN1 unpack failed";
  }
  return "Unknown error";
}

class IRatParser {
public:
  virtual ~IRatParser() = default;

  virtual std::expected<std::vector<Events::RrcEvent>, ParserError> parse(LogCode log_code,
                                                                          std::string_view payload,
                                                                          uint64_t timestamp) = 0;
  virtual std::vector<LogCode> get_supported_codes() const = 0;
  virtual std::string_view log_to_string(LogCode log_code) const noexcept = 0;
};

}  // namespace QCommParser
