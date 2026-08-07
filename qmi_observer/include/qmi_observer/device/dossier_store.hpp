#pragma once

#include "qmi_observer/device/dossier.hpp"
#include "qmi_observer/error.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace qmi_observer::device {

[[nodiscard]] std::string serialize_dossiers(
    const std::unordered_map<std::string, ModemDossier>& map);

[[nodiscard]] Result<std::unordered_map<std::string, ModemDossier>> parse_dossiers(
    std::string_view json);

[[nodiscard]] Result<void> write_dossiers_file(
    const std::filesystem::path& path,
    const std::unordered_map<std::string, ModemDossier>& map);

[[nodiscard]] Result<std::unordered_map<std::string, ModemDossier>> read_dossiers_file(
    const std::filesystem::path& path);

}  // namespace qmi_observer::device
