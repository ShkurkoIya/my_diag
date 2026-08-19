#include <qcom/qmi/session.hpp>

#include <qcom/qmi/health_monitor.hpp>
#include <qcom/qmi/modem_control.hpp>
#include <qcom/qmi/nas_reader.hpp>
#include <qcom/qmi/scan_controller.hpp>

#include "detail/plmn.hpp"
#include "detail/runtime.hpp"
#include "detail/session_impl.hpp"

#include <libqmi-glib.h>

#include <filesystem>
#include <utility>

namespace QCom::Qmi {

Session::Session(Settings settings) : impl_(std::make_unique<Impl>()) {
  impl_->settings = std::move(settings);
  impl_->health_facade = std::make_unique<HealthMonitor>(*this);
  impl_->control_facade = std::make_unique<ModemControl>(*this);
  impl_->nas_facade = std::make_unique<NasReader>(*this);
  impl_->scan_facade = std::make_unique<ScanController>(*this);
}

Session::~Session() { close(); }

Session::Session(Session&&) noexcept = default;
Session& Session::operator=(Session&&) noexcept = default;

const Settings& Session::settings() const noexcept { return impl_->settings; }
Settings& Session::settings() noexcept { return impl_->settings; }

void Session::set_callbacks(Callbacks cb) { impl_->callbacks = std::move(cb); }
const Callbacks& Session::callbacks() const noexcept { return impl_->callbacks; }

bool Session::is_open() const noexcept { return impl_ && impl_->open; }

Session::Impl& Session::impl() { return *impl_; }
const Session::Impl& Session::impl() const { return *impl_; }

HealthMonitor& Session::health() { return *impl_->health_facade; }
ModemControl& Session::control() { return *impl_->control_facade; }
NasReader& Session::nas() { return *impl_->nas_facade; }
ScanController& Session::scan() { return *impl_->scan_facade; }

const ModemHealth& Session::last_health() const noexcept { return impl_->last_health; }

Result<void> Session::open() {
  if (!impl_) {
    return Error::from(Errc::Internal, "null impl");
  }
  if (impl_->open) {
    return Error::from(Errc::AlreadyOpen);
  }
  if (impl_->settings.device_path.empty()) {
    return Error::from(Errc::InvalidArgument, "settings.device_path is empty");
  }
  if (!std::filesystem::exists(impl_->settings.device_path)) {
    return Error::from(Errc::DeviceOpenFailed, "device node missing: " + impl_->settings.device_path);
  }

  const guint timeout = detail::timeout_seconds(impl_->settings.request_timeout);

  struct DeviceNewWait {
    GMainLoop* loop{nullptr};
    QmiDevice* device{nullptr};
    GError* error{nullptr};
  } created{};

  detail::run_main_loop([&](GMainLoop* loop) {
    created.loop = loop;
    g_autoptr(GFile) file = g_file_new_for_path(impl_->settings.device_path.c_str());
    qmi_device_new(
        file, nullptr,
        [](GObject*, GAsyncResult* res, gpointer user_data) {
          auto* w = static_cast<DeviceNewWait*>(user_data);
          w->device = qmi_device_new_finish(res, &w->error);
          g_main_loop_quit(w->loop);
        },
        &created);
  });
  if (!created.device) {
    auto err = detail::from_gerror(Errc::DeviceOpenFailed, created.error, "qmi_device_new failed");
    impl_->emit_error(err);
    return err;
  }
  impl_->device.reset(created.device);

  QmiDeviceOpenFlags flags = QMI_DEVICE_OPEN_FLAGS_NONE;
  if (impl_->settings.use_proxy) {
    flags = static_cast<QmiDeviceOpenFlags>(flags | QMI_DEVICE_OPEN_FLAGS_PROXY);
  }
  if (impl_->settings.open_auto) {
    flags = static_cast<QmiDeviceOpenFlags>(flags | QMI_DEVICE_OPEN_FLAGS_AUTO);
  }

  struct BoolWait {
    GMainLoop* loop{nullptr};
    gboolean ok{FALSE};
    GError* error{nullptr};
  } opened{};

  detail::run_main_loop([&](GMainLoop* loop) {
    opened.loop = loop;
    qmi_device_open(
        impl_->device.get(), flags, timeout, nullptr,
        [](GObject* source, GAsyncResult* res, gpointer user_data) {
          auto* w = static_cast<BoolWait*>(user_data);
          w->ok = qmi_device_open_finish(QMI_DEVICE(source), res, &w->error);
          g_main_loop_quit(w->loop);
        },
        &opened);
  });
  if (!opened.ok) {
    auto err = detail::from_gerror(Errc::DeviceOpenFailed, opened.error, "qmi_device_open failed");
    impl_->device.reset();
    impl_->emit_error(err);
    return err;
  }

  auto alloc_service = [&](QmiService svc) -> Result<QmiClient*> {
    struct ClientWait {
      GMainLoop* loop{nullptr};
      QmiClient* client{nullptr};
      GError* error{nullptr};
    } wait{};
    detail::run_main_loop([&](GMainLoop* loop) {
      wait.loop = loop;
      qmi_device_allocate_client(
          impl_->device.get(), svc, QMI_CID_NONE, timeout, nullptr,
          [](GObject* source, GAsyncResult* res, gpointer user_data) {
            auto* w = static_cast<ClientWait*>(user_data);
            w->client = qmi_device_allocate_client_finish(QMI_DEVICE(source), res, &w->error);
            g_main_loop_quit(w->loop);
          },
          &wait);
    });
    if (!wait.client) {
      return detail::from_gerror(Errc::ClientAllocFailed, wait.error, "allocate client failed");
    }
    return wait.client;
  };

  if (auto dms = alloc_service(QMI_SERVICE_DMS); dms) {
    impl_->dms.reset(QMI_CLIENT_DMS(dms.value()));
  } else {
    close();
    impl_->emit_error(dms.error());
    return dms.error();
  }

  if (auto nas = alloc_service(QMI_SERVICE_NAS); nas) {
    impl_->nas.reset(QMI_CLIENT_NAS(nas.value()));
  } else {
    close();
    impl_->emit_error(nas.error());
    return nas.error();
  }

  impl_->open = true;
  return Result<void>::success();
}

void Session::close() {
  if (!impl_) {
    return;
  }

  const guint timeout = detail::timeout_seconds(impl_->settings.request_timeout);

  auto release = [&](QmiClient* client) {
    if (!client || !impl_->device) {
      return;
    }
    struct BoolWait {
      GMainLoop* loop{nullptr};
      gboolean ok{FALSE};
      GError* error{nullptr};
    } released{};
    detail::run_main_loop([&](GMainLoop* loop) {
      released.loop = loop;
      qmi_device_release_client(
          impl_->device.get(), client, QMI_DEVICE_RELEASE_CLIENT_FLAGS_RELEASE_CID, timeout,
          nullptr,
          [](GObject* source, GAsyncResult* res, gpointer user_data) {
            auto* w = static_cast<BoolWait*>(user_data);
            w->ok = qmi_device_release_client_finish(QMI_DEVICE(source), res, &w->error);
            g_main_loop_quit(w->loop);
          },
          &released);
    });
    if (released.error) {
      g_error_free(released.error);
    }
  };

  if (impl_->nas) {
    release(QMI_CLIENT(impl_->nas.get()));
    impl_->nas.reset();
  }
  if (impl_->dms) {
    release(QMI_CLIENT(impl_->dms.get()));
    impl_->dms.reset();
  }
  if (impl_->device) {
    struct BoolWait {
      GMainLoop* loop{nullptr};
      gboolean ok{FALSE};
      GError* error{nullptr};
    } closed{};
    detail::run_main_loop([&](GMainLoop* loop) {
      closed.loop = loop;
      qmi_device_close_async(
          impl_->device.get(), timeout, nullptr,
          [](GObject* source, GAsyncResult* res, gpointer user_data) {
            auto* w = static_cast<BoolWait*>(user_data);
            w->ok = qmi_device_close_finish(QMI_DEVICE(source), res, &w->error);
            g_main_loop_quit(w->loop);
          },
          &closed);
    });
    if (closed.error) {
      g_error_free(closed.error);
    }
    impl_->device.reset();
  }
  impl_->open = false;
}

Result<ModemIdentity> Session::identity() {
  if (!is_open() || !impl_->dms) {
    return Error::from(Errc::NotOpen);
  }
  const guint timeout = detail::timeout_seconds(impl_->settings.request_timeout);
  ModemIdentity id;

  struct ManWait {
    GMainLoop* loop{nullptr};
    QmiMessageDmsGetManufacturerOutput* out{nullptr};
    GError* error{nullptr};
  } man{};
  detail::run_main_loop([&](GMainLoop* loop) {
    man.loop = loop;
    qmi_client_dms_get_manufacturer(
        impl_->dms.get(), nullptr, timeout, nullptr,
        [](GObject* source, GAsyncResult* res, gpointer user_data) {
          auto* w = static_cast<ManWait*>(user_data);
          w->out = qmi_client_dms_get_manufacturer_finish(QMI_CLIENT_DMS(source), res, &w->error);
          g_main_loop_quit(w->loop);
        },
        &man);
  });
  if (!man.out) {
    return detail::from_gerror(Errc::RequestFailed, man.error, "get manufacturer failed");
  }
  {
    const gchar* v = nullptr;
    qmi_message_dms_get_manufacturer_output_get_result(man.out, nullptr);
    qmi_message_dms_get_manufacturer_output_get_manufacturer(man.out, &v, nullptr);
    if (v) {
      id.manufacturer = v;
    }
    qmi_message_dms_get_manufacturer_output_unref(man.out);
  }

  struct ModelWait {
    GMainLoop* loop{nullptr};
    QmiMessageDmsGetModelOutput* out{nullptr};
    GError* error{nullptr};
  } model{};
  detail::run_main_loop([&](GMainLoop* loop) {
    model.loop = loop;
    qmi_client_dms_get_model(
        impl_->dms.get(), nullptr, timeout, nullptr,
        [](GObject* source, GAsyncResult* res, gpointer user_data) {
          auto* w = static_cast<ModelWait*>(user_data);
          w->out = qmi_client_dms_get_model_finish(QMI_CLIENT_DMS(source), res, &w->error);
          g_main_loop_quit(w->loop);
        },
        &model);
  });
  if (!model.out) {
    return detail::from_gerror(Errc::RequestFailed, model.error, "get model failed");
  }
  {
    const gchar* v = nullptr;
    qmi_message_dms_get_model_output_get_result(model.out, nullptr);
    qmi_message_dms_get_model_output_get_model(model.out, &v, nullptr);
    if (v) {
      id.model = v;
    }
    qmi_message_dms_get_model_output_unref(model.out);
  }

  struct RevWait {
    GMainLoop* loop{nullptr};
    QmiMessageDmsGetRevisionOutput* out{nullptr};
    GError* error{nullptr};
  } rev{};
  detail::run_main_loop([&](GMainLoop* loop) {
    rev.loop = loop;
    qmi_client_dms_get_revision(
        impl_->dms.get(), nullptr, timeout, nullptr,
        [](GObject* source, GAsyncResult* res, gpointer user_data) {
          auto* w = static_cast<RevWait*>(user_data);
          w->out = qmi_client_dms_get_revision_finish(QMI_CLIENT_DMS(source), res, &w->error);
          g_main_loop_quit(w->loop);
        },
        &rev);
  });
  if (!rev.out) {
    return detail::from_gerror(Errc::RequestFailed, rev.error, "get revision failed");
  }
  {
    const gchar* v = nullptr;
    qmi_message_dms_get_revision_output_get_result(rev.out, nullptr);
    qmi_message_dms_get_revision_output_get_revision(rev.out, &v, nullptr);
    if (v) {
      id.revision = v;
    }
    qmi_message_dms_get_revision_output_unref(rev.out);
  }

  return id;
}

Result<ModemHealth> Session::refresh_health() { return health().refresh(); }

}  // namespace QCom::Qmi
