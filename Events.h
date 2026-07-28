#pragma once

#include <concepts>
#include <variant>
#include <vector>

#include "CellIdentity.h"

namespace QCommParser::Events {

// Из SIB1 прилетел юридический паспорт вышки
struct PassportEvent {
  CellPassport passport;
};

// Изменение статуса Serving соты
struct ServingChangedEvent {
  bool is_serving{true};
};

// Из SIB4 прилетели внутричастотные соседи
struct IntraNeighborsEvent {
  std::vector<uint16_t> neighbors;
};

struct InterNeighborsEvent {
  std::vector<InterFreqNeighbor> neighbors;
};

struct SignalUpdateEvent {
  CellSignal signal;
};

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

using RrcEvent = std::variant<PassportEvent, IntraNeighborsEvent, InterNeighborsEvent,
                              GenericRadioParamsEvent, ServingChangedEvent, SignalUpdateEvent>;

struct RrcEventEnvelope {
  LocalCellKey key;
  RatType rat;
  uint64_t timestamp;
  RrcEvent event_data;
};

}  // namespace QCommParser::Events
