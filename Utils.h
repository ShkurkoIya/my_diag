#include <bit>
#include <cstdint>

#include "CellIdentity.h"
// #include "lte/LteRrcParser.h"
#include "lte/LteParser.h"
#include "srsran/asn1/rrc.h"
#include "srsran/asn1/rrc/common.h"
#include "srsran/asn1/rrc/si.h"

namespace QCommParser::Utils {

class Converter {
public:
  template <typename T>
  [[nodiscard]] static constexpr T read_le(std::string_view data, size_t offset) noexcept {
    if (offset + sizeof(T) > data.size()) return 0;

    return std::bit_cast<T>(
        *reinterpret_cast<const typename std::array<char, sizeof(T)>*>(data.data() + offset));
  }

  template <typename Container>
  [[nodiscard]] static constexpr uint16_t digits_to_number(const Container& digits) noexcept {
    uint16_t result = 0;

    for (size_t i = 0; i < digits.size(); ++i) { result = result * 10 + digits[i]; }

    return result;
  }

  [[nodiscard]] static CellPassport extract_passport(const asn1::rrc::sib_type1_s& sib1) noexcept {
    uint16_t parsed_mcc = 0;
    uint16_t parsed_mnc = 0;

    const auto& info = sib1.cell_access_related_info;

    if (info.plmn_id_list.size() > 0) {
      const auto& primary_plmn = info.plmn_id_list[0].plmn_id;

      if (primary_plmn.mcc_present) { parsed_mcc = digits_to_number(primary_plmn.mcc); }

      parsed_mnc = digits_to_number(primary_plmn.mnc);
    }

    using BarredOpts = asn1::rrc::sib_type1_s::cell_access_related_info_s_::cell_barred_opts;
    // Вычисляем статус блокировки вышки по твоему вложенному энуму
    // cell_barred_opts
    bool barred(info.cell_barred.value == BarredOpts::barred);

    // Извлекаем флаг разрешения перевыбора на этой же частоте!
    using ReselOpts = asn1::rrc::sib_type1_s::cell_access_related_info_s_::intra_freq_resel_opts;
    bool resel_allowed = (info.intra_freq_resel.value == ReselOpts::allowed);

    const auto& sel_info = sib1.cell_sel_info;
    // Выкусываем минимальный уровень RSRP для входа на вышку структуры
    // cell_sel_info_s_
    int8_t rx_lev_min_dbm = static_cast<int8_t>(sib1.cell_sel_info.q_rx_lev_min * 2);
    uint8_t rx_offset = 0;
    if (sel_info.q_rx_lev_min_offset_present) {
      rx_offset = static_cast<uint8_t>(sel_info.q_rx_lev_min * 2);
    }

    bool csg_present = info.csg_ind;
    uint32_t csg_identifier = 0;

    if (info.csg_id_present) { csg_identifier = static_cast<uint32_t>(info.csg_id.to_number()); }

    return CellPassport{
        .tac = static_cast<uint32_t>(sib1.cell_access_related_info.tac.to_number()),
        .cell_id = static_cast<uint32_t>(sib1.cell_access_related_info.cell_id.to_number()),
        .mcc = parsed_mcc,
        .mnc = parsed_mnc,
        .cell_barred = barred,
        .q_rx_lev_min = rx_lev_min_dbm,
        .q_rx_lev_min_offset = rx_offset,
        .intra_freq_reselection_allowed = resel_allowed,
        .csg_ind = csg_present,
        .csg_id = csg_identifier,
    };
  }
};

};  // namespace QCommParser::Utils
