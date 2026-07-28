#pragma once

#include <map>
#include <mutex>
#include <variant>
#include <vector>

#include "CellIdentity.h"
#include "Events.h"

namespace QCommParser {

class CellTracker {
private:
  mutable std::mutex m_mutex;
  std::map<LocalCellKey, CellIdentity> m_registry;

private:
  template <RatType R>
  void emplace_signal(CellIdentity& cell) {
    cell.signal.signal_data.template emplace<RatSignal_t<R>>();
  }

  void init_cell_signal(CellIdentity& cell) {
    if (cell.signal.signal_data.index() == 0) {
      if (cell.rat == RatType::LTE)
        emplace_signal<RatType::LTE>(cell);
      else if (cell.rat == RatType::NR)
        emplace_signal<RatType::NR>(cell);
      else if (cell.rat == RatType::WCDMA)
        emplace_signal<RatType::WCDMA>(cell);
      else if (cell.rat == RatType::GSM)
        emplace_signal<RatType::GSM>(cell);
    }
  }

  void update_cell_signal(CellIdentity& cell, const CellSignal& new_signal) {
    init_cell_signal(cell);

    std::visit(
        [&](const auto& incoming, auto& target) {
          using S = std::decay_t<decltype(incoming)>;
          using T = std::decay_t<decltype(target)>;

          if constexpr (std::is_same_v<S, T>) {
            if constexpr (requires { target.rsrp; }) {
              target.rsrp = incoming.rsrp;
              target.rsrq = incoming.rsrq;
              target.sinr = incoming.sinr;
            }

            else if constexpr (requires { target.rscp; }) {
              target.rscp = incoming.rscp;
              target.ecio = incoming.ecio;
            }

            else if constexpr (requires { target.rxlev; }) {
              target.rxlev = incoming.rxlev;
              target.rxqual = incoming.rxqual;
            }
          }
        },
        new_signal.signal_data, cell.signal.signal_data);
  }

  template <RatType R>
  void emplace_and_fill(CellIdentity& cell, LocalCellKey key) {
    auto& params = cell.radio.radio_data.template emplace<RatRadio_t<R>>();

    if constexpr (requires { params.earfcn; })
      params.earfcn = key.freq;
    else if constexpr (requires { params.nrarfcn; })
      params.nrarfcn = key.freq;
    else if constexpr (requires { params.uarfcn; })
      params.uarfcn = key.freq;
    else if constexpr (requires { params.arfcn; })
      params.arfcn = key.freq;

    if constexpr (requires { params.pci; })
      params.pci = key.pci_bsic;
    else if constexpr (requires { params.psc; })
      params.psc = key.pci_bsic;
    else if constexpr (requires { params.bsic; })
      params.bsic = key.pci_bsic;
  }

  void init_radio_params(CellIdentity& cell, LocalCellKey key, RatType rat) {
    if (rat == RatType::LTE)
      emplace_and_fill<RatType::LTE>(cell, key);
    else if (rat == RatType::NR)
      emplace_and_fill<RatType::NR>(cell, key);
    else if (rat == RatType::WCDMA)
      emplace_and_fill<RatType::WCDMA>(cell, key);
    else if (rat == RatType::GSM)
      emplace_and_fill<RatType::GSM>(cell, key);
  }

public:
  CellTracker() = default;
  ~CellTracker() = default;

  CellTracker(const CellTracker&) = delete;
  CellTracker& operator=(const CellTracker&) = delete;

  void handle_rrc_event(Events::RrcEventEnvelope&& envelope) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Достаем метаданные из конверта
    auto& cell = m_registry[envelope.key];
    cell.rat = envelope.rat;

    if (cell.radio.radio_data.index() == 0) { init_radio_params(cell, envelope.key, envelope.rat); }

    // Вскрываем variant события
    std::visit(
        [&](const auto& e) {
          using T = std::decay_t<decltype(e)>;

          // Накатываем паспорт
          if constexpr (std::is_same_v<T, Events::PassportEvent>) {
            cell.passport = std::move(e.passport);
          }
          // Внутричастотные соседи
          else if constexpr (std::is_same_v<T, Events::IntraNeighborsEvent>) {
            cell.radio.intra_freq_neighbors = std::move(e.neighbors);
          }
          // Межчастотные соседи
          else if constexpr (std::is_same_v<T, Events::InterNeighborsEvent>) {
            cell.radio.inter_freq_neighbors = std::move(e.neighbors);
          }
          // Serving статус
          else if constexpr (std::is_same_v<T, Events::ServingChangedEvent>) {
            if (e.is_serving) {
              for (auto& [_, c] : m_registry) {
                if (c.rat == envelope.rat) c.is_serving = false;
              }
            }
            cell.is_serving = e.is_serving;
          }
          // Живой сигнал из MeasurementReport отчетов
          else if constexpr (std::is_same_v<T, Events::SignalUpdateEvent>) {
            update_cell_signal(cell, e.signal);
          }
          // Шаблонный визит для радио-порогов!
          else if constexpr (std::is_same_v<T, Events::GenericRadioParamsEvent>) {
            std::visit(
                [&](auto& target_radio, const auto& source_event) {
                  using TargetType = std::decay_t<decltype(target_radio)>;
                  using SourceTemplateType = std::decay_t<decltype(source_event.data)>;

                  if constexpr (std::is_same_v<TargetType, SourceTemplateType>) {
                    if constexpr (requires { target_radio.q_rx_lev_min; }) {
                      target_radio.q_rx_lev_min = source_event.data.q_rx_lev_min;
                      target_radio.q_hyst = source_event.data.q_hyst;
                      target_radio.intra_freq_reselection_allowed =
                          source_event.data.intra_freq_reselection_allowed;

                      if constexpr (requires { target_radio.q_rx_lev_min_offset; }) {
                        target_radio.q_rx_lev_min_offset = source_event.data.q_rx_lev_min_offset;
                      }
                    } else if constexpr (requires { target_radio.q_rx_lev_min_rscp; }) {
                      target_radio.q_rx_lev_min_rscp = source_event.data.q_rx_lev_min_rscp;
                      target_radio.q_qual_min_ecno = source_event.data.q_qual_min_ecno;
                    } else if constexpr (requires { target_radio.rxlev_access_min; }) {
                      target_radio.rxlev_access_min = source_event.data.rxlev_access_min;
                      target_radio.cell_reselect_hysteresis =
                          source_event.data.cell_reselect_hysteresis;
                    }
                  }
                },
                cell.radio.radio_data, e);
          }
        },
        envelope.event_data);
  }

  void handle_l1_signal(LocalCellKey key, RatType rat, CellSignal&& signal) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto& cell = m_registry[key];
    cell.rat = rat;

    update_cell_signal(cell, signal);

    if (cell.radio.radio_data.index() == 0) { init_radio_params(cell, key, rat); }
  }

  void handle_serving_changed(LocalCellKey key, RatType rat) {
    std::lock_guard<std::mutex> lock(m_mutex);

    for (auto& [_, cell] : m_registry) {
      if (cell.rat == rat) { cell.is_serving = false; }
    }

    m_registry[key].is_serving = true;
  }

  void handle_mib_bandwidth(LocalCellKey key, RatType rat, uint8_t dl_mhz) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto& cell = m_registry[key];
    if (cell.radio.radio_data.index() == 0) { init_radio_params(cell, key, rat); }

    // Утиная типизация: вскрываем variant и ищем поле dl_bw на этапе
    // компиляции! [Pages 2]
    std::visit(
        [&](auto& radio_param) {
          if constexpr (requires { radio_param.dl_bw; }) {
            radio_param.dl_bw = dl_mhz;  // Одной строчкой закрыли и LTE, и NR! [Pages 2]
          }
        },
        cell.radio.radio_data);
  }

  std::vector<CellIdentity> get_snapshot() const {
    std::lock_guard<std::mutex> lock(m_mutex);

    std::vector<CellIdentity> snapshot;
    snapshot.reserve(m_registry.size());
    for (const auto& [_, cell] : m_registry) { snapshot.push_back(cell); }
    return snapshot;
  }
};

}  // namespace QCommParser
