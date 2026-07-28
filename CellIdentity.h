#pragma once

#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

namespace QCommParser {

enum class RatType : uint8_t { GSM = 2, WCDMA = 3, LTE = 4, NR = 5, UNKNOWN = 0 };

struct GlobalCellKey {
  uint16_t mcc{250};
  uint16_t mnc{0};
  uint32_t tac{0};
  uint32_t cell_id{0};

  auto operator<=>(const GlobalCellKey&) const = default;
};

struct LocalCellKey {
  uint32_t freq{0};
  uint16_t pci_bsic{0};

  auto operator<=>(const LocalCellKey&) const = default;
};

struct CellPassport {
  uint32_t tac{0};
  uint32_t cell_id{0};
  uint16_t mcc{0};
  uint16_t mnc{0};
  bool cell_barred{false};  // Запрет на подключение к вышке (SIB1)

  [[nodiscard]] constexpr bool has_identity() const noexcept { return cell_id > 0; }

  auto operator<=>(const CellPassport&) const = default;
};

struct LteRadioParams {
  uint32_t earfcn{0};
  uint16_t pci{0};
  uint8_t dl_bw{0};
  uint8_t ul_bw{0};
  uint8_t q_rx_lev_min_offset{0};
  uint8_t q_hyst{0};
  bool intra_freq_reselection_allowed{true};
  bool csg_ind{false};
  uint32_t csg_id{0};

  auto operator<=>(const LteRadioParams&) const = default;
};

struct NrRadioParams {
  uint32_t nrarfcn{0};
  uint16_t pci{0};
  uint8_t dl_bw{0};
  uint8_t ul_bw{0};
  uint8_t q_rx_lev_min{0};
  uint8_t q_hyst{0};
  bool intra_freq_reselection_allowed{true};
  uint32_t cag_id{0};

  auto operator<=>(const NrRadioParams&) const = default;
};

struct WcdmaRadioParams {
  uint32_t uarfcn{0};
  uint16_t psc{0};
  int8_t q_rx_lev_min_rscp{0};
  int8_t q_qual_min_ecno{0};

  auto operator<=>(const WcdmaRadioParams&) const = default;
};

struct GsmRadioParams {
  uint32_t arfcn{0};
  uint16_t bsic{0};
  int8_t rxlev_access_min{0};
  uint8_t cell_reselect_hysteresis{0};

  auto operator<=>(const GsmRadioParams&) const = default;
};

struct InterFreqNeighbor {
  uint32_t earfcn{0};
  uint8_t allowed_bw{0};

  auto operator<=>(const InterFreqNeighbor&) const = default;
};

// Параметры радиоканала (L1 физика / Склеивающий ключ)
struct CellRadio {
  std::variant<std::monostate, GsmRadioParams, WcdmaRadioParams, LteRadioParams, NrRadioParams>
      radio_data;

  std::vector<uint16_t> intra_freq_neighbors{};

  std::vector<InterFreqNeighbor> inter_freq_neighbors{};

  template <typename T>
  [[nodiscard]] auto& get(this auto&& self) {
    return std::get<T>(std::forward<decltype(self)>(self).radio_data);
  }

  template <typename T>
  [[nodiscard]] auto* get_if(this auto&& self) noexcept {
    return std::get_if<T>(&std::forward<decltype(self)>(self).radio_data);
  }

  template <typename T>
  [[nodiscard]] bool is() const noexcept {
    return std::holds_alternative<T>(radio_data);
  }

  [[nodiscard]] uint32_t freq() const noexcept {
    return std::visit(
        [](const auto& arg) -> uint32_t {
          if constexpr (requires { arg.earfcn; }) return arg.earfcn;
          if constexpr (requires { arg.nrarfcn; }) return arg.nrarfcn;
          if constexpr (requires { arg.uarfcn; }) return arg.uarfcn;
          if constexpr (requires { arg.arfcn; }) return arg.arfcn;
          return 0;
        },
        radio_data);
  }

  [[nodiscard]] uint16_t pci_bsic() const noexcept {
    return std::visit(
        [](const auto& arg) -> uint16_t {
          if constexpr (requires { arg.pci; }) return arg.pci;
          if constexpr (requires { arg.psc; }) return arg.psc;
          if constexpr (requires { arg.bsic; }) return arg.bsic;
          return 0;
        },
        radio_data);
  }

  auto operator<=>(const CellRadio&) const = default;
};

struct GsmSignalParams {
  int8_t rxlev{0};
  uint8_t rxqual{0};
  auto operator<=>(const GsmSignalParams&) const = default;
};

struct WcdmaSignalParams {
  float rscp{0.0f};
  float ecio{0.0f};
  auto operator<=>(const WcdmaSignalParams&) const = default;
};

struct LteSignalParams {
  float rsrp{0.0f};
  float rsrq{0.0f};
  float sinr{0.0f};

  auto operator<=>(const LteSignalParams&) const = default;
};

struct NrSignalParams {
  float rsrp{0.0f};
  float rsrq{0.0f};
  float sinr{0.0f};
  auto operator<=>(const NrSignalParams&) const = default;
};

struct CellSignal {
  std::variant<std::monostate, GsmSignalParams, WcdmaSignalParams, LteSignalParams, NrSignalParams>
      signal_data;

  template <typename T>
  [[nodiscard]] auto& get(this auto&& self) {
    return std::get<T>(std::forward<decltype(self)>(self).signal_data);
  }

  template <typename T>
  [[nodiscard]] auto* get_if(this auto&& self) noexcept {
    return std::get_if<T>(&std::forward<decltype(self)>(self).signal_data);
  }

  [[nodiscard]] float main_level() const noexcept {
    return std::visit(
        [](const auto& arg) -> float {
          if constexpr (requires { arg.rsrp; }) return arg.rsrp;
          if constexpr (requires { arg.rscp; }) return arg.rscp;
          if constexpr (requires { arg.rxlev; }) return static_cast<float>(arg.rxlev);
          return 0.0f;
        },
        signal_data);
  }

  auto operator<=>(const CellSignal&) const = default;
};

class CellIdentity {
public:
  RatType rat{RatType::UNKNOWN};
  bool is_serving{false};
  CellPassport passport{};
  CellRadio radio{};
  CellSignal signal;

  CellIdentity() noexcept = default;
  CellIdentity(RatType r, bool serving, CellPassport p, CellRadio rad, CellSignal s = {}) noexcept
      : rat(r), is_serving(serving), passport(p), radio(rad), signal(s) {}

  template <typename T>
  [[nodiscard]] auto& radio_as(this auto&& self) {
    return std::forward<decltype(self)>(self).radio.template get<T>();
  }

  template <typename T>
  [[nodiscard]] auto* radio_as_if(this auto&& self) noexcept {
    return std::forward<decltype(self)>(self).radio.template get_if<T>();
  }

  template <typename T>
  [[nodiscard]] bool is_rat() const noexcept {
    return radio.is<T>();
  }

  template <typename T>
  [[nodiscard]] auto& signal_as(this auto&& self) {
    return std::forward<decltype(self)>(self).signal.template get<T>();
  }

  template <typename T>
  [[nodiscard]] auto* signal_as_if(this auto&& self) noexcept {
    return std::forward<decltype(self)>(self).signal.template get_if<T>();
  }

  auto operator<=>(const CellIdentity&) const = default;
};

template <RatType R>
struct RatTraits;
template <>
struct RatTraits<RatType::GSM> {
  using radio_type = GsmRadioParams;
  using signal_type = GsmSignalParams;
};
template <>
struct RatTraits<RatType::WCDMA> {
  using radio_type = WcdmaRadioParams;
  using signal_type = WcdmaSignalParams;
};
template <>
struct RatTraits<RatType::LTE> {
  using radio_type = LteRadioParams;
  using signal_type = LteSignalParams;
};
template <>
struct RatTraits<RatType::NR> {
  using radio_type = NrRadioParams;
  using signal_type = NrSignalParams;
};

template <RatType R>
using RatRadio_t = typename RatTraits<R>::radio_type;
template <RatType R>
using RatSignal_t = typename RatTraits<R>::signal_type;

}  // namespace QCommParser
