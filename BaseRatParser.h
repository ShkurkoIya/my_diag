#pragma once

#include <cstdint>
#include <expected>
#include <string_view>
#include <tuple>

#include "Events.h"
#include "ParserInterface.h"
#include "Utils.h"
#include "srsran/asn1/asn1_utils.h"

namespace QCommParser {

template <typename Derived>
using ParserSlotPtr =
    std::expected<std::vector<Events::RrcEvent>, ParserError> (Derived::*)(std::string_view);

template <typename Derived>
struct LogCodeMapping {
  LogCode code;
  ParserSlotPtr<Derived> slot_ptr;
  std::string_view name;
};

template <typename T>
struct ParserTraits;

template <typename T>
concept ValidRatParserDerived = requires {
  // 1. ПРОВЕРКА СЛОЯ ТИПОВ:
  typename ParserTraits<T>::MessageTuple;  // Обязан быть srsRAN кортеж!

  // 2. ПРОВЕРКА СЛОЯ МЕТОДОВ (КОНТРАКТ ПЛAГИНА):
  // База жестко требует, чтобы плагин умел выкусывать частоту и PCI из 7 байт Qualcomm!
  // { parser.parse_metadata(metadata) } -> std::same_as<std::optional<LocalCellKey>>;

  // автоматически и гарантированно генерирует наш макрос QCOM_REGISTER_LOG_CODES!
};
template <typename Derived>
  requires ValidRatParserDerived<Derived>
class BaseRatParser : public IRatParser {
public:
  // Первые 7 байт — это всегда метаданные чипсета (EARFCN, PCI, Системные флаги)
  static constexpr size_t QCOM_RRC_METADATA_SIZE = 7;
  // Тип логического 3GPP канала (BCCH, DL-CCCH и т.д.) лежит строго на 7-м байте (индекс 6)
  static constexpr size_t QCOM_RRC_CHANNEL_TYPE_OFFSET = 6;
  // Чистый бинарный ASN.1 payload для srsRAN начинается сразу за метаданными
  static constexpr size_t QCOM_RRC_ASN1_DATA_OFFSET = 7;

protected:
  BaseRatParser() = default;
  virtual ~BaseRatParser() = default;

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_rrc_ota_base(
      std::string_view payload) {
    if (payload.size() < QCOM_RRC_METADATA_SIZE) {
      return std::unexpected(ParserError::PacketTooShort);
    }

    // Выкусываем тип канала из первого байта payload Qualcomm
    ChannelType channel =
        to_channel_type(Utils::Converter::read_le<uint8_t>(payload, QCOM_RRC_CHANNEL_TYPE_OFFSET));

    if (channel == ChannelType::UNKNOWN) {
      return std::unexpected(ParserError::UnknownChannelType);
    }

    std::string_view asn1_data = payload.substr(QCOM_RRC_ASN1_DATA_OFFSET);
    if (asn1_data.empty()) { return std::unexpected(ParserError::NoAsn1Payload); }

    asn1::cbit_ref bref(reinterpret_cast<const uint8_t*>(asn1_data.data()), asn1_data.size());

    // Просим наследника проверить заголовок (выкусить Freq/PCI для валидации пакета)
    auto radio_opt =
        static_cast<Derived*>(this)->parse_metadata(payload.substr(0, QCOM_RRC_METADATA_SIZE));
    if (!radio_opt.has_value()) { return std::unexpected(ParserError::PacketTooShort); }

    return dispatch_unpack(channel, bref);
  }

private:
  template <size_t Index = 1>
  std::vector<Events::RrcEvent> dispatch_unpack(ChannelType channel, asn1::cbit_ref& bref) {
    using TupleType = typename Derived::MessageTuple;

    if constexpr (Index >= std::tuple_size_v<TupleType>) {
      return {};
    } else {
      if (static_cast<uint8_t>(channel) == Index) {
        using MsgType = std::tuple_element_t<Index, TupleType>;
        MsgType msg;

        // srsRAN распаковывает биты прямо в C++ структуру на стеке
        if (msg.unpack(bref) == asn1::SRSASN_SUCCESS) {
          // Мгновенно отдаем структуру наследнику (LteParser), собираем события и летим наверх
          return static_cast<Derived*>(this)->on_message_unpacked(msg);
        }
        return {};
      }
      return dispatch_unpack<Index + 1>(channel, bref);
    }
  }
};

// =========================================================================
// Вспомогательные макросы для использования в наследниках BaseRatParser<Derived>
// =========================================================================

#define QCOM_MAP_ENUM(name, val, slot, desc) name = val,
#define QCOM_MAP_VECTOR(name, val, slot, desc) val,
#define QCOM_MAP_ARRAY(name, val, slot, desc) LogCodeMapping<Derived>{val, &Derived::slot, desc},

// =========================================================================
// МАКРОС 1: СЛУЖЕБНАЯ ОБВЯЗКА (Вставляется в начале класса наследника)
// =========================================================================
#define QCOM_PARSER(ClassName)                                                               \
private:                                                                                     \
  static_assert(std::is_base_of_v<QCommParser::BaseRatParser<ClassName>, ClassName>,         \
                "🛑 БРО: Твой класс должен наследоваться от BaseRatParser!");                \
                                                                                             \
public:                                                                                      \
  std::expected<std::vector<QCommParser::Events::RrcEvent>, QCommParser::ParserError> parse( \
      QCommParser::QualcommPacketView pkt) override {                                        \
    return dispatch_execute<ClassName>(pkt.log_code, pkt.payload);                           \
  }
// =========================================================================
// МАКРОС 2: ДЕКЛАРАТИВНЫЙ РЕГИСТРАТОР КОДОВ И СЛOТОВ
// =========================================================================
#define QCOM_REGISTER_LOG_CODES(ClassName, LogListMacro)                                    \
public:                                                                                     \
  /* 🤖 Авто-генерация локального анонимного энума */                                       \
  enum { LogListMacro(QCOM_MAP_ENUM) };                                                     \
                                                                                            \
  /* Авто-список лог кодов */                                                               \
  std::vector<QCommParser::LogCode> get_supported_codes() const override {                  \
    return {LogListMacro(QCOM_MAP_VECTOR)};                                                 \
  }                                                                                         \
                                                                                            \
  template <typename Derived>                                                               \
  static constexpr auto get_local_log_map() noexcept {                                      \
    constexpr std::array<LocalLogCodeMapping<Derived>,                                      \
                         []() constexpr {                                                   \
                           size_t count = 0;                                                \
                           return (LogListMacro(++count - count +) 0);                      \
                         }()>                                                               \
        map_data{{LogListMacro(QCOM_MAP_ARRAY)}};                                           \
    return map_data;                                                                        \
  }                                                                                         \
                                                                                            \
  [[nodiscard]] bool can_handle(QCommParser::LogCode log_code) const noexcept {             \
    constexpr auto map_data = get_local_log_map<ClassName>();                               \
    return std::any_of(map_data.begin(), map_data.end(),                                    \
                       [log_code](const auto& item) { return item.code == log_code; });     \
  }                                                                                         \
                                                                                            \
  std::string_view log_to_string(QCommParser::LogCode log_code) const noexcept override {   \
    constexpr auto map_data = get_local_log_map<ClassName>();                               \
    auto it = std::find_if(map_data.begin(), map_data.end(),                                \
                           [log_code](const auto& item) { return item.code == log_code; }); \
    if (it != map_data.end()) { return it->name; }                                          \
    return "Unknown Log Code for " #ClassName;                                              \
  }                                                                                         \
                                                                                            \
  /* Табличный маршрутизатор указателей на методы */                                        \
  template <typename Derived>                                                               \
  std::expected<std::vector<QCommParser::Events::RrcEvent>, QCommParser::ParserError>       \
  dispatch_execute(QCommParser::LogCode log_code, std::string_view payload) {               \
    constexpr auto map_data = get_local_log_map<Derived>();                                 \
    auto it = std::find_if(map_data.begin(), map_data.end(),                                \
                           [log_code](const auto& item) { return item.code == log_code; }); \
    if (it != map_data.end()) {                                                             \
      auto slot = it->slot_ptr;                                                             \
      return (static_cast<Derived*>(this)->*slot)(payload);                                 \
    }                                                                                       \
    return std::unexpected(QCommParser::ParserError::WrongLogCode);                         \
  }

}  // namespace QCommParser
