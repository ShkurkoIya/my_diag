/// @file LinuxSource.h
/// @brief Linux ttyUSB data source for Qualcomm modems (SIMCom 8200 M2 etc.).
///
/// Opens a serial port in raw mode, configures DIAG log masks, reads HDLC frames
/// via epoll, deframes and CRC-verifies them, then delivers clean QualcommPacketView
/// to the callback.
///
/// Data flow:
///   /dev/ttyUSBx → epoll_wait → bulk read → HdlcDeframer → adapt_frame → callback
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include "core/Types.h"
#include "core/Utils.h"
#include "transport/DataSourceInterface.h"
#include "transport/DiagCommands.h"
#include "transport/HdlcCodec.h"

#ifdef __linux__
#  include <fcntl.h>
#  include <sys/epoll.h>
#  include <termios.h>
#  include <unistd.h>
#endif

namespace QCom {

class LinuxSource : public IDataSource {
public:
  explicit LinuxSource(std::string device = "/dev/ttyUSB0", speed_t baud = B921600)
      : m_device(std::move(device)), m_baud(baud) {}

  ~LinuxSource() override { stop(); }

  void set_frame_callback(FrameCallback cb) override { m_callback = std::move(cb); }

  [[nodiscard]] bool start() override {
#ifdef __linux__
    if (m_running.exchange(true)) return true;

    m_fd = open_serial();
    if (m_fd < 0) {
      m_running = false;
      return false;
    }

    m_epoll_fd = epoll_create1(0);
    if (m_epoll_fd < 0) {
      ::close(m_fd);
      m_fd = -1;
      m_running = false;
      return false;
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = m_fd;
    epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_fd, &ev);

    if (!init_diag_session()) {
      cleanup_fds();
      m_running = false;
      return false;
    }

    m_deframer.set_callback(
        [this](std::span<const uint8_t> payload) { adapt_and_deliver(payload); });

    m_worker = std::thread(&LinuxSource::read_loop, this);
    return true;
#else
    return false;
#endif
  }

  void stop() override {
    if (!m_running.exchange(false)) return;
    if (m_worker.joinable()) m_worker.join();
#ifdef __linux__
    cleanup_fds();
#endif
  }

  [[nodiscard]] uint64_t frames_ok() const noexcept { return m_deframer.frames_ok(); }
  [[nodiscard]] uint64_t frames_bad_crc() const noexcept { return m_deframer.frames_bad_crc(); }

private:
  std::string m_device;
  speed_t m_baud;
  int m_fd{-1};
  int m_epoll_fd{-1};
  std::atomic<bool> m_running{false};
  std::thread m_worker;
  FrameCallback m_callback;
  HdlcDeframer m_deframer;

#ifdef __linux__
  int open_serial() {
    int fd = ::open(m_device.c_str(), O_RDWR | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) return -1;

    struct termios tio{};
    tcgetattr(fd, &tio);
    cfmakeraw(&tio);
    cfsetispeed(&tio, m_baud);
    cfsetospeed(&tio, m_baud);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);

    return fd;
  }

  bool init_diag_session() {
    DiagSession session([this](std::span<const uint8_t> frame) -> bool {
      return write_all(frame.data(), frame.size());
    });
    return session.init_modem();
  }

  bool write_all(const uint8_t* data, size_t len) {
    size_t written = 0;
    while (written < len) {
      ssize_t n = ::write(m_fd, data + written, len - written);
      if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        return false;
      }
      written += static_cast<size_t>(n);
    }
    return true;
  }

  void cleanup_fds() {
    if (m_epoll_fd >= 0) {
      ::close(m_epoll_fd);
      m_epoll_fd = -1;
    }
    if (m_fd >= 0) {
      ::close(m_fd);
      m_fd = -1;
    }
    m_deframer.reset();
  }

  void read_loop() {
    constexpr size_t BUF_SIZE = 4096;
    uint8_t buf[BUF_SIZE];
    struct epoll_event events[1];

    while (m_running.load(std::memory_order_relaxed)) {
      int nfds = epoll_wait(m_epoll_fd, events, 1, 100);
      if (nfds <= 0) continue;

      ssize_t n = ::read(m_fd, buf, BUF_SIZE);
      if (n > 0) {
        m_deframer.feed({buf, static_cast<size_t>(n)});
      } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
        break;  // device disconnected or fatal error
      }
    }
  }
#endif

  /// Adapt a clean HDLC payload to QualcommPacketView and deliver via callback.
  ///
  /// DIAG LOG_F wire format (after HDLC deframing):
  ///   [0]     cmd_code (0x10 = LOG_F, skip 0x73/0x7D/0x1C)
  ///   [1]     reserved
  ///   [2-3]   total length (LE16)
  ///   [4-5]   log_code (LE16)
  ///   [6-13]  timestamp (8 bytes)
  ///   [14+]   payload
  void adapt_and_deliver(std::span<const uint8_t> raw) {
    if (raw.size() < 14) return;

    uint8_t cmd_code = raw[0];
    // Skip non-LOG responses (command ACKs, message configs)
    if (cmd_code != 0x10) return;

    uint16_t log_code = Utils::Converter::read_le<uint16_t>(raw.data(), 4);
    uint64_t timestamp = Utils::Converter::read_le<uint64_t>(raw.data(), 6);
    auto payload_data = raw.subspan(14);

    if (m_callback && !payload_data.empty()) {
      m_callback(QualcommPacketView{
          .log_code = log_code,
          .timestamp = timestamp,
          .payload = payload_data,
      });
    }
  }
};

}  // namespace QCom
