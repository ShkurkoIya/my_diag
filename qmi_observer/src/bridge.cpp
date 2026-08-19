#include <qcom/qmi/bridge.hpp>

#include <observer/model/CellIdentity.h>

#include <algorithm>
#include <map>

namespace QCom::Qmi {

QCom::RatType to_qcom_rat(Rat r) noexcept {
  switch (r) {
    case Rat::Gsm: return QCom::RatType::GSM;
    case Rat::Wcdma: return QCom::RatType::WCDMA;
    case Rat::Lte: return QCom::RatType::LTE;
    case Rat::Nr: return QCom::RatType::NR;
    default: return QCom::RatType::UNKNOWN;
  }
}

std::vector<QCom::Events::RrcEventEnvelope> to_rrc_envelopes(const CellSnapshot& snap,
                                                             uint64_t timestamp) {
  using namespace QCom;
  using namespace QCom::Events;

  std::vector<RrcEventEnvelope> out;
  out.reserve(snap.cells.size() * 4);

  for (const auto& c : snap.cells) {
    // Need a physical key. PCI/PSC may be 0 on some stubs — still accept with RF+CID.
    if (!c.rf_channel) continue;
    if (!c.phy_id && !c.cell_id) continue;

    const RatType rat = to_qcom_rat(c.rat);
    const uint16_t phy = c.phy_id.value_or(0);
    const LocalCellKey key{.freq = *c.rf_channel, .pci_bsic = phy};

    // Always mint RADIO so EARFCN|PCI rows exist even without passport.
    if (rat == RatType::LTE && phy <= 503) {
      LteRadioParams radio;
      radio.earfcn = *c.rf_channel;
      radio.pci = phy;
      if (c.s_intra_search) radio.s_intra_search = static_cast<int8_t>(*c.s_intra_search);
      if (c.s_non_intra_search)
        radio.s_non_intra_search = static_cast<int8_t>(*c.s_non_intra_search);
      if (c.thresh_serving_low) radio.thresh_serving_low = *c.thresh_serving_low;
      if (c.cell_resel_prio) radio.cell_resel_prio = *c.cell_resel_prio;
      if (c.thresh_x_high) radio.thresh_x_high = *c.thresh_x_high;
      if (c.thresh_x_low) radio.thresh_x_low = *c.thresh_x_low;
      if (c.timing_advance) radio.timing_advance = *c.timing_advance;
      if (c.idle) radio.nas_idle = *c.idle ? 1 : 0;
      out.push_back(RrcEventEnvelope{
          .key = key,
          .rat = rat,
          .timestamp = timestamp,
          .event_data = GenericRadioParamsEvent{RadioParamsEvent<LteRadioParams>{.data = radio}},
      });
    } else if (rat == RatType::WCDMA) {
      WcdmaRadioParams radio;
      radio.dl_uarfcn = *c.rf_channel;
      radio.psc = phy;
      out.push_back(RrcEventEnvelope{
          .key = key,
          .rat = rat,
          .timestamp = timestamp,
          .event_data = GenericRadioParamsEvent{RadioParamsEvent<WcdmaRadioParams>{.data = radio}},
      });
    } else if (rat == RatType::GSM) {
      GsmRadioParams radio;
      radio.arfcn = *c.rf_channel;
      radio.bsic = static_cast<uint8_t>(phy & 0x3F);
      radio.ncc = static_cast<uint8_t>((phy >> 3) & 0x7);
      radio.bcc = static_cast<uint8_t>(phy & 0x7);
      if (c.timing_advance) radio.timing_advance = *c.timing_advance;
      out.push_back(RrcEventEnvelope{
          .key = key,
          .rat = rat,
          .timestamp = timestamp,
          .event_data = GenericRadioParamsEvent{RadioParamsEvent<GsmRadioParams>{.data = radio}},
      });
    } else if (rat == RatType::NR) {
      NrRadioParams radio;
      radio.nrarfcn = *c.rf_channel;
      radio.pci = phy;
      out.push_back(RrcEventEnvelope{
          .key = key,
          .rat = rat,
          .timestamp = timestamp,
          .event_data = GenericRadioParamsEvent{RadioParamsEvent<NrRadioParams>{.data = radio}},
      });
    }

    if (c.plmn || c.lac_or_tac || c.cell_id) {
      CellPassport pass;
      if (c.plmn) {
        pass.mcc = c.plmn->mcc;
        pass.mnc = c.plmn->mnc;
      }
      if (c.lac_or_tac) {
        pass.tac = *c.lac_or_tac;
      }
      if (c.cell_id) {
        pass.cell_id = *c.cell_id;
      }
      out.push_back(RrcEventEnvelope{
          .key = key,
          .rat = rat,
          .timestamp = timestamp,
          .event_data = PassportEvent{.passport = pass},
      });
    }

    if (c.rsrp_dbm || c.rsrq_db || c.rssi_dbm || c.snr_db) {
      CellSignal sig;
      if (rat == RatType::WCDMA) {
        WcdmaSignalParams w{};
        if (c.rsrp_dbm) {
          w.rscp = *c.rsrp_dbm;
        }
        if (c.rsrq_db) {
          w.ecio = *c.rsrq_db;
          w.has_ecio = true;
        }
        sig.signal_data = w;
      } else if (rat == RatType::LTE) {
        LteSignalParams l{};
        if (c.rsrp_dbm) {
          l.rsrp = *c.rsrp_dbm;
        }
        if (c.rsrq_db) {
          l.rsrq = *c.rsrq_db;
        }
        if (c.rssi_dbm) {
          l.rssi = *c.rssi_dbm;
          l.has_rssi = true;
        }
        if (c.snr_db) {
          l.sinr = *c.snr_db;
          l.has_sinr = true;
        }
        sig.signal_data = l;
      } else if (rat == RatType::NR) {
        NrSignalParams n{};
        if (c.rsrp_dbm) {
          n.ss_rsrp = *c.rsrp_dbm;
        }
        if (c.rsrq_db) {
          n.ss_rsrq = *c.rsrq_db;
        }
        if (c.snr_db) {
          n.ss_sinr = *c.snr_db;
        }
        sig.signal_data = n;
      } else if (rat == RatType::GSM) {
        GsmSignalParams g{};
        if (c.rssi_dbm) {
          g.rxlev = static_cast<int8_t>(*c.rssi_dbm);
        } else if (c.rsrp_dbm) {
          g.rxlev = static_cast<int8_t>(*c.rsrp_dbm);
        }
        sig.signal_data = g;
      }
      out.push_back(RrcEventEnvelope{
          .key = key,
          .rat = rat,
          .timestamp = timestamp,
          .event_data = SignalUpdateEvent{.signal = sig},
      });
    }

    if (c.serving) {
      out.push_back(RrcEventEnvelope{
          .key = key,
          .rat = rat,
          .timestamp = timestamp,
          .event_data = ServingChangedEvent{.is_serving = true},
      });
    }
  }

  // Attach LTE intra/inter PCI onto the serving key so hop's serving-neigh
  // whitelist sees QMI neighbors (CMGRMI is often ERROR while EMM deregistered).
  const CellObservation* lte_srv = nullptr;
  for (const auto& c : snap.cells) {
    if (c.rat == Rat::Lte && c.serving && c.rf_channel && c.phy_id) {
      lte_srv = &c;
      break;
    }
  }
  if (lte_srv) {
    const LocalCellKey skey{.freq = *lte_srv->rf_channel, .pci_bsic = *lte_srv->phy_id};
    std::vector<NeighborMeasResult> meas;
    std::map<uint32_t, InterFreqCarrier> inter;
    for (const auto& c : snap.cells) {
      if (c.rat != Rat::Lte || !c.rf_channel || !c.phy_id) continue;
      if (*c.phy_id == 0 || *c.phy_id > 503) continue;
      if (*c.rf_channel == *lte_srv->rf_channel && *c.phy_id == *lte_srv->phy_id) continue;
      if (*c.rf_channel == *lte_srv->rf_channel) {
        NeighborMeasResult n;
        n.pci = *c.phy_id;
        if (c.rsrp_dbm) {
          n.rsrp_dbm = *c.rsrp_dbm;
          n.has_rsrp = true;
        }
        if (c.rsrq_db) {
          n.rsrq_db = *c.rsrq_db;
          n.has_rsrq = true;
        }
        meas.push_back(n);
      } else {
        auto& car = inter[*c.rf_channel];
        car.earfcn = *c.rf_channel;
        if (std::find(car.neigh_pcis.begin(), car.neigh_pcis.end(), *c.phy_id) ==
            car.neigh_pcis.end())
          car.neigh_pcis.push_back(*c.phy_id);
      }
    }
    if (!meas.empty()) {
      out.push_back(RrcEventEnvelope{
          .key = skey,
          .rat = RatType::LTE,
          .timestamp = timestamp,
          .event_data = NeighborMeasEvent{.neighbors = std::move(meas)},
      });
    }
    if (!inter.empty()) {
      std::vector<InterFreqCarrier> carriers;
      carriers.reserve(inter.size());
      for (auto& [_, car] : inter) carriers.push_back(std::move(car));
      out.push_back(RrcEventEnvelope{
          .key = skey,
          .rat = RatType::LTE,
          .timestamp = timestamp,
          .event_data = InterFreqCarriersEvent{.carriers = std::move(carriers)},
      });
    }
  }

  return out;
}

}  // namespace QCom::Qmi
