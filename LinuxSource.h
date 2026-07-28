#pragma once

#include <atomic>
#include <iostream>
#include <thread>

#include "DataSourceInterface.h"

namespace QComScanner {
class LinuxSource : public IDataSource {
public:
  LinuxSource() = default;
  ~LinuxSource() override { stop(); }

  void set_frame_callback(FrameCallback cb) override { m_callback = std::move(cb); }

  bool start() override {
    if (m_is_running) return true;

    std::cout << "[LinuxSource] Симуляция: Открытие /dev/ttyUSB0 и запуск DIAG масок...\n";
    m_is_running = true;

    m_worker_thread = std::thread(&LinuxSource::loop_simulation, this);

    return true;
  }

  void stop() override {
    if (!m_is_running) return;
    m_is_running = false;
    if (m_worker_thread.joinable()) { m_worker_thread.join(); }

    std::cout << "[LinuxSource] Симуляция: Порт /dev/ttyUSB0 закрыт.\n";
  }

  void loop_simulation() {
    uint64_t mock_ts = 1718912345000ULL;

    // ВЫДЕЛЯЕМ БУФЕР ОДИН РАЗ
    // Сюда мы пишем фейковые бинарные данные, которые якобы прилетели из HDLC
    std::string mock_rrc_payload = "\x01\x00\x00\x00\x00\x00\x00"
                                   "ASN1_SIB1_BINARY_DATA_FROM_SRSAN";
    std::string mock_ml1_payload =
        "\x00\x00\x72\x06\x7B\x00\xD0\xFB\x90\xFF";  // Имитация RSRP/RSRQ

    while (m_is_running) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (!m_is_running) break;

      mock_ts += 1000;

      if (m_callback) {
        // 🔴 Имитируем прилет пакета 0xB193 (LTE ML1 Метрики сигнала)
        m_callback(QCommParser::QualcommPacketView{
            .log_code = QCommParser::LogCode::LTE_ML1_SERV_MEAS,
            .timestamp = mock_ts,
            .payload = mock_ml1_payload  // Завернули указатель на буфер за 0 наносекунд [Pages 2]
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 🔴 Имитируем прилет пакета 0xB0C0 (LTE RRC OTA Message / SIB1)
        m_callback(QCommParser::QualcommPacketView{.log_code = QCommParser::LogCode::LTE_RRC_OTA,
                                                   .timestamp = mock_ts + 50,
                                                   .payload = mock_rrc_payload});
      }
    }
  }

private:
  FrameCallback m_callback;
  std::thread m_worker_thread;
  std::atomic<bool> m_is_running{false};
};
}  // namespace QComScanner
