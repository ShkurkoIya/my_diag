#pragma once

#include <atomic>
#include <thread>

#include "transport/DataSourceInterface.h"

namespace QCom {

/// Linux ttyUSB data source for SIMCom modules.
/// Currently: simulation mode with synthetic packets for testing.
class LinuxSource : public IDataSource {
public:
  LinuxSource() = default;
  ~LinuxSource() override { stop(); }

  void set_frame_callback(FrameCallback cb) override { m_callback = std::move(cb); }

  [[nodiscard]] bool start() override {
    if (m_running.exchange(true)) return true;
    m_worker = std::thread(&LinuxSource::run_loop, this);
    return true;
  }

  void stop() override {
    if (!m_running.exchange(false)) return;
    if (m_worker.joinable()) m_worker.join();
  }

private:
  std::atomic<bool> m_running{false};
  FrameCallback m_callback;
  std::thread m_worker;

  void run_loop() {
    uint64_t mock_ts = 1718912345000ULL;
    std::string mock_rrc_payload = "\x01\x00\x00\x00\x00\x00\x01"
                                   "MOCK_ASN1_SIB1_DATA";

    while (m_running.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!m_running.load(std::memory_order_relaxed)) break;

      mock_ts += 1000;
      if (m_callback) {
        m_callback(QualcommPacketView{
            .log_code = 0xB0C0,
            .timestamp = mock_ts,
            .payload = mock_rrc_payload,
        });
      }
    }
  }
};

}  // namespace QCom
