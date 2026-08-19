#pragma once

/**
 * @file version.hpp
 * @brief Library version (independent of libqmi).
 */

#include <string>
#include <string_view>

namespace QCom::Qmi {

inline constexpr std::string_view kVersion = "0.3.0";

[[nodiscard]] std::string_view version() noexcept;
[[nodiscard]] std::string libqmi_version_string();

}  // namespace QCom::Qmi
