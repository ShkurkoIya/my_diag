/// @file JournalSource.h
/// @brief Offline IDataSource: push HDLC-clean LOG_F frames or ready PacketViews.
///
/// Same contract as live sources — parsers/engine do not care about origin.
#pragma once

#include <atomic>
#include <span>
#include <string_view>

#include <observer/io/DataSourceInterface.h>
#include <qcom/protocol/LogFrameAdapter.h>

namespace QCom {

class JournalSource : public IDataSource {
public:
  void set_frame_callback(FrameCallback cb) override { m_callback = std::move(cb); }

  [[nodiscard]] bool start() override {
    m_running = true;
    return true;
  }

  void stop() override { m_running = false; }

  [[nodiscard]] std::string_view name() const noexcept override { return "journal"; }
  [[nodiscard]] bool is_running() const noexcept override { return m_running.load(); }

  /// Push a full LOG_F buffer (14-byte header + payload), as in dump tools.
  void feed_log_f(std::span<const uint8_t> raw_frame) {
    if (!m_running || !m_callback) return;
    if (auto pkt = adapt_log_f_frame(raw_frame)) m_callback(*pkt);
  }

  void feed_packet(QualcommPacketView pkt) {
    if (!m_running || !m_callback) return;
    m_callback(pkt);
  }

private:
  FrameCallback m_callback;
  std::atomic<bool> m_running{false};
};

}  // namespace QCom
