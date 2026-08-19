#pragma once

#include <glib.h>

#include <functional>
#include <memory>

namespace QCom::Qmi::detail {

struct GErrorDeleter {
  void operator()(GError* e) const noexcept {
    if (e) {
      g_error_free(e);
    }
  }
};

using GErrorPtr = std::unique_ptr<GError, GErrorDeleter>;

template <typename T>
struct GObjectDeleter {
  void operator()(T* obj) const noexcept {
    if (obj) {
      g_object_unref(obj);
    }
  }
};

template <typename T>
using GObjectPtr = std::unique_ptr<T, GObjectDeleter<T>>;

inline void run_main_loop(const std::function<void(GMainLoop*)>& kickoff) {
  g_autoptr(GMainLoop) loop = g_main_loop_new(nullptr, FALSE);
  kickoff(loop);
  g_main_loop_run(loop);
}

}  // namespace QCom::Qmi::detail
