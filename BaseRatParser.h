#include <cstdint>
#include <expected>
#include <mutex>
#include <string_view>
#include <tuple>
#include <vector>

#include "CellIdentity.h"
#include "ParserInterface.h"
#include "srsran/asn1/asn1_utils.h"

namespace observer_qcom_parser {
template <typename Derived> class BaseRatParser : public IRatParser {
public:
  std::expected<void, ParserError> parse(LogCode log_code,
                                         std::string_view payload,
                                         uint64_t timestamp) override {
    if (log_code == m_rrc_code) {
      return parse_rrc_ota(payload);
    } else if (log_code == m_ml1_code) {
      return static_cast<Derived *>(this)->parse_ml1_metrics(payload);
    }
    return std::unexpected(ParserError::WrongLogCode);
  }

  std::vector<CellIdentity> get_cells() const override {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_captured_cells;
  }

protected:
  BaseRatParser(LogCode rrc_code, LogCode ml1_code, RatType rat)
      : m_rrc_code(rrc_code), m_ml1_code(ml1_code), m_rat(rat) {}

  mutable std::mutex m_mutex;
  std::vector<CellIdentity> m_captured_cells;
  RatType m_rat;

private:
  LogCode m_rrc_code;
  LogCode m_ml1_code;

  template <size_t Index = 1>
  bool dispatch_unpack(ChannelType channel, asn1::cbit_ref &bref,
                       asn1::json_writer &j_writer) {
    using TupleType = typename Derived::MessageTuple;

    if constexpr (Index >= std::tuple_size_v<TupleType>) {
      return false;
    } else {
      if (static_cast<uint8_t>(channel) == Index) {
        using MsgType = std::tuple_element<Index, TupleType>;
        MsgType msg;

        if (msg.unpack(bref) == asn1::SRSASN_SUCCESS) {
          msg.to_json(j_writer);

          // Даем дочернему классу шанс залезть внутрь (например, выковырять
          // SIB1)
          static_cast<Derived *>(this)->on_message_unpacked(msg);
          return true;
        }
        return false;
      }
      return dispatch_unpack<Index + 1>(channel, bref, j_writer);
    }
  }

  std::expected<void, ParserError> parse_rrc_ota(std::string_view payload) {
    if (payload.size() < 7)
      return std::unexpected(ParserError::PacketTooShort);

    ChannelType channel = to_channel_type(static_cast<uint8_t>(payload[0]));
    std::string_view asn1_data = payload.substr(7);

    if (asn1_data.empty())
      return std::unexpected(ParserError::NoAsn1Payload);

    asn1::cbit_ref bref(reinterpret_cast<const uint8_t *>(asn1_data.data()),
                        asn1_data.size());

    asn1::json_writer j_writer;

    // Даем наследнику выкусить свою физику (EARFCN/PCI) из первых 7 байт
    // заголовка Qualcomm
    static_cast<Derived *>(this)->on_pre_parse(payload.substr(0, 7));

    if (!dispatch_unpack(channel, bref, j_writer)) {
      return std::unexpected(ParserError::SrsranUnpackFailed);
    }

    static_cast<Derived *>(this)->on_post_parse();

    return {};
  }
};

} // namespace observer_qcom_parser
