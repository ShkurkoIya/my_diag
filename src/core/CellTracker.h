/// @file CellTracker.h
/// @brief Single source of truth for cell state, aggregated from parser events.
#pragma once

#include <map>
#include <mutex>
#include <variant>
#include <vector>

#include "core/CellIdentity.h"
#include "core/Events.h"

namespace QCom {

class CellTracker {
public:
  CellTracker() = default;
  ~CellTracker() = default;

  CellTracker(const CellTracker&) = delete;
  CellTracker& operator=(const CellTracker&) = delete;

  void handle_rrc_event(Events::RrcEventEnvelope&& envelope) {
    std::lock_guard lock(m_mutex);

    auto& cell = m_registry[envelope.key];
    cell.rat = envelope.rat;

    if (cell.radio.radio_data.index() == 0) { init_radio_params(cell, envelope.key, envelope.rat); }

    std::visit(
        [&](const auto& e) {
          using T = std::decay_t<decltype(e)>;

          if constexpr (std::is_same_v<T, Events::PassportEvent>) {
            cell.passport = e.passport;
          } else if constexpr (std::is_same_v<T, Events::IntraNeighborsEvent>) {
            cell.radio.intra_freq_neighbors = e.neighbors;
          } else if constexpr (std::is_same_v<T, Events::InterFreqCarriersEvent>) {
            cell.radio.inter_freq_carriers = e.carriers;
          } else if constexpr (std::is_same_v<T, Events::UtraNeighborsEvent>) {
            cell.radio.utra_neighbors = e.neighbors;
          } else if constexpr (std::is_same_v<T, Events::GeranNeighborsEvent>) {
            cell.radio.geran_neighbors = e.neighbors;
          } else if constexpr (std::is_same_v<T, Events::ServingChangedEvent>) {
            if (e.is_serving) {
              for (auto& [_, c] : m_registry) {
                if (c.rat == envelope.rat) c.is_serving = false;
              }
            }
            cell.is_serving = e.is_serving;
          } else if constexpr (std::is_same_v<T, Events::SignalUpdateEvent>) {
            update_cell_signal(cell, e.signal);
          } else if constexpr (std::is_same_v<T, Events::NeighborMeasEvent>) {
            cell.radio.meas_neighbors = e.neighbors;
          } else if constexpr (std::is_same_v<T, Events::GsmNeighborsEvent>) {
            cell.radio.gsm_neighbors = e.neighbors;
          } else if constexpr (std::is_same_v<T, Events::WcdmaNeighborsEvent>) {
            cell.radio.wcdma_neighbors = e.neighbors;
          } else if constexpr (std::is_same_v<T, Events::GenericRadioParamsEvent>) {
            apply_radio_params(cell, e);
          }
        },
        envelope.event_data);
  }

  void handle_l1_signal(LocalCellKey key, RatType rat, CellSignal&& signal) {
    std::lock_guard lock(m_mutex);
    auto& cell = m_registry[key];
    cell.rat = rat;
    update_cell_signal(cell, signal);
    if (cell.radio.radio_data.index() == 0) init_radio_params(cell, key, rat);
  }

  std::vector<CellIdentity> get_snapshot() const {
    std::lock_guard lock(m_mutex);
    std::vector<CellIdentity> snapshot;
    snapshot.reserve(m_registry.size());
    for (const auto& [_, cell] : m_registry) snapshot.push_back(cell);
    return snapshot;
  }

  size_t cell_count() const {
    std::lock_guard lock(m_mutex);
    return m_registry.size();
  }

  /// Get the LocalCellKey of the current serving cell for a given RAT.
  /// Returns {0,0} if no serving cell is known.
  [[nodiscard]] LocalCellKey serving_key(RatType rat) const {
    std::lock_guard lock(m_mutex);
    for (const auto& [key, cell] : m_registry) {
      if (cell.rat == rat && cell.is_serving) return key;
    }
    return {};
  }

private:
  mutable std::mutex m_mutex;
  std::map<LocalCellKey, CellIdentity> m_registry;

  template <RatType R>
  void emplace_and_fill(CellIdentity& cell, LocalCellKey key) {
    auto& params = cell.radio.radio_data.template emplace<RatRadio_t<R>>();
    if constexpr (requires { params.earfcn; })
      params.earfcn = key.freq;
    else if constexpr (requires { params.nrarfcn; })
      params.nrarfcn = key.freq;
    else if constexpr (requires { params.dl_uarfcn; })
      params.dl_uarfcn = key.freq;
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
    switch (rat) {
      case RatType::LTE: emplace_and_fill<RatType::LTE>(cell, key); break;
      case RatType::NR: emplace_and_fill<RatType::NR>(cell, key); break;
      case RatType::WCDMA: emplace_and_fill<RatType::WCDMA>(cell, key); break;
      case RatType::GSM: emplace_and_fill<RatType::GSM>(cell, key); break;
      default: break;
    }
  }

  template <RatType R>
  void emplace_signal(CellIdentity& cell) {
    cell.signal.signal_data.template emplace<RatSignal_t<R>>();
  }

  void init_cell_signal(CellIdentity& cell) {
    if (cell.signal.signal_data.index() != 0) return;
    switch (cell.rat) {
      case RatType::LTE: emplace_signal<RatType::LTE>(cell); break;
      case RatType::NR: emplace_signal<RatType::NR>(cell); break;
      case RatType::WCDMA: emplace_signal<RatType::WCDMA>(cell); break;
      case RatType::GSM: emplace_signal<RatType::GSM>(cell); break;
      default: break;
    }
  }

  void update_cell_signal(CellIdentity& cell, const CellSignal& new_signal) {
    init_cell_signal(cell);
    std::visit(
        [&](const auto& incoming, auto& target) {
          using S = std::decay_t<decltype(incoming)>;
          using D = std::decay_t<decltype(target)>;
          if constexpr (std::is_same_v<S, D>) target = incoming;
        },
        new_signal.signal_data, cell.signal.signal_data);
  }

  void apply_radio_params(CellIdentity& cell, const Events::GenericRadioParamsEvent& generic) {
    std::visit(
        [&](const auto& radio_event) {
          using EventData = std::decay_t<decltype(radio_event.data)>;
          std::visit(
              [&](auto& target) {
                using TargetType = std::decay_t<decltype(target)>;
                if constexpr (std::is_same_v<TargetType, EventData>) {
                  merge_radio_fields(target, radio_event.data);
                }
              },
              cell.radio.radio_data);
        },
        generic);
  }

  /// Merge non-zero fields from source into target (additive update)
  template <typename T>
  void merge_radio_fields(T& target, const T& src) {
    if constexpr (requires { target.dl_bw; }) {
      if (src.dl_bw) target.dl_bw = src.dl_bw;
    }
    if constexpr (requires { target.ul_bw; }) {
      if (src.ul_bw) target.ul_bw = src.ul_bw;
    }
    if constexpr (requires { target.ul_earfcn; }) {
      if (src.ul_earfcn) target.ul_earfcn = src.ul_earfcn;
    }
    if constexpr (requires { target.freq_band_ind; }) {
      if (src.freq_band_ind) target.freq_band_ind = src.freq_band_ind;
    }
    if constexpr (requires { target.q_hyst; }) {
      if (src.q_hyst) target.q_hyst = src.q_hyst;
    }
    if constexpr (requires { target.t_resel_eutra; }) {
      if (src.t_resel_eutra) target.t_resel_eutra = src.t_resel_eutra;
    }
    if constexpr (requires { target.s_intra_search; }) {
      if (src.s_intra_search) target.s_intra_search = src.s_intra_search;
    }
    if constexpr (requires { target.s_non_intra_search; }) {
      if (src.s_non_intra_search) target.s_non_intra_search = src.s_non_intra_search;
    }
    if constexpr (requires { target.thresh_serving_low; }) {
      if (src.thresh_serving_low) target.thresh_serving_low = src.thresh_serving_low;
    }
    if constexpr (requires { target.sfn; }) {
      if (src.sfn) target.sfn = src.sfn;
    }
    if constexpr (requires { target.phich_duration; }) {
      if (src.phich_duration) target.phich_duration = src.phich_duration;
    }
    if constexpr (requires { target.phich_resource; }) {
      if (src.phich_resource) target.phich_resource = src.phich_resource;
    }
    if constexpr (requires { target.ac_barr_emergency; }) {
      target.ac_barr_emergency = src.ac_barr_emergency;
      target.ac_barr_mo_signaling = src.ac_barr_mo_signaling;
      target.ac_barr_mo_data = src.ac_barr_mo_data;
    }
    if constexpr (requires { target.q_rx_lev_min; }) {
      if (src.q_rx_lev_min) target.q_rx_lev_min = src.q_rx_lev_min;
    }
    if constexpr (requires { target.q_qual_min; }) {
      if (src.q_qual_min) target.q_qual_min = src.q_qual_min;
    }
    if constexpr (requires { target.ranac; }) {
      if (src.ranac) target.ranac = src.ranac;
    }
  }
};

}  // namespace QCom
