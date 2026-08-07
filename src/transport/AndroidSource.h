/// @file AndroidSource.h
/// @brief Android DIAG/DCI data source stub — same IDataSource contract as Linux.
///
/// Production path (to implement later):
///   1. Open DCI client (libdiag / vendor diag) or equivalent.
///   2. Register log codes (same semantic set as DiagSession masks).
///   3. On each log: call adapt_log_f_frame() if buffer has LOG_F header,
///      or adapt_dci_log(code, ts, payload) if already split.
///   4. Invoke FrameCallback — QualcomParser / CellTracker unchanged.
///
/// Do NOT port Linux tty/epoll here; keep OS I/O in this file only.
#pragma once

#include <atomic>
#include <span>
#include <string>
#include <string_view>

#include "transport/DataSourceInterface.h"
#include "transport/DiagSourceConfig.h"
#include "transport/LogFrameAdapter.h"

namespace QCom {

class AndroidSource : public IDataSource {
public:
  explicit AndroidSource(DiagSourceConfig cfg = {}) : m_cfg(std::move(cfg)) {}

  ~AndroidSource() override { stop(); }

  void set_frame_callback(FrameCallback cb) override { m_callback = std::move(cb); }

  [[nodiscard]] std::string_view name() const noexcept override { return "android-diag"; }
  [[nodiscard]] bool is_running() const noexcept override { return m_running.load(); }
  [[nodiscard]] const DiagSourceConfig& config() const noexcept { return m_cfg; }

  /// Stub: returns false until DCI/libdiag wiring exists.
  [[nodiscard]] bool start() override {
    // Intentionally not implemented — fill with DCI client registration.
    (void)m_cfg;
    m_running = false;
    return false;
  }

  void stop() override { m_running = false; }

  /// Helpers for the future DCI callback thread (call only after start succeeds).
  void deliver_log_f(std::span<const uint8_t> raw) {
    if (!m_running || !m_callback) return;
    if (auto pkt = adapt_log_f_frame(raw)) m_callback(*pkt);
  }

  void deliver_dci(LogCode code, uint64_t timestamp, std::span<const uint8_t> payload) {
    if (!m_running || !m_callback) return;
    m_callback(adapt_dci_log(code, timestamp, payload));
  }

private:
  DiagSourceConfig m_cfg;
  FrameCallback m_callback;
  std::atomic<bool> m_running{false};
};

}  // namespace QCom
