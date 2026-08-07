#include "qmi_observer/bridge.hpp"

#include "core/CellIdentity.h"

namespace qmi_observer {

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
  out.reserve(snap.cells.size() * 3);

  for (const auto& c : snap.cells) {
    if (!c.rf_channel || !c.phy_id) {
      continue;
    }

    const RatType rat = to_qcom_rat(c.rat);
    const LocalCellKey key{.freq = *c.rf_channel, .pci_bsic = *c.phy_id};

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

    if (c.rsrp_dbm || c.rsrq_db || c.rssi_dbm) {
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
        sig.signal_data = l;
      } else if (rat == RatType::NR) {
        NrSignalParams n{};
        if (c.rsrp_dbm) {
          n.ss_rsrp = *c.rsrp_dbm;
        }
        if (c.rsrq_db) {
          n.ss_rsrq = *c.rsrq_db;
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

  return out;
}

}  // namespace qmi_observer
