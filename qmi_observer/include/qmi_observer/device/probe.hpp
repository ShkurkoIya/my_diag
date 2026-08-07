#pragma once

/**
 * @file probe.hpp
 * @brief Простукивание QMI/AT для заполнения ModemDossier.
 */

#include "qmi_observer/device/catalog.hpp"
#include "qmi_observer/error.hpp"

namespace qmi_observer::device {

[[nodiscard]] Result<ModemDossier> probe_endpoint(const ModemEndpoint& ep,
                                                  const ModemProfile* profile,
                                                  const ProbeOptions& opts,
                                                  const ModemDossier* previous = nullptr);

/**
 * @brief Короткий AT ping: отправить AT\\r, ждать OK.
 * @return Текст ответа (обрезанный) или ошибка.
 */
[[nodiscard]] Result<std::string> probe_at_port(const std::string& path,
                                                std::chrono::milliseconds timeout);

}  // namespace qmi_observer::device
