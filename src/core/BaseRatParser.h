#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>
#include <string_view>

#include "core/Events.h"
#include "core/ParserInterface.h"
#include "core/Utils.h"
#include "srsran/asn1/asn1_utils.h"

namespace QCom {

// Dispatch table entry: maps a Qualcomm log code to a parser method
template <typename Derived>
struct LogEntry {
  LogCode code;
  std::expected<std::vector<Events::RrcEvent>, ParserError> (Derived::*handler)(
      std::string_view payload);
  std::string_view name;
};

// Trait that each RAT parser must specialize to declare its ASN.1 message tuple
template <typename T>
struct ParserTraits;

template <typename Derived>
class BaseRatParser : public IRatParser {
public:
  // Qualcomm RRC OTA header layout
  static constexpr size_t QCOM_RRC_METADATA_SIZE = 7;
  static constexpr size_t QCOM_RRC_CHANNEL_TYPE_OFFSET = 6;
  static constexpr size_t QCOM_RRC_ASN1_DATA_OFFSET = 7;

protected:
  BaseRatParser() = default;
  ~BaseRatParser() override = default;

public:
  // --- Virtual contract from IRatParser ---

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse(QualcommPacketView pkt) override {
    return dispatch_to_handler(pkt.log_code, pkt.payload);
  }

  std::vector<LogCode> get_supported_codes() const override {
    constexpr auto& table = Derived::kLogTable;
    std::vector<LogCode> codes;
    codes.reserve(table.size());
    for (const auto& entry : table) codes.push_back(entry.code);
    return codes;
  }

  std::string_view log_to_string(LogCode log_code) const noexcept override {
    constexpr auto& table = Derived::kLogTable;
    auto it = std::find_if(table.begin(), table.end(),
                           [log_code](const auto& e) { return e.code == log_code; });
    if (it != table.end()) return it->name;
    return "Unknown";
  }

  // --- RRC OTA base parsing (shared by LTE and NR) ---

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_rrc_ota_base(
      std::string_view payload) {
    if (payload.size() < QCOM_RRC_METADATA_SIZE) {
      return std::unexpected(ParserError::PacketTooShort);
    }

    ChannelType channel =
        to_channel_type(Utils::Converter::read_le<uint8_t>(payload, QCOM_RRC_CHANNEL_TYPE_OFFSET));
    if (channel == ChannelType::UNKNOWN) {
      return std::unexpected(ParserError::UnknownChannelType);
    }

    std::string_view asn1_data = payload.substr(QCOM_RRC_ASN1_DATA_OFFSET);
    if (asn1_data.empty()) return std::unexpected(ParserError::NoAsn1Payload);

    auto radio_opt =
        static_cast<Derived*>(this)->parse_metadata(payload.substr(0, QCOM_RRC_METADATA_SIZE));
    if (!radio_opt.has_value()) return std::unexpected(ParserError::PacketTooShort);

    asn1::cbit_ref bref(reinterpret_cast<const uint8_t*>(asn1_data.data()), asn1_data.size());
    return dispatch_unpack(channel, bref);
  }

private:
  // Compile-time recursive dispatch: match ChannelType index to MessageTuple element
  template <size_t Index = 1>
  std::vector<Events::RrcEvent> dispatch_unpack(ChannelType channel, asn1::cbit_ref& bref) {
    using TupleType = typename ParserTraits<Derived>::MessageTuple;

    if constexpr (Index >= std::tuple_size_v<TupleType>) {
      return {};
    } else {
      if (static_cast<uint8_t>(channel) == Index) {
        using MsgType = std::tuple_element_t<Index, TupleType>;
        MsgType msg;
        if (msg.unpack(bref) == asn1::SRSASN_SUCCESS) {
          return static_cast<Derived*>(this)->on_message_unpacked(msg);
        }
        return {};
      }
      return dispatch_unpack<Index + 1>(channel, bref);
    }
  }

  // Dispatch log code to the correct handler via the static table
  std::expected<std::vector<Events::RrcEvent>, ParserError> dispatch_to_handler(
      LogCode log_code, std::string_view payload) {
    constexpr auto& table = Derived::kLogTable;
    auto it = std::find_if(table.begin(), table.end(),
                           [log_code](const auto& e) { return e.code == log_code; });
    if (it != table.end()) {
      auto handler = it->handler;
      return (static_cast<Derived*>(this)->*handler)(payload);
    }
    return std::unexpected(ParserError::WrongLogCode);
  }
};

}  // namespace QCom
