#pragma once

#include <qcom/qmi/error.hpp>
#include <qcom/qmi/types.hpp>

#include "detail/runtime.hpp"

#include <glib.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

namespace QCom::Qmi::detail {

/**
 * @brief Decode 3GPP 24.008-style PLMN from 3 octets (QMI NAS arrays).
 */
[[nodiscard]] inline std::optional<Plmn> plmn_from_bcd(std::span<const uint8_t> bytes) {
  if (bytes.size() < 3) {
    return std::nullopt;
  }
  const auto d = [&](unsigned octet, unsigned nibble) -> int {
    const unsigned v = (nibble == 0) ? (bytes[octet] & 0x0Fu) : ((bytes[octet] >> 4) & 0x0Fu);
    return static_cast<int>(v);
  };
  const int mcc1 = d(0, 0);
  const int mcc2 = d(0, 1);
  const int mcc3 = d(1, 0);
  const int mnc3 = d(1, 1);
  const int mnc1 = d(2, 0);
  const int mnc2 = d(2, 1);
  if (mcc1 > 9 || mcc2 > 9 || mcc3 > 9 || mnc1 > 9 || mnc2 > 9) {
    return std::nullopt;
  }
  Plmn p;
  p.mcc = static_cast<uint16_t>(mcc1 * 100 + mcc2 * 10 + mcc3);
  if (mnc3 == 0xF) {
    p.mnc_digits = 2;
    p.mnc = static_cast<uint16_t>(mnc1 * 10 + mnc2);
  } else {
    if (mnc3 > 9) {
      return std::nullopt;
    }
    p.mnc_digits = 3;
    p.mnc = static_cast<uint16_t>(mnc1 * 100 + mnc2 * 10 + mnc3);
  }
  return p;
}

[[nodiscard]] inline guint timeout_seconds(std::chrono::milliseconds ms) {
  const auto sec = std::chrono::duration_cast<std::chrono::seconds>(ms).count();
  return static_cast<guint>(sec <= 0 ? 1 : sec);
}

[[nodiscard]] inline Error from_gerror(Errc code, GError* error, const char* fallback) {
  GErrorPtr err(error);
  return Error::from(code, err ? err->message : fallback);
}

}  // namespace QCom::Qmi::detail
