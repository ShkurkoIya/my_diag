#pragma once

#include "observer/AtParse.h"
#include "observer/AtSession.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace Observer {

class LiveDashboard;

/// One AT tty, mutexed so hop / rat-guard / CPSI / ghost never interleave.
class AtBus {
public:
  explicit AtBus(std::string path) : sess_(std::make_unique<AtSession>(std::move(path))) {}

  [[nodiscard]] bool ok() const noexcept { return static_cast<bool>(sess_); }
  [[nodiscard]] const std::string& path() const { return sess_->path(); }
  AtSession* session() noexcept { return sess_.get(); }

  [[nodiscard]] std::optional<std::string> cmd(const char* command, int timeout_ms = 2000,
                                               AtSession::TickFn tick = {}) {
    if (!sess_) return std::nullopt;
    std::lock_guard lock(mu_);
    return sess_->transact(command, timeout_ms, std::move(tick));
  }

  void reconnect() {
    if (!sess_) return;
    std::lock_guard lock(mu_);
    sess_->reconnect();
  }

  void reopen(const std::string& path) {
    std::lock_guard lock(mu_);
    sess_ = std::make_unique<AtSession>(path);
  }

private:
  std::unique_ptr<AtSession> sess_;
  std::mutex mu_;
};

struct FlagGuard {
  std::atomic<bool>& f;
  explicit FlagGuard(std::atomic<bool>& flag) : f(flag) { f.store(true, std::memory_order_relaxed); }
  ~FlagGuard() { f.store(false, std::memory_order_relaxed); }
};

}  // namespace Observer
