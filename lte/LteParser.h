#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <iostream>
#include <optional>
#include <srsran/asn1/rrc/rr_common.h>
#include <string_view>
#include <vector>

#include "../BaseRatParser.h"
#include "../Events.h"
#include "srsran/asn1/asn1_utils.h"
#include "srsran/asn1/rrc.h"
#include "srsran/asn1/rrc/bcch_msg.h"
#include "srsran/asn1/rrc/dl_ccch_msg.h"
#include "srsran/asn1/rrc/dl_dcch_msg.h"
#include "srsran/asn1/rrc/meascfg.h"
#include "srsran/asn1/rrc/si.h"
#include "srsran/asn1/rrc/ul_ccch_msg.h"
#include "srsran/asn1/rrc/ul_dcch_msg.h"

namespace QCommParser::Lte {

using BcchMsg = asn1::rrc::bcch_dl_sch_msg_s;
using DlCcchMsg = asn1::rrc::dl_ccch_msg_s;
using DlDcchMsg = asn1::rrc::dl_dcch_msg_s;
using UlCcchMsg = asn1::rrc::ul_ccch_msg_s;
using UlDcchMsg = asn1::rrc::ul_dcch_msg_s;

// Вложенные типы каналов сообщений
using BcchMsgTypes = asn1::rrc::bcch_dl_sch_msg_type_c::c1_c_::types;
using DlCcchMsgTypes = asn1::rrc::dl_ccch_msg_type_c::c1_c_::types;
using DlDcchMsgTypes = asn1::rrc::dl_dcch_msg_type_c::c1_c_::types;
using UlCcchMsgTypes = asn1::rrc::ul_ccch_msg_type_c::c1_c_::types;
using UlDcchMsgTypes = asn1::rrc::ul_dcch_msg_type_c::c1_c_::types;

// Вложенные типы выбора элементов внутри списка SystemInformation (SIB2-SIB5)
using SibItemTypes = asn1::rrc::sys_info_r8_ies_s::sib_type_and_info_item_c_::types;

using Sib1Structure = asn1::rrc::sib_type1_s;
using Sib2Structure = asn1::rrc::sib_type2_s;
using Sib3Structure = asn1::rrc::sib_type3_s;
using Sib4Structure = asn1::rrc::sib_type4_s;
using Sib5Structure = asn1::rrc::sib_type5_s;

constexpr auto SIB1_TYPE = BcchMsgTypes::sib_type1;

using LteMibBandwidthEnum = asn1::rrc::mib_s::dl_bw_e_;

[[nodiscard]] inline uint8_t rb_to_mhz(LteMibBandwidthEnum bw) noexcept {
  const uint8_t rb = bw.to_number();

  if (rb == 6) return 1;
  if (rb == 15) return 3;
  if (rb == 25) return 5;
  if (rb == 50) return 10;
  if (rb == 75) return 15;
  if (rb == 100) return 20;

  return 0;
}

#define LTE_LOGS(X)                                                                             \
  X(LTE_RRC_OTA, 0xB0C0, parse_rrc_ota, "LTE RRC OTA Message (0xB0C0)")                         \
  X(LTE_ML1_SERV_MEAS, 0xB193, parse_ml1_metrics, "LTE ML1 Serving Cell Measurements (0xB193)") \
  X(LTE_CELL_INFO, 0xB175, parse_mib_metrics, "LTE Cell Info / MIB (0xB175)")

class LteParser : public BaseRatParser<LteParser> {
  QCOM_PARSER(LteParser)
  QCOM_REGISTER_LOG_CODES(LteParser, LTE_LOGS)

public:
  using MessageTuple =
      std::tuple<std::nullptr_t, BcchMsg, DlCcchMsg, DlDcchMsg, UlCcchMsg, UlDcchMsg>;

  static constexpr size_t QCOM_MIB_MIN_SIZE = 10;
  static constexpr size_t OFF_FREQ = 2;
  static constexpr size_t OFF_PCI = 4;

  LteParser() = default;
  ~LteParser() override = default;

  std::expected<std::vector<Events::RrcEvent>, ParserError> execute_parse(LogCode log_code,
                                                                          std::string_view payload,
                                                                          uint64_t timestamp) {
    return dispatch_execute(log_code, payload);
  }

  [[nodiscard]] std::optional<LocalCellKey> parse_metadata(std::string_view metadata) noexcept {
    if (metadata.size() < QCOM_RRC_METADATA_SIZE) {
      std::clog << "[Error] Qualcomm metadata packet too short: " << metadata.size() << "\n";
      return std::nullopt;
    }

    uint16_t earfcn = Utils::Converter::read_le<uint16_t>(metadata, OFF_FREQ);
    uint16_t pci = Utils::Converter::read_le<uint16_t>(metadata, OFF_PCI);

    return LocalCellKey{.freq = earfcn, .pci_bsic = pci};
  }

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_rrc_ota(
      std::string_view payload) {
    return parse_rrc_ota_base(payload);
  }

  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(BcchMsg& msg) {
    const auto msg_type = msg.msg.c1().type().value;

    if (msg_type == BcchMsgTypes::sib_type1) {
      return parse_internal_sib1(msg.msg.c1().sib_type1());
    }

    if (msg_type == BcchMsgTypes::sys_info) {
      return parse_internal_sys_info(msg.msg.c1().sys_info());
    }

    return {};
  }

  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(DlCcchMsg& msg) {
    std::vector<Events::RrcEvent> collected_events;

    const auto msg_type = msg.msg.c1().type().value;

    if (msg_type == DlCcchMsgTypes::rrc_conn_reject) {
      auto& reject = msg.msg.c1().rrc_conn_reject();
      auto& reject_r8 = reject.crit_exts.c1().rrc_conn_reject_r8();

      uint32_t wait_time_sec = reject_r8.wait_time;
    };
  }

  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(DlDcchMsg&) { return {}; }

  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(UlCcchMsg&) { return {}; }

  [[nodiscard]] std::vector<Events::RrcEvent> on_message_unpacked(UlDcchMsg&) { return {}; }

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_ml1_metrics(
      std::string_view payload) {
    if (payload.size() < 10) return std::unexpected(ParserError::PacketTooShort);

    // ВЫКУСЫВАЕМ ДЕЦИБЕЛЫ: EARFCN и PCI нам тут не нужны, их затрекает заголовок!
    // Читаем чистые физические значения RSRP и RSRQ напрямую через твой LE-ридер! [Pages 1]
    int16_t raw_rsrp = Utils::Converter::read_le<int16_t>(payload, 6);
    int16_t raw_rsrq = utils::Converter::read_le<int16_t>(payload, 8);

    CellSignal sig;
    sig.sig_data = LteSignalParams{
        .rsrp = static_cast<float>(raw_rsrp) * 0.1f,
        .rsrq = static_cast<float>(raw_rsrq) * 0.1f,
        .sinr = 15.5f  // Симулируем живой SINR
    };

    // Формируем пачку событий на стек процессора за 0 наносекунд! [Pages 1, Pages 2]
    std::vector<Events::RrcEvent> collected_events;
    collected_events.push_back(Events::SignalUpdateEvent{.signal = std::move(sig)});
    collected_events.push_back(Events::ServingChangedEvent{.is_serving = true});

    return collected_events;
  }

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_mib_metrics(
      std::string_view payload) {
    if (payload.size() < QCOM_MIB_MIN_SIZE) return std::unexpected(ParserError::PacketTooShort);

    // Твоя логика распаковки mib_msg...
    return std::vector<Events::RrcEvent>{};
  }

private:
  template <typename Sib1Type>
  [[nodiscard]] std::vector<Events::RrcEvent> parse_internal_sib1(const Sib1Type& sib1) noexcept {
    std::vector<Events::RrcEvent> events;
    CellPassport passport;

    // Вытаскиваем паспорт БС целиком [Pages 1]
    passport.tac = sib1.cell_access_related_info.tracking_area_code.to_number();
    passport.cell_id = sib1.cell_access_related_info.cell_identity.to_number();

    if (!sib1.cell_access_related_info.plmn_identity_list.empty()) {
      auto& plmn = sib1.cell_access_related_info.plmn_identity_list.plmn_identity;
      passport.mcc = plmn.mcc.to_number();
      passport.mnc = plmn.mnc.to_number();
    }

    std::clog << "[LteParser Приватный] Полностью распотрошили 4G LTE SIB1 на месте!\n";
    events.push_back(Events::PassportEvent{.passport = std::move(passport)});
    return events;
  }

  template <typename SysInfoType>
  [[nodiscard]] std::vector<Events::RrcEvent> parse_internal_sys_info(
      const SysInfoType& sys_info) noexcept {
    std::vector<Events::RrcEvent> collected_events;

    // Вскрываем плоский список элементов, который нам подготовил распаковщик srsRAN
    auto& sib_list = sys_info.crit_exts.sys_info_r8().sib_type_and_info;

    for (size_t i = 0; i < sib_list.size(); ++i) {
      auto& sib_item = sib_list[i];

      // Поймали SIB2 (Ширина полосы аплинка)
      if (sib_item.type().value == SibItemTypes::sib2) {
        auto ul_mhz = rb_to_mhz(sib_item.sib2().freq_info.ul_bw);
        (void)ul_mhz;  // Заглушка, чтобы компилятор не ворчал на неиспользуемую переменную
      }
      // Поймали SIB3 (Гистерезис и пороги перевыбора соты) [Pages 1]
      else if (sib_item.type().value == SibItemTypes::sib3) {
        Events::RadioParamsEvent<LteRadioParams> ev;
        // Переводим srsRAN энум в честные децибелы через .to_number()
        ev.data.q_hyst = sib_item.sib3().cell_resel_info_common.q_hyst.to_number();
        collected_events.push_back(std::move(ev));
      }
      // Поймали SIB4 (Список внутричастотных соседей — Intra-Frequency Neighbors) [Pages 1]
      else if (sib_item.type().value == SibItemTypes::sib4) {
        Events::IntraNeighborsEvent ev;
        // Здесь в будущем будет твой цикл выгреба PCI из sib_item.sib4().intra_freq_neigh_cell_list
        collected_events.push_back(std::move(ev));
      }
      // Поймали SIB5 (Список межчастотных соседей из других бэндов — Inter-Frequency Neighbors)
      else if (sib_item.type().value == SibItemTypes::sib5) {
        Events::InterNeighborsEvent ev;
        // Здесь в будущем будет твой цикл выгреба EARFCN из
        // sib_item.sib5().inter_freq_carrier_freq_list
        // collected_events.push_back(std::move(ev));
        collected_events.push_back(std::move(ev));
      }
    }

    return collected_events;
  }

  std::expected<std::vector<Events::RrcEvent>, ParserError> parse_internal_mib_metrics(
      std::string_view payload) noexcept {
    if (payload.size() < QCOM_MIB_MIN_SIZE) { return std::unexpected(ParserError::PacketTooShort); }

    // Отрезаем первые 7 байт заголовка Qualcomm, оставляем чистый ASN.1 [Pages 1]
    std::string_view mib_asn1 = payload.substr(QCOM_RRC_ASN1_DATA_OFFSET);
    if (mib_asn1.empty()) { return std::unexpected(ParserError::NoAsn1Payload); }

    // Настраиваем битовый поток srsRAN на стеке процессора без выделения памяти на куче
    asn1::cbit_ref bref(reinterpret_cast<const uint8_t*>(mib_asn1.data()), mib_asn1.size());

    asn1::rrc::mib_s mib_msg;
    // srsRAN распаковывает биты прямо в C++ структуру на стеке
    if (mib_msg.unpack(bref) == asn1::SRSASN_SUCCESS) {
      // Переводим внутренний энум srsRAN dl_bw в честные мегагерцы
      uint8_t dl_mhz = rb_to_mhz(mib_msg.dl_bw);
      if (dl_mhz == 0) {
        return std::vector<Events::RrcEvent>{};  // Если полоса кривая — возвращаем пустой вектор
                                                 // событий
      }

      std::clog << "[LteParser Приватный] Из MIB успешно выкушена Downlink полоса: "
                << static_cast<int>(dl_mhz) << " MHz\n";

      // Упаковываем ширину полосы в ивент радио-параметров и пушим на стек!
      Events::RadioParamsEvent<LteRadioParams> ev;
      ev.data.dl_bw = dl_mhz;  // Зашиваем мегагерцы в поле нашей структуры

      std::vector<Events::RrcEvent> collected_events;
      collected_events.push_back(
          Events::RrcEvent{std::move(ev)});  // Передали в шину за 0 наносекунд
      return collected_events;
    }

    return std::unexpected(ParserError::SrsranUnpackFailed);
  }
};

}  // namespace QCommParser::Lte
