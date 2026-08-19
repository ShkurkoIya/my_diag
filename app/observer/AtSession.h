#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "observer/Signals.h"

namespace Observer {

/// True when AT response contains a final result code on its own line.
[[nodiscard]] inline bool at_has_final_result(std::string_view out) noexcept {
  size_t i = 0;
  while (i < out.size()) {
    while (i < out.size() && (out[i] == '\r' || out[i] == '\n')) ++i;
    if (i >= out.size()) break;
    size_t e = i;
    while (e < out.size() && out[e] != '\r' && out[e] != '\n') ++e;
    const std::string_view line = out.substr(i, e - i);
    if (line == "OK" || line == "ERROR" || line.starts_with("+CME ERROR") ||
        line.starts_with("+CMS ERROR"))
      return true;
    i = e;
  }
  return false;
}

[[nodiscard]] inline std::string at_wire_cmd(const char* cmd) {
  std::string wire = cmd ? cmd : "";
  if (wire.empty() || wire.back() != '\n') wire += "\r\n";
  else if (wire.size() >= 2 && wire[wire.size() - 2] != '\r')
    wire.insert(wire.end() - 1, '\r');
  return wire;
}

/// Persistent AT port: one fd, serialized cmds, drain-before-write, line-final OK/ERROR.
/// Open/close-per-command used to drop URCs and race hop/QMI/cereg threads.
class AtSession {
public:
  using TickFn = std::function<void(int elapsed_ms, std::string_view partial)>;

  explicit AtSession(std::string path) : path_(std::move(path)) {}
  ~AtSession() { close_fd(); }
  AtSession(const AtSession&) = delete;
  AtSession& operator=(const AtSession&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }

  [[nodiscard]] std::optional<std::string> transact(const char* cmd, int timeout_ms = 2000,
                                                    TickFn tick = {}) {
    std::lock_guard lock(mu_);
    return transact_unlocked(cmd, timeout_ms, std::move(tick));
  }

  /// Drop and reopen tty (needed after AT+CFUN=1,1 soft reset).
  void reconnect() {
    std::lock_guard lock(mu_);
    close_fd();
    (void)ensure_open();
  }

  /// Best-effort write without waiting for OK (prefer @ref transact).
  bool write_raw(const char* cmd) {
    std::lock_guard lock(mu_);
    if (!ensure_open()) return false;
    drain_input();
    const std::string wire = at_wire_cmd(cmd);
    const ssize_t n = ::write(fd_, wire.data(), wire.size());
    if (n != static_cast<ssize_t>(wire.size())) {
      close_fd();
      return false;
    }
    ::tcdrain(fd_);
    return true;
  }

private:
  std::string path_;
  std::mutex mu_;
  int fd_{-1};
  bool echo_off_{false};

  void close_fd() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    echo_off_ = false;
  }

  bool ensure_open() {
    if (fd_ >= 0) return true;
    fd_ = ::open(path_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) return false;
    termios tio{};
    if (tcgetattr(fd_, &tio) == 0) {
      cfmakeraw(&tio);
      cfsetispeed(&tio, B115200);
      cfsetospeed(&tio, B115200);
      tio.c_cflag |= (CLOCAL | CREAD);
      tio.c_cc[VMIN] = 0;
      tio.c_cc[VTIME] = 0;
      tcsetattr(fd_, TCSANOW, &tio);
    }
    drain_input();
    if (!echo_off_) {
      // Quiet echo so parsers see clean OK lines; ignore failures on quirky firmwares.
      const std::string wire = at_wire_cmd("ATE0");
      (void)::write(fd_, wire.data(), wire.size());
      ::tcdrain(fd_);
      const auto until =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
      std::string junk;
      while (std::chrono::steady_clock::now() < until) {
        pollfd pfd{.fd = fd_, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, 50) <= 0) continue;
        char buf[256];
        const ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n > 0) junk.append(buf, static_cast<size_t>(n));
        if (at_has_final_result(junk)) break;
      }
      echo_off_ = true;
      drain_input();
    }
    return true;
  }

  void drain_input() {
    if (fd_ < 0) return;
    char junk[512];
    for (int i = 0; i < 64; ++i) {
      const ssize_t n = ::read(fd_, junk, sizeof(junk));
      if (n <= 0) break;
    }
  }

  [[nodiscard]] std::optional<std::string> transact_unlocked(const char* cmd, int timeout_ms,
                                                             TickFn tick) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (!ensure_open()) return std::nullopt;
      drain_input();
      const std::string wire = at_wire_cmd(cmd);
      if (::write(fd_, wire.data(), wire.size()) != static_cast<ssize_t>(wire.size())) {
        close_fd();
        continue;
      }
      ::tcdrain(fd_);

      std::string out;
      const auto start = std::chrono::steady_clock::now();
      const auto deadline = start + std::chrono::milliseconds(std::max(200, timeout_ms));
      int last_tick_bucket = -1;
      while (std::chrono::steady_clock::now() < deadline) {
        // Stop must not wait out AT+COPS=? / PLMN select (up to 120s).
        if (g_user_stop.load(std::memory_order_relaxed)) {
          return out.empty() ? std::nullopt : std::optional<std::string>(out);
        }
        pollfd pfd{.fd = fd_, .events = POLLIN, .revents = 0};
        const int pr = ::poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
          close_fd();
          break;
        }
        if (pr > 0 && (pfd.revents & POLLIN)) {
          char buf[1024];
          for (;;) {
            const ssize_t n = ::read(fd_, buf, sizeof(buf));
            if (n > 0) {
              out.append(buf, static_cast<size_t>(n));
              continue;
            }
            break;
          }
          if (at_has_final_result(out)) return out;
        }
        if (tick) {
          const int elapsed = static_cast<int>(
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - start)
                  .count());
          const int bucket = elapsed / 1000;
          if (bucket != last_tick_bucket) {
            last_tick_bucket = bucket;
            tick(elapsed, out);
          }
        }
      }
      if (!out.empty()) return out;  // timeout with partial (COPS list mid-flight)
      close_fd();                    // retry with fresh fd
    }
    return std::nullopt;
  }
};

/// Legacy one-shot helpers (cleanup paths / when no session yet).
inline bool at_write(const std::string& path, const char* cmd) {
  AtSession s(path);
  return s.write_raw(cmd);
}

[[nodiscard]] inline std::optional<std::string> at_transact(const std::string& path, const char* cmd,
                                                     int timeout_ms = 2000) {
  AtSession s(path);
  return s.transact(cmd, timeout_ms);
}

}  // namespace Observer
