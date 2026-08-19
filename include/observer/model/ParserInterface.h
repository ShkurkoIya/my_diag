/// @file ParserInterface.h
/// @brief Abstract parser interface for RAT-specific DIAG log handlers.
#pragma once

#include <expected>
#include <string_view>
#include <vector>

#include <observer/model/Events.h>
#include <observer/model/Types.h>

namespace QCom {

/// @brief Interface for RAT-specific Qualcomm DIAG log parsers.
///
/// Implementations (LteParser, NrParser, etc.) register their supported
/// log codes and parse incoming packets into event batches.
class IRatParser {
public:
  virtual ~IRatParser() = default;

  [[nodiscard]] virtual std::expected<std::vector<Events::RrcEvent>, ParserError> parse(
      QualcommPacketView pkt) = 0;

  [[nodiscard]] virtual std::vector<LogCode> get_supported_codes() const = 0;
  [[nodiscard]] virtual std::string_view log_to_string(LogCode log_code) const noexcept = 0;
};

}  // namespace QCom
