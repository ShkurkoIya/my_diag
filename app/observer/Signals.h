/// @file Signals.h
/// @brief Process-wide stop flags so SIGINT/SIGTERM abort AT waits and worker threads.
#pragma once

#include <atomic>
#include <csignal>

namespace Observer {

inline std::atomic<bool> g_user_stop{false};
inline std::atomic<bool>* g_hop_stop_ptr{nullptr};
inline std::atomic<bool>* g_qmi_stop_ptr{nullptr};
inline std::atomic<bool>* g_cereg_stop_ptr{nullptr};
inline std::atomic<bool>* g_rat_guard_stop_ptr{nullptr};
inline std::atomic<bool>* g_at_only_stop_ptr{nullptr};

extern "C" inline void live_scanner_on_signal(int) {
  g_user_stop.store(true, std::memory_order_relaxed);
  if (auto* p = g_hop_stop_ptr) p->store(true, std::memory_order_relaxed);
  if (auto* p = g_qmi_stop_ptr) p->store(true, std::memory_order_relaxed);
  if (auto* p = g_cereg_stop_ptr) p->store(true, std::memory_order_relaxed);
  if (auto* p = g_rat_guard_stop_ptr) p->store(true, std::memory_order_relaxed);
  if (auto* p = g_at_only_stop_ptr) p->store(true, std::memory_order_relaxed);
}

inline void install_stop_handlers() {
  std::signal(SIGINT, live_scanner_on_signal);
  std::signal(SIGTERM, live_scanner_on_signal);
}

}  // namespace Observer
