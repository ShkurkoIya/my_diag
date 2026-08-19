#pragma once

/**
 * @file session.hpp
 * @brief QMI session lifetime (device + DMS/NAS clients). Public API is GLib-free.
 */

#include <qcom/qmi/callbacks.hpp>
#include <qcom/qmi/error.hpp>
#include <qcom/qmi/health.hpp>
#include <qcom/qmi/settings.hpp>
#include <qcom/qmi/types.hpp>

#include <memory>

namespace QCom::Qmi {

class HealthMonitor;
class ModemControl;
class NasReader;
class ScanController;

/**
 * @brief Owns the QMI port and service clients used by control/observe layers.
 *
 * Typical use:
 * @code
 * Session s({.device_path="/dev/cdc-wdm0", .use_proxy=false});
 * if (auto r = s.open(); !r) { ... }
 * auto id = s.identity();
 * @endcode
 */
class Session {
 public:
  explicit Session(Settings settings = {});
  ~Session();

  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  Session(Session&&) noexcept;
  Session& operator=(Session&&) noexcept;

  [[nodiscard]] const Settings& settings() const noexcept;
  Settings& settings() noexcept;

  void set_callbacks(Callbacks cb);
  [[nodiscard]] const Callbacks& callbacks() const noexcept;

  [[nodiscard]] Result<void> open();
  void close();
  [[nodiscard]] bool is_open() const noexcept;

  /// DMS manufacturer / model / revision (hello-world path).
  [[nodiscard]] Result<ModemIdentity> identity();

  /**
   * @brief Refresh health probe from the modem and cache @ref ModemHealth.
   * @return Latest evaluated health (or NotOpen).
   */
  [[nodiscard]] Result<ModemHealth> refresh_health();

  [[nodiscard]] const ModemHealth& last_health() const noexcept;

  // Facades (bound to this session; invalid after move)
  [[nodiscard]] HealthMonitor& health();
  [[nodiscard]] ModemControl& control();
  [[nodiscard]] NasReader& nas();
  [[nodiscard]] ScanController& scan();

  struct Impl;  ///< @private Internal QMI handles (src only).
  [[nodiscard]] Impl& impl();
  [[nodiscard]] const Impl& impl() const;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace QCom::Qmi
