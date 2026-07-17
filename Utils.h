#include <bit>
#include <cstdint>

#include "CellIdentity.h"
#include "srsran/asn1/rrc.h"

namespace observer_qcom_parser::utils {

class Converter {
public:
  template <typename T>
  [[nodiscard]] static constexpr T read_le(std::string_view data,
                                           size_t offset) noexcept {
    if (offset + sizeof(T) > data.size())
      return 0;

    return std::bit_cast<T>(
        *reinterpret_cast<const typename std::array<char, sizeof(T)> *>(
            data.data() + offset));
  }

  [[nodiscard]] static CellPassport
  extract_passport(const asn1::rrc::sib_type1_s &sib1) noexcept {
    uint16_t parsed_mcc = 0;
    uint16_t parsed_mnc = 0;

    if (sib1.cell_access_related_info.plmn_id_list.size() > 0) {
      const auto &primary_plmn =
          sib1.cell_access_related_info.plmn_id_list[0].plmn_id;

      if (primary_plmn.mcc_present) {
        parsed_mcc = primary_plmn.mcc[0] * 100 + primary_plmn.mcc[1] * 10 +
                     primary_plmn.mcc[2];
      }

      const size_t mnc_len = primary_plmn.mnc.size();
      if (mnc_len == 2) {
        parsed_mnc = primary_plmn.mnc[0] * 10 + primary_plmn.mnc[1];
      } else if (mnc_len == 3) {
        parsed_mnc = primary_plmn.mnc[0] * 100 + primary_plmn.mnc[1] * 10 +
                     primary_plmn.mcc[2];
      }
    }

    return CellPassport{.tac = static_cast<uint32_t>(
                            sib1.cell_access_related_info.tac.to_number()),
                        .cell_id = static_cast<uint32_t>(
                            sib1.cell_access_related_info.cell_id.to_number()),
                        .mcc = parsed_mcc,
                        .mnc = parsed_mnc};
  }
};

}; // namespace observer_qcom_parser::utils
