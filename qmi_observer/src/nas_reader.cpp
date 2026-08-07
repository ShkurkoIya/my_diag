#include "qmi_observer/nas_reader.hpp"

#include "qmi_observer/session.hpp"

#include "detail/plmn.hpp"
#include "detail/runtime.hpp"
#include "detail/session_impl.hpp"

#include <libqmi-glib.h>

#include <span>

namespace qmi_observer {
namespace {

std::optional<Plmn> plmn_from_garray(GArray* arr) {
  if (!arr || arr->len < 3) {
    return std::nullopt;
  }
  const auto* bytes = reinterpret_cast<const uint8_t*>(arr->data);
  return detail::plmn_from_bcd(std::span<const uint8_t>(bytes, arr->len));
}

/// LTE RSRP/RSRQ/RSSI in QMI cell-location are typically 0.1 dB units.
float lte_q_to_db(gint16 v) { return static_cast<float>(v) / 10.0f; }

void decode_umts_v2(QmiMessageNasGetCellLocationInfoOutput* out, CellSnapshot& snap) {
  guint16 cell16 = 0;
  GArray* plmn = nullptr;
  guint16 lac = 0;
  guint16 uarfcn = 0;
  guint16 psc = 0;
  gint16 rscp = 0;
  gint16 ecio = 0;
  GArray* neighbors = nullptr;
  GArray* geran = nullptr;
  if (!qmi_message_nas_get_cell_location_info_output_get_umts_info_v2(
          out, &cell16, &plmn, &lac, &uarfcn, &psc, &rscp, &ecio, &neighbors, &geran, nullptr)) {
    return;
  }

  CellObservation serving;
  serving.rat = Rat::Wcdma;
  serving.serving = true;
  serving.plmn = plmn_from_garray(plmn);
  serving.lac_or_tac = lac;
  serving.rf_channel = uarfcn;
  serving.phy_id = psc;
  serving.rsrp_dbm = static_cast<float>(rscp);
  serving.rsrq_db = static_cast<float>(ecio);

  guint32 cell28 = 0;
  if (qmi_message_nas_get_cell_location_info_output_get_umts_cell_id(out, &cell28, nullptr) &&
      cell28 != 0) {
    serving.cell_id = cell28;
  } else if (cell16 != 0) {
    serving.cell_id = cell16;
  }
  snap.cells.push_back(serving);

  if (neighbors) {
    for (guint i = 0; i < neighbors->len; ++i) {
      const auto& n =
          g_array_index(neighbors, QmiMessageNasGetCellLocationInfoOutputUmtsInfoV2CellElement, i);
      CellObservation o;
      o.rat = Rat::Wcdma;
      o.rf_channel = n.utra_absolute_rf_channel_number;
      o.phy_id = n.primary_scrambling_code;
      o.rsrp_dbm = static_cast<float>(n.rscp);
      o.rsrq_db = static_cast<float>(n.ecio);
      o.serving = false;
      snap.cells.push_back(o);
    }
  }
  (void)geran;
}

void decode_lte_intra_v2(QmiMessageNasGetCellLocationInfoOutput* out, CellSnapshot& snap) {
  gboolean idle = FALSE;
  GArray* plmn = nullptr;
  guint16 tac = 0;
  guint32 gci = 0;
  guint16 earfcn = 0;
  guint16 serving_pci = 0;
  guint8 prio = 0, s_non = 0, s_low = 0, s_intra = 0;
  GArray* cells = nullptr;
  if (!qmi_message_nas_get_cell_location_info_output_get_intrafrequency_lte_info_v2(
          out, &idle, &plmn, &tac, &gci, &earfcn, &serving_pci, &prio, &s_non, &s_low, &s_intra,
          &cells, nullptr)) {
    return;
  }

  auto plmn_opt = plmn_from_garray(plmn);
  if (cells) {
    for (guint i = 0; i < cells->len; ++i) {
      const auto& n = g_array_index(
          cells, QmiMessageNasGetCellLocationInfoOutputIntrafrequencyLteInfoV2CellElement, i);
      CellObservation o;
      o.rat = Rat::Lte;
      o.rf_channel = earfcn;
      o.phy_id = n.physical_cell_id;
      o.rsrp_dbm = lte_q_to_db(n.rsrp);
      o.rsrq_db = lte_q_to_db(n.rsrq);
      o.rssi_dbm = lte_q_to_db(n.rssi);
      o.serving = (n.physical_cell_id == serving_pci);
      // PLMN/TAC/CID are serving-only fields in this TLV.
      if (o.serving) {
        o.plmn = plmn_opt;
        o.lac_or_tac = tac;
        if (gci != 0) {
          o.cell_id = gci;
        }
      }
      snap.cells.push_back(o);
    }
  } else if (serving_pci != 0 || gci != 0) {
    CellObservation o;
    o.rat = Rat::Lte;
    o.plmn = plmn_opt;
    o.lac_or_tac = tac;
    o.rf_channel = earfcn;
    o.phy_id = serving_pci;
    if (gci != 0) {
      o.cell_id = gci;
    }
    o.serving = true;
    snap.cells.push_back(o);
  }
  (void)idle;
  (void)prio;
  (void)s_non;
  (void)s_low;
  (void)s_intra;
}

void decode_lte_interfreq(QmiMessageNasGetCellLocationInfoOutput* out, CellSnapshot& snap) {
  gboolean idle = FALSE;
  GArray* frequencies = nullptr;
  if (!qmi_message_nas_get_cell_location_info_output_get_interfrequency_lte_info(
          out, &idle, &frequencies, nullptr) ||
      !frequencies) {
    return;
  }

  for (guint i = 0; i < frequencies->len; ++i) {
    const auto& freq = g_array_index(
        frequencies, QmiMessageNasGetCellLocationInfoOutputInterfrequencyLteInfoFrequencyElement, i);
    if (!freq.cell) {
      continue;
    }
    for (guint j = 0; j < freq.cell->len; ++j) {
      const auto& n = g_array_index(
          freq.cell,
          QmiMessageNasGetCellLocationInfoOutputInterfrequencyLteInfoFrequencyElementCellElement, j);
      CellObservation o;
      o.rat = Rat::Lte;
      o.rf_channel = freq.eutra_absolute_rf_channel_number;
      o.phy_id = n.physical_cell_id;
      o.rsrp_dbm = lte_q_to_db(n.rsrp);
      o.rsrq_db = lte_q_to_db(n.rsrq);
      o.rssi_dbm = lte_q_to_db(n.rssi);
      o.serving = false;
      snap.cells.push_back(o);
    }
  }
  (void)idle;
}

}  // namespace

NasReader::NasReader(Session& session) : session_(session) {}

Result<CellSnapshot> NasReader::snapshot_cells() {
  if (!session_.is_open() || !session_.impl().nas) {
    return Error::from(Errc::NotOpen);
  }

  const guint timeout = detail::timeout_seconds(session_.settings().request_timeout);

  struct Wait {
    GMainLoop* loop{nullptr};
    QmiMessageNasGetCellLocationInfoOutput* out{nullptr};
    GError* error{nullptr};
  } wait{};

  detail::run_main_loop([&](GMainLoop* loop) {
    wait.loop = loop;
    qmi_client_nas_get_cell_location_info(
        session_.impl().nas.get(), nullptr, timeout, nullptr,
        [](GObject* source, GAsyncResult* res, gpointer user_data) {
          auto* w = static_cast<Wait*>(user_data);
          w->out =
              qmi_client_nas_get_cell_location_info_finish(QMI_CLIENT_NAS(source), res, &w->error);
          g_main_loop_quit(w->loop);
        },
        &wait);
  });

  if (!wait.out) {
    if (wait.error && wait.error->code == QMI_PROTOCOL_ERROR_NO_NETWORK_FOUND) {
      const auto err = detail::from_gerror(Errc::NoNetwork, wait.error, "no network found");
      return err;
    }
    return detail::from_gerror(Errc::RequestFailed, wait.error, "cell location failed");
  }

  GError* raw = nullptr;
  if (!qmi_message_nas_get_cell_location_info_output_get_result(wait.out, &raw)) {
    if (raw && raw->code == QMI_PROTOCOL_ERROR_NO_NETWORK_FOUND) {
      qmi_message_nas_get_cell_location_info_output_unref(wait.out);
      return detail::from_gerror(Errc::NoNetwork, raw, "no network found");
    }
    qmi_message_nas_get_cell_location_info_output_unref(wait.out);
    return detail::from_gerror(Errc::RequestFailed, raw, "cell location result failed");
  }

  CellSnapshot snap;
  decode_umts_v2(wait.out, snap);
  decode_lte_intra_v2(wait.out, snap);
  decode_lte_interfreq(wait.out, snap);
  qmi_message_nas_get_cell_location_info_output_unref(wait.out);

  if (session_.callbacks().on_snapshot) {
    session_.callbacks().on_snapshot(snap);
  }
  return snap;
}

Result<NasRadioStatus> NasReader::snapshot_status() {
  if (!session_.is_open() || !session_.impl().nas) {
    return Error::from(Errc::NotOpen);
  }

  const guint timeout = detail::timeout_seconds(session_.settings().request_timeout);
  NasRadioStatus st;

  // ── Serving system ────────────────────────────────────────────────────────
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
    if (!wait.out) {
      if (wait.error) g_error_free(wait.error);
    } else {
      (void)qmi_message_nas_get_serving_system_output_get_result(wait.out, nullptr);
      QmiNasRegistrationState reg = QMI_NAS_REGISTRATION_STATE_UNKNOWN;
      QmiNasAttachState cs = QMI_NAS_ATTACH_STATE_UNKNOWN;
      QmiNasAttachState ps = QMI_NAS_ATTACH_STATE_UNKNOWN;
      GArray* radios = nullptr;
      if (qmi_message_nas_get_serving_system_output_get_serving_system(
              wait.out, &reg, &cs, &ps, nullptr, &radios, nullptr)) {
        if (const char* s = qmi_nas_registration_state_get_string(reg)) st.registration = s;
        if (const char* s = qmi_nas_attach_state_get_string(cs)) st.cs_attach = s;
        if (const char* s = qmi_nas_attach_state_get_string(ps)) st.ps_attach = s;
        if (radios && radios->len > 0) {
          const auto rif = g_array_index(radios, QmiNasRadioInterface, 0);
          if (const char* s = qmi_nas_radio_interface_get_string(rif)) st.radio = s;
        }
      }
      guint16 mcc = 0, mnc = 0;
      const gchar* desc = nullptr;
      if (qmi_message_nas_get_serving_system_output_get_current_plmn(wait.out, &mcc, &mnc, &desc,
                                                                    nullptr)) {
        st.plmn = Plmn{.mcc = mcc, .mnc = mnc, .mnc_digits = 2};
        if (desc && desc[0]) st.plmn_name = desc;
      }
      QmiNasRoamingIndicatorStatus roam = QMI_NAS_ROAMING_INDICATOR_STATUS_OFF;
      if (qmi_message_nas_get_serving_system_output_get_roaming_indicator(wait.out, &roam,
                                                                         nullptr)) {
        st.roaming_indicator = static_cast<uint8_t>(roam);
      }
      qmi_message_nas_get_serving_system_output_unref(wait.out);
    }
  }

  // ── Signal info ───────────────────────────────────────────────────────────
  {
    struct Wait {
      GMainLoop* loop{nullptr};
      QmiMessageNasGetSignalInfoOutput* out{nullptr};
      GError* error{nullptr};
    } wait{};
    detail::run_main_loop([&](GMainLoop* loop) {
      wait.loop = loop;
      qmi_client_nas_get_signal_info(
          session_.impl().nas.get(), nullptr, timeout, nullptr,
          [](GObject* source, GAsyncResult* res, gpointer user_data) {
            auto* w = static_cast<Wait*>(user_data);
            w->out = qmi_client_nas_get_signal_info_finish(QMI_CLIENT_NAS(source), res, &w->error);
            g_main_loop_quit(w->loop);
          },
          &wait);
    });
    if (!wait.out) {
      if (wait.error) g_error_free(wait.error);
    } else {
      (void)qmi_message_nas_get_signal_info_output_get_result(wait.out, nullptr);
      gint8 rssi = 0, rsrq = 0;
      gint16 rsrp = 0, snr = 0;
      if (qmi_message_nas_get_signal_info_output_get_lte_signal_strength(wait.out, &rssi, &rsrq,
                                                                       &rsrp, &snr, nullptr)) {
        st.lte_rssi_dbm = static_cast<float>(rssi);
        st.lte_rsrq_db = static_cast<float>(rsrq);
        st.lte_rsrp_dbm = static_cast<float>(rsrp);
        // libqmi SNR is in 0.1 dB units.
        st.lte_snr_db = static_cast<float>(snr) / 10.0f;
      }
      gint8 w_rssi = 0;
      gint16 w_ecio = 0;
      if (qmi_message_nas_get_signal_info_output_get_wcdma_signal_strength(wait.out, &w_rssi,
                                                                          &w_ecio, nullptr)) {
        st.wcdma_rssi_dbm = static_cast<float>(w_rssi);
        st.wcdma_ecio_db = static_cast<float>(w_ecio) / 10.0f;
      }
      qmi_message_nas_get_signal_info_output_unref(wait.out);
    }
  }

  return st;
}

}  // namespace qmi_observer
