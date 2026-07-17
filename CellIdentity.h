#pragma once

#include <compare>
#include <cstdint>

namespace observer_qcom_parser {

enum class RatType : uint8_t {
  GSM = 2,
  WCDMA = 3,
  LTE = 4,
  NR = 5,
  UNKNOWN = 0
};

// Коробка 1: Глобальный паспорт вышки (L3 сигналка / SIB1)
struct CellPassport {
  uint32_t tac{0};
  uint32_t cell_id{0};
  uint16_t mcc{0};
  uint16_t mnc{0};

  [[nodiscard]] constexpr bool has_identity() const noexcept {
    return cell_id > 0;
  }

  auto operator<=>(const CellPassport &) const = default;
};

// Параметры радиоканала (L1 физика / Склеивающий ключ)
struct CellRadio {
  union ChannelUnion {
    struct {
      uint32_t freq{0};
      uint16_t pci_bsic{0};
    } raw;
    struct {
      uint32_t earfcn{0};
      uint16_t pci{0};
    } lte;
    struct {
      uint32_t nrarfcn{0};
      uint16_t pci{0};
    } nr;
    struct {
      uint32_t uarfcn{0};
      uint16_t psc{0};
    } wcndma;
    struct {
      uint32_t arfcn;
      uint8_t bsic;
    } gsm;
  } channel;

  constexpr CellRadio() noexcept : channel{.raw = {0, 0}} {}
  constexpr CellRadio(uint32_t f, uint16_t p) noexcept
      : channel{.raw = {f, p}} {}

  [[nodiscard]] constexpr uint32_t freq() const noexcept {
    return channel.raw.freq;
  }
  [[nodiscard]] constexpr uint16_t pci_bsic() const noexcept {
    return channel.raw.pci_bsic;
  }

  auto operator<=>(const CellRadio &other) const noexcept {
    if (auto cmp = channel.raw.freq <=> other.channel.raw.freq; cmp != 0) {
      return cmp;
    }
    return channel.raw.pci_bsic <=> other.channel.raw.pci_bsic;
  }

  bool operator==(const CellRadio &other) const noexcept {
    return channel.raw.freq == other.channel.raw.freq &&
           channel.raw.pci_bsic == other.channel.raw.pci_bsic;
  }
};

struct CellSignal {
  float rsrp_rscp_rxlev{0.0f}; // RSRP (4G/5G), RSCP (3G), RxLev (2G)
  float rsrq_ecio{0.0f};       // RSRQ (4G/5G), Ec/Io (3G)
};

struct CellIdentity {
  RatType rat{RatType::UNKNOWN};
  bool is_serving{false};

  // 1. Глобальный паспорт (Заполняется из L3 сигналки / SIB-ов)
  CellPassport passport{};
  // 2. Радиофизика L1 (Экономим память через анонимные union)
  CellRadio radio{};
  // 3. Метрики уровней сигналов (ML1)
  CellSignal signal;

  CellIdentity() noexcept = default;
  CellIdentity(CellIdentity &&) noexcept = default;
  CellIdentity &operator=(CellIdentity &&) noexcept = default;
  CellIdentity(const CellIdentity &) = default;
  CellIdentity &operator=(const CellIdentity &) = default;

  auto operator<=>(const CellIdentity &other) const noexcept {
    if (auto cmp = rat <=> other.rat; cmp != 0)
      return cmp;
    if (auto cmp = is_serving <=> other.is_serving; cmp != 0)
      return cmp;
    if (auto cmp = radio <=> other.radio; cmp != 0)
      return cmp;
    return passport <=> other.passport;
  }

  bool operator==(const CellIdentity &other) const noexcept {
    return rat == other.rat && is_serving == other.is_serving &&
           radio == other.radio && passport == other.passport;
  }
};

} // namespace observer_qcom_parser
