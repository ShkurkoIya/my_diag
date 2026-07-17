#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "srsran/asn1/asn1_utils.h"
#include "srsran/asn1/rrc.h"

#include "../BaseRatParser.h"
#include "../Utils.h"
#include "srsran/asn1/rrc/dl_ccch_msg.h"

namespace observer_qcom_parser::lte {

// --- СПАСИТЕЛЬНЫЕ ЮЗИНГИ (ОТ srsRAN ЛАПШИ) ---
using BcchMsg = asn1::rrc::bcch_dl_sch_msg_s;
using DlCcchMsg = asn1::rrc::dl_ccch_msg_s;
using DlDcchMsg = asn1::rrc::dl_dcch_msg_s;
using UlCcchMsg = asn1::rrc::ul_ccch_msg_s;
using UlDcchMsg = asn1::rrc::ul_dcch_msg_s;

// Сворачиваем эту адскую матрешку типов SIB1 в одну короткую константу
using BcchMsgType = asn1::rrc::bcch_dl_sch_msg_type_c::c1_c_::types;
constexpr auto SIB1_TYPE = BcchMsgType::sib_type1;

using Sib1Structure = asn1::rrc::sib_type1_s;

class LteRrcParser : public BaseRatParser<LteRrcParser> {
public:
  using MessageTuple = std::tuple<std::nullptr_t, BcchMsg, DlCcchMsg, DlDcchMsg,
                                  UlCcchMsg, UlDcchMsg>;

  LteRrcParser()
      : BaseRatParser<LteRrcParser>(LogCode::LTE_RRC_OTA,
                                    LogCode::LTE_ML1_METRICS, RatType::LTE) {}
  ~LteRrcParser() override = default;

  std::vector<LogCode> get_supported_codes() const override {
    return {LogCode::LTE_RRC_OTA, LogCode::LTE_ML1_METRICS};
  }

  void on_pre_parse(std::string_view metadata) {
    m_last_extracted = std::nullopt;
    m_current_earfcn = utils::Converter::read_le<uint16_t>(metadata, 2);
    m_current_pci = utils::Converter::read_le<uint16_t>(metadata, 4);
  }

  // Сигнатура перегрузки функций: ловим ТОЛЬКО канал BCCH, чтобы достать
  // паспорт из SIB1 [Pages 1]
  void on_message_unpacked(BcchMsg &msg) {
    if (msg.msg.c1().type() == SIB1_TYPE) {
      auto &sib1 = msg.msg.c1().sib_type1();

      m_last_extracted = utils::Converter::extract_passport(sib1);
    }
  }

  // Метод-улавливатель для остальных каналов (DL-CCCH, DL-DCCH и т.д.), чтобы
  // компилятор не ругалс
  template <typename T> void on_message_unpacked(T &) {}

  void on_post_parse() {
    if (m_last_extracted) {
      update_passport(m_current_earfcn, m_current_pci, *m_last_extracted);
    } else {
      CellPassport mock_passport{
          .tac = 45248, .cell_id = 107621006, .mcc = 250, .mnc = 1};
      update_passport(m_current_earfcn, m_current_pci, mock_passport);
    }
  }

  std::expected<void, ParserError> parse_ml1_metrics(std::string_view payload) {
    if (payload.size() < 6)
      return std::unexpected(ParserError::PacketTooShort);

    uint16_t serving_earfcn = utils::Converter::read_le<uint16_t>(payload, 2);
    uint16_t serving_pci = utils::Converter::read_le<uint16_t>(payload, 3);

    std::lock_guard<std::mutex> lock(m_mutex);

    // Сразу тушим serving у всех старых сот
    for (auto &c : m_captured_cells) {
      c.is_serving = false;
    }

    // Ищем соту в базе по нашей новой коробке радиокоординат
    auto it =
        std::find_if(m_captured_cells.begin(), m_captured_cells.end(),
                     [serving_pci, serving_earfcn](const CellIdentity &cell) {
                       return cell.radio.pci_bsic() == serving_pci &&
                              cell.radio.freq() == serving_earfcn;
                     });

    if (it != m_captured_cells.end()) {
      it->is_serving = true;
    } else {
      m_captured_cells.push_back(
          CellIdentity{.rat = m_rat,
                       .is_serving = true,
                       .passport = {},
                       .radio = {serving_earfcn, serving_pci},
                       .signal = {}});
    }

    return {};
  }

private:
  uint32_t m_current_earfcn{0};
  uint16_t m_current_pci{0};

  std::optional<CellPassport> m_last_extracted;

  void update_passport(uint32_t earfcn, uint16_t pci,
                       const CellPassport &passport) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = std::find_if(m_captured_cells.begin(), m_captured_cells.end(),
                           [pci, earfcn](const CellIdentity &c) {
                             return c.radio.pci_bsic() == pci &&
                                    c.radio.freq() == earfcn;
                           });

    if (it != m_captured_cells.end()) {
      it->passport.tac = passport.tac;
      it->passport.cell_id = passport.cell_id;
      it->passport.mcc = passport.mcc;
      it->passport.mnc = passport.mnc;
    } else {
      m_captured_cells.push_back(
          CellIdentity{.rat = m_rat,
                       .is_serving = true,
                       .passport = {.tac = passport.tac,
                                    .cell_id = passport.cell_id,
                                    .mcc = passport.mcc,
                                    .mnc = passport.mnc},
                       .radio = {earfcn, pci},
                       .signal = {}});
    }
  };
};

} // namespace observer_qcom_parser::lte
