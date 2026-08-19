#include <qcom/qmi/health_monitor.hpp>

#include <qcom/qmi/session.hpp>

#include "detail/plmn.hpp"
#include "detail/runtime.hpp"
#include "detail/session_impl.hpp"

#include <libqmi-glib.h>

#include <filesystem>

namespace QCom::Qmi {
namespace {

OperatingModeKind map_op_mode(QmiDmsOperatingMode m) {
  switch (m) {
    case QMI_DMS_OPERATING_MODE_ONLINE: return OperatingModeKind::Online;
    case QMI_DMS_OPERATING_MODE_LOW_POWER:
    case QMI_DMS_OPERATING_MODE_PERSISTENT_LOW_POWER:
    case QMI_DMS_OPERATING_MODE_MODE_ONLY_LOW_POWER:
      return OperatingModeKind::LowPower;
    case QMI_DMS_OPERATING_MODE_OFFLINE: return OperatingModeKind::Offline;
    case QMI_DMS_OPERATING_MODE_RESET: return OperatingModeKind::Reset;
    default: return OperatingModeKind::Other;
  }
}

RegistrationKind map_reg(QmiNasRegistrationState s) {
  switch (s) {
    case QMI_NAS_REGISTRATION_STATE_NOT_REGISTERED: return RegistrationKind::NotRegistered;
    case QMI_NAS_REGISTRATION_STATE_REGISTERED: return RegistrationKind::Registered;
    case QMI_NAS_REGISTRATION_STATE_NOT_REGISTERED_SEARCHING: return RegistrationKind::Searching;
    case QMI_NAS_REGISTRATION_STATE_REGISTRATION_DENIED: return RegistrationKind::Denied;
    default: return RegistrationKind::Unknown;
  }
}

Rat map_radio(QmiNasRadioInterface rif) {
  switch (rif) {
    case QMI_NAS_RADIO_INTERFACE_GSM: return Rat::Gsm;
    case QMI_NAS_RADIO_INTERFACE_UMTS: return Rat::Wcdma;
    case QMI_NAS_RADIO_INTERFACE_LTE: return Rat::Lte;
    case QMI_NAS_RADIO_INTERFACE_5GNR: return Rat::Nr;
    case QMI_NAS_RADIO_INTERFACE_NONE: return Rat::Unknown;
    default: return Rat::Unknown;
  }
}

std::vector<Rat> modes_from_mask(QmiNasRatModePreference mask) {
  std::vector<Rat> out;
  if (mask & QMI_NAS_RAT_MODE_PREFERENCE_GSM) out.push_back(Rat::Gsm);
  if (mask & QMI_NAS_RAT_MODE_PREFERENCE_UMTS) out.push_back(Rat::Wcdma);
  if (mask & QMI_NAS_RAT_MODE_PREFERENCE_LTE) out.push_back(Rat::Lte);
  if (mask & QMI_NAS_RAT_MODE_PREFERENCE_5GNR) out.push_back(Rat::Nr);
  return out;
}

}  // namespace

HealthMonitor::HealthMonitor(Session& session) : session_(session) {}

Result<ModemHealth> HealthMonitor::refresh() {
  HealthProbe probe;
  probe.session_open = session_.is_open();
  probe.device_node_present =
      !session_.settings().device_path.empty() &&
      std::filesystem::exists(session_.settings().device_path);

  if (!session_.is_open()) {
    auto h = evaluate_health(std::move(probe));
    session_.impl().publish_health(h);
    return h;
  }

  const guint timeout = detail::timeout_seconds(session_.settings().request_timeout);

  // DMS operating mode
  {
    struct Wait {
      GMainLoop* loop{nullptr};
      QmiMessageDmsGetOperatingModeOutput* out{nullptr};
      GError* error{nullptr};
    } wait{};
    detail::run_main_loop([&](GMainLoop* loop) {
      wait.loop = loop;
      qmi_client_dms_get_operating_mode(
          session_.impl().dms.get(), nullptr, timeout, nullptr,
          [](GObject* source, GAsyncResult* res, gpointer user_data) {
            auto* w = static_cast<Wait*>(user_data);
            w->out =
                qmi_client_dms_get_operating_mode_finish(QMI_CLIENT_DMS(source), res, &w->error);
            g_main_loop_quit(w->loop);
          },
          &wait);
    });
    if (wait.out) {
      QmiDmsOperatingMode mode = QMI_DMS_OPERATING_MODE_UNKNOWN;
      qmi_message_dms_get_operating_mode_output_get_result(wait.out, nullptr);
      qmi_message_dms_get_operating_mode_output_get_mode(wait.out, &mode, nullptr);
      probe.operating_mode = map_op_mode(mode);
      qmi_message_dms_get_operating_mode_output_unref(wait.out);
    } else if (wait.error) {
      probe.last_error = wait.error->message;
      g_error_free(wait.error);
    }
  }

  // Serving system
  {
    struct Wait {
      GMainLoop* loop{nullptr};
      QmiMessageNasGetServingSystemOutput* out{nullptr};
      GError* error{nullptr};
    } wait{};
    detail::run_main_loop([&](GMainLoop* loop) {
      wait.loop = loop;
      qmi_client_nas_get_serving_system(
          session_.impl().nas.get(), nullptr, timeout, nullptr,
          [](GObject* source, GAsyncResult* res, gpointer user_data) {
            auto* w = static_cast<Wait*>(user_data);
            w->out =
                qmi_client_nas_get_serving_system_finish(QMI_CLIENT_NAS(source), res, &w->error);
            g_main_loop_quit(w->loop);
          },
          &wait);
    });
    if (wait.out) {
      QmiNasRegistrationState reg = QMI_NAS_REGISTRATION_STATE_UNKNOWN;
      GArray* radios = nullptr;
      qmi_message_nas_get_serving_system_output_get_result(wait.out, nullptr);
      qmi_message_nas_get_serving_system_output_get_serving_system(wait.out, &reg, nullptr,
                                                                   nullptr, nullptr, &radios,
                                                                   nullptr);
      probe.registration = map_reg(reg);
      if (radios && radios->len > 0) {
        const auto rif = g_array_index(radios, QmiNasRadioInterface, 0);
        probe.radio = map_radio(rif);
      }
      guint16 mcc = 0, mnc = 0;
      const gchar* desc = nullptr;
      if (qmi_message_nas_get_serving_system_output_get_current_plmn(wait.out, &mcc, &mnc, &desc,
                                                                    nullptr)) {
        probe.current_plmn = Plmn{.mcc = mcc, .mnc = mnc, .mnc_digits = 2};
      }
      qmi_message_nas_get_serving_system_output_unref(wait.out);
    } else if (wait.error) {
      probe.last_error = wait.error->message;
      g_error_free(wait.error);
    }
  }

  // Home network (best-effort)
  {
    struct Wait {
      GMainLoop* loop{nullptr};
      QmiMessageNasGetHomeNetworkOutput* out{nullptr};
      GError* error{nullptr};
    } wait{};
    detail::run_main_loop([&](GMainLoop* loop) {
      wait.loop = loop;
      qmi_client_nas_get_home_network(
          session_.impl().nas.get(), nullptr, timeout, nullptr,
          [](GObject* source, GAsyncResult* res, gpointer user_data) {
            auto* w = static_cast<Wait*>(user_data);
            w->out = qmi_client_nas_get_home_network_finish(QMI_CLIENT_NAS(source), res, &w->error);
            g_main_loop_quit(w->loop);
          },
          &wait);
    });
    if (wait.out) {
      guint16 mcc = 0, mnc = 0;
      const gchar* desc = nullptr;
      qmi_message_nas_get_home_network_output_get_result(wait.out, nullptr);
      if (qmi_message_nas_get_home_network_output_get_home_network(wait.out, &mcc, &mnc, &desc,
                                                                  nullptr)) {
        probe.home_plmn = Plmn{.mcc = mcc, .mnc = mnc, .mnc_digits = 2};
      }
      qmi_message_nas_get_home_network_output_unref(wait.out);
    } else if (wait.error) {
      g_error_free(wait.error);
    }
  }

  // Mode preference
  {
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
    if (wait.out) {
      QmiNasRatModePreference mask{};
      qmi_message_nas_get_system_selection_preference_output_get_result(wait.out, nullptr);
      qmi_message_nas_get_system_selection_preference_output_get_mode_preference(wait.out, &mask,
                                                                                 nullptr);
      probe.mode_preference = modes_from_mask(mask);
      qmi_message_nas_get_system_selection_preference_output_unref(wait.out);
    } else if (wait.error) {
      g_error_free(wait.error);
    }
  }

  // SIM: leave unknown unless we add UIM later — do not block scans falsely.
  probe.sim_known = false;
  probe.sim_ready = true;

  auto health = evaluate_health(std::move(probe));
  session_.impl().publish_health(health);
  return health;
}

}  // namespace QCom::Qmi
