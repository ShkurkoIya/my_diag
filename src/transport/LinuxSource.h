/// @file LinuxSource.h
/// @brief Linux tty DIAG data source (SIMCom etc.): serial + demux + DiagSession.
///
/// Data flow: /dev/ttyUSBx → epoll → DiagSerialDemux → adapt_log_f_frame → callback
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include <cerrno>

#include "transport/DataSourceInterface.h"
#include "transport/DiagCommands.h"
#include "transport/DiagSerialDemux.h"
#include "transport/DiagSourceConfig.h"

#ifdef __linux__
#  include <errno.h>
#  include <fcntl.h>
#  include <sys/epoll.h>
#  include <termios.h>
#  include <unistd.h>
#endif

namespace QCom {

class LinuxSource : public IDataSource {
public:
  explicit LinuxSource(DiagSourceConfig cfg = {})
      : m_cfg(std::move(cfg))
#ifdef __linux__
        ,
        m_baud(baud_from_rate(m_cfg.baud_rate))
#endif
  {
  }

  explicit LinuxSource(std::string device, int baud_rate = 921600)
      : LinuxSource(DiagSourceConfig{.device_path = std::move(device), .baud_rate = baud_rate}) {}

  ~LinuxSource() override { stop(); }

  void set_frame_callback(FrameCallback cb) override { m_callback = std::move(cb); }

  [[nodiscard]] std::string_view name() const noexcept override { return "linux-diag"; }
  [[nodiscard]] bool is_running() const noexcept override { return m_running.load(); }

  [[nodiscard]] const DiagSourceConfig& config() const noexcept { return m_cfg; }
  [[nodiscard]] bool init_ok() const noexcept { return m_init_ok; }
  [[nodiscard]] std::string_view last_error() const noexcept { return m_last_error; }

  void set_device_path(std::string path) { m_cfg.device_path = std::move(path); }

  /// stop() + start() — safe after USB unplug (joins leftover worker from HUP).
  [[nodiscard]] bool reconnect() {
    stop();
    return start();
  }

  [[nodiscard]] bool start() override {
#ifdef __linux__
    // USB HUP ends read_loop with m_running=false but leave a joinable std::thread.
    // Starting again without join → std::terminate. Always reap first.
    if (m_worker.joinable()) {
      m_stop_requested = true;
      m_running = false;
      m_worker.join();
      m_stop_requested = false;
    }
    cleanup_fds();

    if (m_running.exchange(true)) return true;
    m_last_error.clear();
    m_init_ok = false;
    // Keep cumulative byte counters across reconnects (session totals).

    m_fd = open_serial();
    if (m_fd < 0) {
      if (m_last_error.empty()) m_last_error = "open_serial failed";
      m_running = false;
      return false;
    }

    m_epoll_fd = epoll_create1(0);
    if (m_epoll_fd < 0) {
      m_last_error = "epoll_create1 failed";
      ::close(m_fd);
      m_fd = -1;
      m_running = false;
      return false;
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    ev.data.fd = m_fd;
    if (epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_fd, &ev) < 0) {
      m_last_error = "epoll_ctl failed";
      cleanup_fds();
      m_running = false;
      return false;
    }

    m_demux.clear_buffer();
    m_demux.set_log_callback([this](QualcommPacketView pkt) {
      if (m_callback) m_callback(pkt);
    });

    if (m_cfg.init_masks) {
      m_init_ok = init_diag_session();
      if (!m_init_ok) {
        m_last_error = "DiagSession::init_modem failed (masks may be unset)";
      }
    } else {
      m_init_ok = true;
    }

    m_worker = std::thread(&LinuxSource::read_loop, this);
    return true;
#else
    (void)m_cfg;
    m_last_error = "LinuxSource not available on this platform";
    return false;
#endif
  }

  void stop() override {
    m_stop_requested = true;
    // Always join if the worker exists — read_loop may clear m_running on USB
    // disconnect before stop() runs; early-return used to leave a joinable
    // std::thread and abort in ~LinuxSource via std::terminate.
    (void)m_running.exchange(false);
    if (m_worker.joinable()) m_worker.join();
#ifdef __linux__
    cleanup_fds();
#endif
    m_stop_requested = false;
  }

  [[nodiscard]] uint64_t frames_ok() const noexcept {
    return m_demux.messages_ok() + m_demux.hdlc_ok();
  }
  [[nodiscard]] uint64_t frames_bad_crc() const noexcept { return m_demux.hdlc_bad_crc(); }
  [[nodiscard]] uint64_t logs_delivered() const noexcept { return m_demux.logs_delivered(); }
  [[nodiscard]] uint64_t bytes_raw() const noexcept { return m_bytes_raw.load(); }
  [[nodiscard]] uint64_t silent_revives() const noexcept { return m_revives.load(); }

private:
  DiagSourceConfig m_cfg;
#ifdef __linux__
  speed_t m_baud{B921600};
#endif
  int m_fd{-1};
  int m_epoll_fd{-1};
  std::atomic<bool> m_running{false};
  std::atomic<bool> m_stop_requested{false};
  std::atomic<uint64_t> m_bytes_raw{0};
  std::atomic<uint64_t> m_revives{0};
  std::thread m_worker;
  FrameCallback m_callback;
  DiagSerialDemux m_demux;
  bool m_init_ok{false};
  std::string m_last_error;

#ifdef __linux__
  static speed_t baud_from_rate(int baud) {
    switch (baud) {
      case 115200: return B115200;
      case 230400: return B230400;
      case 460800: return B460800;
      case 921600: return B921600;
      default: return B921600;
    }
  }

  int open_serial(int max_attempts = 12) {
    // After USB re-enumerate the node appears before the driver finishes bind —
    // open/tcsetattr often fails briefly with EBUSY / EIO / ENOENT.
    for (int attempt = 0; attempt < std::max(1, max_attempts); ++attempt) {
      if (attempt > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

      errno = 0;
      int fd = ::open(m_cfg.device_path.c_str(), O_RDWR | O_NONBLOCK | O_NOCTTY);
      if (fd < 0) {
        m_last_error = std::string("open(") + m_cfg.device_path + "): " + std::strerror(errno);
        const int e = errno;
        if (e == ENOENT || e == EBUSY || e == EAGAIN || e == EACCES || e == EIO || e == ENODEV)
          continue;
        return -1;
      }

      struct termios tio{};
      if (tcgetattr(fd, &tio) != 0) {
        m_last_error = std::string("tcgetattr(") + m_cfg.device_path + "): " + std::strerror(errno);
        ::close(fd);
        continue;
      }
      cfmakeraw(&tio);
      cfsetispeed(&tio, m_baud);
      cfsetospeed(&tio, m_baud);
      tio.c_cflag |= (CLOCAL | CREAD);
      tio.c_cc[VMIN] = 0;
      tio.c_cc[VTIME] = 0;
      if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        m_last_error = std::string("tcsetattr(") + m_cfg.device_path + "): " + std::strerror(errno);
        ::close(fd);
        continue;
      }
      tcflush(fd, TCIOFLUSH);
      m_last_error.clear();
      return fd;
    }
    if (m_last_error.empty())
      m_last_error = std::string("open_serial(") + m_cfg.device_path + ") retries exhausted";
    return -1;
  }

  bool init_diag_session() {
    DiagSession session([this](std::span<const uint8_t> frame) -> bool {
      return write_all(frame.data(), frame.size());
    });
    bool ok = session.init_modem();
    if (!ok) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      drain_rx();
      ok = session.init_modem();
    }
    drain_rx();
    return ok;
  }

  void drain_rx() {
    uint8_t buf[4096];
    for (;;) {
      ssize_t n = ::read(m_fd, buf, sizeof(buf));
      if (n > 0) {
        m_bytes_raw.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
        m_demux.feed({buf, static_cast<size_t>(n)});
        continue;
      }
      break;
    }
  }

  bool write_all(const uint8_t* data, size_t len) {
    size_t written = 0;
    int spins = 0;
    while (written < len) {
      drain_rx();
      ssize_t n = ::write(m_fd, data + written, len - written);
      if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) {
          if (++spins > 2000) return false;
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        return false;
      }
      spins = 0;
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
    m_demux.clear_buffer();
  }

  /// Re-open DIAG tty on the same epoll loop (worker thread only).
  /// SIMCOM signals HUP/EOF on RAT switches; full disconnect storms are worse.
  [[nodiscard]] bool revive_serial_on_worker() {
    if (m_epoll_fd < 0) return false;
    if (m_fd >= 0) {
      ::epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, m_fd, nullptr);
      ::close(m_fd);
      m_fd = -1;
    }
    m_demux.clear_buffer();

    for (int attempt = 0; attempt < 20; ++attempt) {
      if (!m_running.load(std::memory_order_relaxed) ||
          m_stop_requested.load(std::memory_order_relaxed))
        return false;
      if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(400));

      // Short open attempts — outer loop already backs off.
      m_fd = open_serial(/*max_attempts=*/2);
      if (m_fd < 0) continue;

      struct epoll_event ev{};
      ev.events = EPOLLIN | EPOLLERR | EPOLLHUP;
      ev.data.fd = m_fd;
      if (::epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, m_fd, &ev) < 0) {
        m_last_error = std::string("epoll_ctl revive: ") + std::strerror(errno);
        ::close(m_fd);
        m_fd = -1;
        continue;
      }
      // Skip mask re-init — DiagSession during revive can re-HUP the link.
      m_revives.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  void read_loop() {
    constexpr size_t BUF_SIZE = 65536;
    uint8_t buf[BUF_SIZE];
    struct epoll_event events[1];

    while (m_running.load(std::memory_order_relaxed)) {
      if (m_fd < 0 || m_epoll_fd < 0) {
        if (!revive_serial_on_worker()) break;
        continue;
      }

      int nfds = epoll_wait(m_epoll_fd, events, 1, 50);
      if (nfds < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (nfds == 0) continue;

      const uint32_t evbits = events[0].events;
      bool got_data = false;
      bool need_revive = false;

      for (;;) {
        ssize_t n = ::read(m_fd, buf, BUF_SIZE);
        if (n > 0) {
          got_data = true;
          m_bytes_raw.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
          m_demux.feed({buf, static_cast<size_t>(n)});
          continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        // n==0 (EOF) after HUP is normal on option/tty during RAT change — revive.
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
          need_revive = true;
          break;
        }
        break;
      }

      if (got_data) continue;

      if (!need_revive && (evbits & (EPOLLERR | EPOLLHUP))) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ssize_t n = ::read(m_fd, buf, BUF_SIZE);
        if (n > 0) {
          m_bytes_raw.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
          m_demux.feed({buf, static_cast<size_t>(n)});
          continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        need_revive = true;
      }

      if (!need_revive) continue;

      if (revive_serial_on_worker()) continue;
      // Second chance after a longer pause (USB re-enum).
      std::this_thread::sleep_for(std::chrono::seconds(2));
      if (revive_serial_on_worker()) continue;
      break;
    }

    const bool intentional = m_stop_requested.load(std::memory_order_relaxed);
    m_running = false;
    if (!intentional) notify_disconnect();
  }
#endif
};

}  // namespace QCom
