#pragma once

/// @file Qmi.hpp
/// @brief Umbrella for the QMI NAS backend (`qcom::qmi`, namespace `QCom::Qmi`).
///
/// Qualcomm-only: Session, NAS cell snapshots, device catalog, CellTracker bridge.
/// Domain types (`CellIdentity`) live in observer::model; this header does not own them.

#include <qcom/qmi/bridge.hpp>
#include <qcom/qmi/callbacks.hpp>
#include <qcom/qmi/device/catalog.hpp>
#include <qcom/qmi/device/dossier.hpp>
#include <qcom/qmi/device/dossier_store.hpp>
#include <qcom/qmi/device/endpoint.hpp>
#include <qcom/qmi/device/port.hpp>
#include <qcom/qmi/device/probe.hpp>
#include <qcom/qmi/device/profile.hpp>
#include <qcom/qmi/error.hpp>
#include <qcom/qmi/health.hpp>
#include <qcom/qmi/health_monitor.hpp>
#include <qcom/qmi/modem_control.hpp>
#include <qcom/qmi/nas_reader.hpp>
#include <qcom/qmi/scan_controller.hpp>
#include <qcom/qmi/session.hpp>
#include <qcom/qmi/settings.hpp>
#include <qcom/qmi/types.hpp>
#include <qcom/qmi/version.hpp>
