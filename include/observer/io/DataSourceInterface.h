/// @file DataSourceInterface.h
/// @brief OS-agnostic contract: modem bytes → QualcommPacketView.
///
/// Linux (tty DIAG), Android (DCI/libdiag), and offline journal all implement
/// this interface so parsers/tracker stay shared across platforms.
#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include <observer/model/Types.h>

namespace QCom {

class IDataSource {
public:
  using FrameCallback = std::function<void(QualcommPacketView pkt)>;
  using DisconnectCallback = std::function<void()>;

  virtual ~IDataSource() = default;

  virtual void set_frame_callback(FrameCallback cb) = 0;
  virtual void set_disconnect_callback(DisconnectCallback cb) { m_on_disconnect = std::move(cb); }

  [[nodiscard]] virtual bool start() = 0;
  virtual void stop() = 0;

  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual bool is_running() const noexcept = 0;

protected:
  void notify_disconnect() {
    if (m_on_disconnect) m_on_disconnect();
  }

  DisconnectCallback m_on_disconnect;
};

}  // namespace QCom
