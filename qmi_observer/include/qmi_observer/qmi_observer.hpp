#pragma once

/**
 * @file qmi_observer.hpp
 * @brief Umbrella header for the QMI observer library.
 *
 * @defgroup qmi_observer_api qmi_observer
 * @{
 * Layered QMI facade for multi-RAT cell collection:
 * - @ref Session — port + clients
 * - @ref HealthMonitor / @ref evaluate_health — readiness gate
 * - @ref ModemControl — SSP / force search / cautious recover
 * - @ref NasReader — cell location snapshot
 * - @ref ScanController — collect orchestration
 * - @ref to_rrc_envelopes — CellTracker bridge
 * @}
 */

#include "qmi_observer/bridge.hpp"
#include "qmi_observer/callbacks.hpp"
#include "qmi_observer/device/catalog.hpp"
#include "qmi_observer/device/dossier.hpp"
#include "qmi_observer/device/dossier_store.hpp"
#include "qmi_observer/device/endpoint.hpp"
#include "qmi_observer/device/port.hpp"
#include "qmi_observer/device/probe.hpp"
#include "qmi_observer/device/profile.hpp"
#include "qmi_observer/error.hpp"
#include "qmi_observer/health.hpp"
#include "qmi_observer/health_monitor.hpp"
#include "qmi_observer/modem_control.hpp"
#include "qmi_observer/nas_reader.hpp"
#include "qmi_observer/scan_controller.hpp"
#include "qmi_observer/session.hpp"
#include "qmi_observer/settings.hpp"
#include "qmi_observer/types.hpp"
#include "qmi_observer/version.hpp"
