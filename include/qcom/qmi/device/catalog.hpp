#pragma once

/**
 * @file catalog.hpp
 * @brief Каталог модемов: быстрый enumerate, dossier, probe, выдача отчётов.
 *
 * @par Модель использования (малина / горячая смена USB)
 * 1. @ref refresh — дешёвый sysfs-обход (миллисекунды)
 * 2. @ref list_reports — отдать GUI/сканеру без блокировок
 * 3. @ref probe — по запросу/фону простукать AT+QMI и сохранить dossier
 * 4. @ref to_qmi_settings / выбрать endpoint → @ref Session
 */

#include <qcom/qmi/device/dossier.hpp>
#include <qcom/qmi/device/endpoint.hpp>
#include <qcom/qmi/device/profile.hpp>
#include <qcom/qmi/error.hpp>
#include <qcom/qmi/settings.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace QCom::Qmi::device {

struct EnumerateOptions {
  /// Корень sysfs (для тестов можно подменить на fixture).
  std::filesystem::path sysfs_root{"/sys"};
  std::filesystem::path dev_root{"/dev"};
  bool only_with_qmi{false};  ///< отфильтровать устройства без cdc-wdm
};

struct ProbeOptions {
  ProbeLevel level{ProbeLevel::Identity};
  bool probe_at{true};
  std::chrono::milliseconds at_timeout{std::chrono::milliseconds(400)};
  std::chrono::milliseconds qmi_timeout{std::chrono::seconds(8)};
  bool use_proxy{false};  ///< для exclusive на малине обычно false
};

struct CatalogCallbacks {
  std::function<void(const ModemEndpoint&)> on_added;
  std::function<void(const std::string& id)> on_removed;
  std::function<void(const ModemReport&)> on_updated;
};

/**
 * @brief Менеджер устройств модемов (device catalog).
 *
 * Потокобезопасность: один владелец (поток сканера). Не шарьте без мьютекса.
 */
class DeviceCatalog {
 public:
  explicit DeviceCatalog(ProfileRegistry profiles = {});

  void set_callbacks(CatalogCallbacks cb);
  void set_dossier_path(std::filesystem::path path);

  [[nodiscard]] const ProfileRegistry& profiles() const noexcept { return profiles_; }

  /**
   * @brief Быстрый инвентарь USB-модемов (без QMI/AT open).
   */
  [[nodiscard]] Result<std::vector<ModemEndpoint>> refresh(EnumerateOptions opts = {});

  [[nodiscard]] const std::vector<ModemEndpoint>& endpoints() const noexcept {
    return endpoints_;
  }

  [[nodiscard]] const ModemEndpoint* find(std::string_view id) const noexcept;

  [[nodiscard]] std::vector<ModemReport> list_reports() const;

  [[nodiscard]] Result<ModemReport> report(std::string_view id) const;

  /**
   * @brief Простучать endpoint и обновить dossier (+ сохранить на диск, если путь задан).
   */
  [[nodiscard]] Result<ModemDossier> probe(std::string_view id, ProbeOptions opts = {});

  [[nodiscard]] Result<void> load_dossiers();
  [[nodiscard]] Result<void> save_dossiers() const;

  [[nodiscard]] Settings to_qmi_settings(std::string_view id) const;

  /**
   * @brief Сравнить с предыдущим refresh и дернуть callbacks added/removed.
   *        Вызывается автоматически из @ref refresh.
   */
  void emit_diff(const std::vector<ModemEndpoint>& previous);

 private:
  ProfileRegistry profiles_;
  CatalogCallbacks callbacks_{};
  std::filesystem::path dossier_path_;
  std::vector<ModemEndpoint> endpoints_;
  std::unordered_map<std::string, ModemDossier> dossiers_;
};

/**
 * @brief Низкоуровневый sysfs enumerate (тестируется отдельно).
 */
[[nodiscard]] Result<std::vector<ModemEndpoint>> enumerate_sysfs(
    const EnumerateOptions& opts, const ProfileRegistry& profiles);

}  // namespace QCom::Qmi::device
