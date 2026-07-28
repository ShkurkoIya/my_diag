/// @file Events.h
/// @brief Stateless event bus for parser -> tracker communication.
///
/// Parsers produce RrcEvent variants, which CellTracker consumes to
/// build and update the cell registry. Each event type maps to a
/// specific 3GPP information element or measurement source.
#pragma once

#include <concepts>
#include <variant>
#include <vector>

#include "CellIdentity.h"

namespace QCom::Events {

/// @brief Cell identity from SIB1 (LTE 36.331 / NR 38.331).
/// Source: BCCH-DL-SCH -> SIB1 -> cellAccessRelatedInfo
struct PassportEvent {
  CellPassport passport;
};

/// @brief Serving cell change notification.
/// Source: ML1 serving cell measurements, RRC Connection Reconfiguration
struct ServingChangedEvent {
  bool is_serving{true};
};

/// @brief Intra-frequency neighbor PCI list from SIB4.
/// Source: BCCH-DL-SCH -> SystemInformation -> SIB4 -> intraFreqNeighCellList
struct IntraNeighborsEvent {
  std::vector<IntraFreqNeighbor> neighbors;
};

/// @brief Inter-frequency carriers from SIB5.
/// Source: BCCH-DL-SCH -> SystemInformation -> SIB5 -> interFreqCarrierFreqList
struct InterFreqCarriersEvent {
  std::vector<InterFreqCarrier> carriers;
};

/// @brief UTRA (WCDMA) neighbor frequencies from SIB6.
/// Source: BCCH-DL-SCH -> SystemInformation -> SIB6
struct UtraNeighborsEvent {
  std::vector<UtraNeighborFreq> neighbors;
};

/// @brief GERAN (GSM) neighbor frequencies from SIB7.
/// Source: BCCH-DL-SCH -> SystemInformation -> SIB7
struct GeranNeighborsEvent {
  std::vector<GeranNeighborFreq> neighbors;
};

/// @brief Live signal quality update.
/// Source: ML1 metrics (0xB193/0xB17F) or MeasurementReport PCell
struct SignalUpdateEvent {
  CellSignal signal;
};

/// @brief Neighbor measurement results from UL-DCCH MeasurementReport.
/// Source: UL-DCCH -> MeasurementReport -> measResultNeighCells
struct NeighborMeasEvent {
  std::vector<NeighborMeasResult> neighbors;
};

/// @brief Generic radio parameters update (SIB2/SIB3/MIB).
/// Templated per RAT so CellTracker can merge into the correct variant.
template <typename T>
concept IsValidRadioParams = std::same_as<T, GsmRadioParams> || std::same_as<T, WcdmaRadioParams> ||
                             std::same_as<T, LteRadioParams> || std::same_as<T, NrRadioParams>;

template <typename T>
  requires IsValidRadioParams<T>
struct RadioParamsEvent {
  T data;
};

using GenericRadioParamsEvent =
    std::variant<RadioParamsEvent<GsmRadioParams>, RadioParamsEvent<WcdmaRadioParams>,
                 RadioParamsEvent<LteRadioParams>, RadioParamsEvent<NrRadioParams>>;

/// @brief Union of all possible parser output events.
using RrcEvent = std::variant<PassportEvent, IntraNeighborsEvent, InterFreqCarriersEvent,
                              UtraNeighborsEvent, GeranNeighborsEvent, GenericRadioParamsEvent,
                              ServingChangedEvent, SignalUpdateEvent, NeighborMeasEvent>;

/// @brief Envelope wrapping an event with routing metadata.
struct RrcEventEnvelope {
  LocalCellKey key;       ///< Physical cell key (freq + PCI) for CellTracker lookup
  RatType rat;            ///< RAT type for variant initialization
  uint64_t timestamp{0};  ///< DIAG packet timestamp
  RrcEvent event_data;
};

}  // namespace QCom::Events
