#include "qmi_observer/modem_control.hpp"

#include "qmi_observer/session.hpp"

#include "detail/plmn.hpp"
#include "detail/runtime.hpp"
#include "detail/session_impl.hpp"

#include <libqmi-glib.h>

#include <chrono>
#include <thread>

namespace qmi_observer {
namespace {

QmiNasRatModePreference to_qmi_modes(const std::vector<Rat>& modes) {
  QmiNasRatModePreference mask = static_cast<QmiNasRatModePreference>(0);
  for (Rat r : modes) {
    switch (r) {
      case Rat::Gsm:
        mask = static_cast<QmiNasRatModePreference>(mask | QMI_NAS_RAT_MODE_PREFERENCE_GSM);
        break;
      case Rat::Wcdma:
        mask = static_cast<QmiNasRatModePreference>(mask | QMI_NAS_RAT_MODE_PREFERENCE_UMTS);
        break;
      case Rat::Lte:
        mask = static_cast<QmiNasRatModePreference>(mask | QMI_NAS_RAT_MODE_PREFERENCE_LTE);
        break;
      case Rat::Nr:
        mask = static_cast<QmiNasRatModePreference>(mask | QMI_NAS_RAT_MODE_PREFERENCE_5GNR);
        break;
      default:
        break;
    }
  }
  return mask;
}

std::vector<Rat> from_qmi_modes(QmiNasRatModePreference mask) {
  std::vector<Rat> out;
  if (mask & QMI_NAS_RAT_MODE_PREFERENCE_GSM) {
    out.push_back(Rat::Gsm);
  }
  if (mask & QMI_NAS_RAT_MODE_PREFERENCE_UMTS) {
    out.push_back(Rat::Wcdma);
  }
  if (mask & QMI_NAS_RAT_MODE_PREFERENCE_LTE) {
    out.push_back(Rat::Lte);
  }
  if (mask & QMI_NAS_RAT_MODE_PREFERENCE_5GNR) {
    out.push_back(Rat::Nr);
  }
  return out;
}

}  // namespace

ModemControl::ModemControl(Session& session) : session_(session) {}

Result<void> ModemControl::set_mode_preference(const std::vector<Rat>& modes) {
  if (!session_.is_open()) {
    return Error::from(Errc::NotOpen);
  }
  const auto mask = to_qmi_modes(modes);
  if (static_cast<unsigned>(mask) == 0) {
    return Error::from(Errc::InvalidArgument, "empty mode preference");
  }

  const guint timeout = detail::timeout_seconds(session_.settings().request_timeout);
  g_autoptr(QmiMessageNasSetSystemSelectionPreferenceInput) input =
      qmi_message_nas_set_system_selection_preference_input_new();
  GError* raw = nullptr;
  if (!qmi_message_nas_set_system_selection_preference_input_set_mode_preference(input, mask,
                                                                                 &raw)) {
    return detail::from_gerror(Errc::InvalidArgument, raw, "set mode preference TLV failed");
  }

  struct Wait {
    GMainLoop* loop{nullptr};
    QmiMessageNasSetSystemSelectionPreferenceOutput* out{nullptr};
    GError* error{nullptr};
  } wait{};

  detail::run_main_loop([&](GMainLoop* loop) {
    wait.loop = loop;
    qmi_client_nas_set_system_selection_preference(
        session_.impl().nas.get(), input, timeout, nullptr,
        [](GObject* source, GAsyncResult* res, gpointer user_data) {
          auto* w = static_cast<Wait*>(user_data);
          w->out = qmi_client_nas_set_system_selection_preference_finish(QMI_CLIENT_NAS(source),
                                                                         res, &w->error);
          g_main_loop_quit(w->loop);
        },
        &wait);
  });

  if (!wait.out) {
    return detail::from_gerror(Errc::RequestFailed, wait.error, "set SSP failed");
  }
  raw = nullptr;
  if (!qmi_message_nas_set_system_selection_preference_output_get_result(wait.out, &raw)) {
    qmi_message_nas_set_system_selection_preference_output_unref(wait.out);
    return detail::from_gerror(Errc::RequestFailed, raw, "set SSP result failed");
  }
  qmi_message_nas_set_system_selection_preference_output_unref(wait.out);
  return Result<void>::success();
}

Result<std::vector<Rat>> ModemControl::get_mode_preference() {
  if (!session_.is_open()) {
    return Error::from(Errc::NotOpen);
  }
  const guint timeout = detail::timeout_seconds(session_.settings().request_timeout);

  struct Wait {
    GMainLoop* loop{nullptr};
    QmiMessageNasGetSystemSelectionPreferenceOutput* out{nullptr};
    GError* error{nullptr};
  } wait{};

  detail::run_main_loop([&](GMainLoop* loop) {
    wait.loop = loop;
    qmi_client_nas_get_system_selection_preference(
        session_.impl().nas.get(), nullptr, timeout, nullptr,
        [](GObject* source, GAsyncResult* res, gpointer user_data) {
          auto* w = static_cast<Wait*>(user_data);
          w->out = qmi_client_nas_get_system_selection_preference_finish(QMI_CLIENT_NAS(source),
                                                                         res, &w->error);
          g_main_loop_quit(w->loop);
        },
        &wait);
  });
  if (!wait.out) {
    return detail::from_gerror(Errc::RequestFailed, wait.error, "get SSP failed");
  }

  QmiNasRatModePreference mask{};
  GError* raw = nullptr;
  if (!qmi_message_nas_get_system_selection_preference_output_get_result(wait.out, &raw)) {
    qmi_message_nas_get_system_selection_preference_output_unref(wait.out);
    return detail::from_gerror(Errc::RequestFailed, raw, "get SSP result failed");
  }
  qmi_message_nas_get_system_selection_preference_output_get_mode_preference(wait.out, &mask,
                                                                             nullptr);
  qmi_message_nas_get_system_selection_preference_output_unref(wait.out);
  return from_qmi_modes(mask);
}

Result<void> ModemControl::force_network_search() {
  if (!session_.is_open()) {
    return Error::from(Errc::NotOpen);
  }
  const guint timeout = detail::timeout_seconds(session_.settings().request_timeout);

  struct Wait {
    GMainLoop* loop{nullptr};
    QmiMessageNasForceNetworkSearchOutput* out{nullptr};
    GError* error{nullptr};
  } wait{};

  detail::run_main_loop([&](GMainLoop* loop) {
    wait.loop = loop;
    qmi_client_nas_force_network_search(
        session_.impl().nas.get(), nullptr, timeout, nullptr,
        [](GObject* source, GAsyncResult* res, gpointer user_data) {
          auto* w = static_cast<Wait*>(user_data);
          w->out =
              qmi_client_nas_force_network_search_finish(QMI_CLIENT_NAS(source), res, &w->error);
          g_main_loop_quit(w->loop);
        },
        &wait);
  });
  if (!wait.out) {
    return detail::from_gerror(Errc::RequestFailed, wait.error, "force search failed");
  }
  GError* raw = nullptr;
  if (!qmi_message_nas_force_network_search_output_get_result(wait.out, &raw)) {
    qmi_message_nas_force_network_search_output_unref(wait.out);
    return detail::from_gerror(Errc::RequestFailed, raw, "force search result failed");
  }
  qmi_message_nas_force_network_search_output_unref(wait.out);
  return Result<void>::success();
}

Result<void> ModemControl::try_recover_online() {
  if (!session_.is_open()) {
    return Error::from(Errc::NotOpen);
  }
  if (!session_.settings().allow_dms_offline) {
    // Safe path only: low-power → online (never offline).
  }
  const guint timeout = detail::timeout_seconds(session_.settings().request_timeout);

  auto set_mode = [&](QmiDmsOperatingMode mode) -> Result<void> {
    g_autoptr(QmiMessageDmsSetOperatingModeInput) input =
        qmi_message_dms_set_operating_mode_input_new();
    GError* raw = nullptr;
    if (!qmi_message_dms_set_operating_mode_input_set_mode(input, mode, &raw)) {
      return detail::from_gerror(Errc::InvalidArgument, raw, "set operating mode TLV failed");
    }
    struct Wait {
      GMainLoop* loop{nullptr};
      QmiMessageDmsSetOperatingModeOutput* out{nullptr};
      GError* error{nullptr};
    } wait{};
    detail::run_main_loop([&](GMainLoop* loop) {
      wait.loop = loop;
      qmi_client_dms_set_operating_mode(
          session_.impl().dms.get(), input, timeout, nullptr,
          [](GObject* source, GAsyncResult* res, gpointer user_data) {
            auto* w = static_cast<Wait*>(user_data);
            w->out =
                qmi_client_dms_set_operating_mode_finish(QMI_CLIENT_DMS(source), res, &w->error);
            g_main_loop_quit(w->loop);
          },
          &wait);
    });
    if (!wait.out) {
      return detail::from_gerror(Errc::NeedsRecover, wait.error, "set operating mode failed");
    }
    raw = nullptr;
    if (!qmi_message_dms_set_operating_mode_output_get_result(wait.out, &raw)) {
      qmi_message_dms_set_operating_mode_output_unref(wait.out);
      return detail::from_gerror(Errc::NeedsRecover, raw, "set operating mode result failed");
    }
    qmi_message_dms_set_operating_mode_output_unref(wait.out);
    return Result<void>::success();
  };

  // Prefer low-power bounce; avoids the offline InvalidTransition trap.
  if (auto r = set_mode(QMI_DMS_OPERATING_MODE_LOW_POWER); !r) {
    return r.error();
  }
  std::this_thread::sleep_for(std::chrono::seconds(2));
  return set_mode(QMI_DMS_OPERATING_MODE_ONLINE);
}

}  // namespace qmi_observer
