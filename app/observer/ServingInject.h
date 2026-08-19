#pragma once

#include "observer/AtParse.h"
#include "observer/SurveyCtx.h"

#include <observer/model/CellIdentity.h>
#include <observer/model/Events.h>
#include <observer/model/Utils.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace Observer {

enum class ServingStamp : uint8_t { None = 0, Soft = 1, Full = 2 };

[[nodiscard]] inline QCom::LocalCellKey diag_serving_key(SurveyCtx& ctx) {
  if (!ctx.engine) return {};
  for (const auto& c : ctx.engine->tracker().get_snapshot()) {
    if (c.rat == QCom::RatType::LTE && c.is_serving && c.radio.freq() != 0 &&
        c.radio.pci_bsic() != 0 && c.radio.pci_bsic() <= 503) {
      return {.freq = c.radio.freq(), .pci_bsic = c.radio.pci_bsic()};
    }
  }
  return {};
}

/// Full serving passport ONLY when we have EARFCN|PCI from CPSI or QMI.
[[nodiscard]] inline ServingStamp inject_serving_identity(SurveyCtx& ctx) {
  if (!ctx.at && !ctx.qmi) return ServingStamp::None;

  if (ctx.at) {
    if (auto cpsi_raw = ctx.at_cmd("AT+CPSI?", 1500)) {
      auto cpsi = parse_cpsi_lte(*cpsi_raw);
      if (cpsi.ok) {
        QCom::LocalCellKey key{.freq = cpsi.earfcn, .pci_bsic = cpsi.pci};
        QCom::CellPassport pass;
        pass.mcc = cpsi.mcc;
        pass.mnc = cpsi.mnc;
        pass.tac = cpsi.tac;
        pass.cell_id = cpsi.cell_id;

        QCom::Events::RadioParamsEvent<QCom::LteRadioParams> radio;
        radio.data.earfcn = cpsi.earfcn;
        radio.data.pci = cpsi.pci;
        radio.data.dl_bw = cpsi.dl_bw_mhz;
        radio.data.ul_bw = cpsi.ul_bw_mhz;

        std::vector<QCom::Events::RrcEventEnvelope> envs;
        envs.push_back(QCom::Events::RrcEventEnvelope{
            .key = key,
            .rat = QCom::RatType::LTE,
            .event_data = QCom::Events::RrcEvent{std::move(radio)},
        });
        envs.push_back(QCom::Events::RrcEventEnvelope{
            .key = key,
            .rat = QCom::RatType::LTE,
            .event_data = QCom::Events::PassportEvent{.passport = pass},
        });
        if (QCom::Utils::valid_lte_rsrp(cpsi.rsrp)) {
          QCom::CellSignal sig;
          QCom::LteSignalParams lp;
          lp.rsrp = cpsi.rsrp;
          if (cpsi.rsrq > -30.0f && cpsi.rsrq <= -1.0f) lp.rsrq = cpsi.rsrq;
          sig.signal_data = lp;
          envs.push_back(QCom::Events::RrcEventEnvelope{
              .key = key,
              .rat = QCom::RatType::LTE,
              .event_data = QCom::Events::SignalUpdateEvent{.signal = std::move(sig)},
          });
        }
        envs.push_back(QCom::Events::RrcEventEnvelope{
            .key = key,
            .rat = QCom::RatType::LTE,
            .event_data = QCom::Events::ServingChangedEvent{.is_serving = true},
        });
        ctx.engine->inject_envelopes(std::move(envs));
        ++ctx.cpsi_ok;
        ++ctx.cereg_ok;
        return ServingStamp::Full;
      }

      auto wcpsi = parse_cpsi_wcdma(*cpsi_raw);
      if (wcpsi.ok) {
        QCom::LocalCellKey key{.freq = wcpsi.uarfcn, .pci_bsic = wcpsi.psc};
        QCom::CellPassport pass;
        pass.mcc = wcpsi.mcc;
        pass.mnc = wcpsi.mnc;
        pass.tac = wcpsi.lac;
        pass.cell_id = wcpsi.cell_id;

        QCom::Events::RadioParamsEvent<QCom::WcdmaRadioParams> radio;
        radio.data.dl_uarfcn = wcpsi.uarfcn;
        radio.data.psc = wcpsi.psc;

        std::vector<QCom::Events::RrcEventEnvelope> envs;
        envs.push_back(QCom::Events::RrcEventEnvelope{
            .key = key,
            .rat = QCom::RatType::WCDMA,
            .event_data = QCom::Events::RrcEvent{std::move(radio)},
        });
        envs.push_back(QCom::Events::RrcEventEnvelope{
            .key = key,
            .rat = QCom::RatType::WCDMA,
            .event_data = QCom::Events::PassportEvent{.passport = pass},
        });
        if (wcpsi.rscp > -120.0f && wcpsi.rscp < 0.0f) {
          QCom::CellSignal sig;
          QCom::WcdmaSignalParams wp;
          wp.rscp = wcpsi.rscp;
          if (wcpsi.ecio > -30.0f && wcpsi.ecio < 0.0f) {
            wp.ecio = wcpsi.ecio;
            wp.has_ecio = true;
          }
          sig.signal_data = wp;
          envs.push_back(QCom::Events::RrcEventEnvelope{
              .key = key,
              .rat = QCom::RatType::WCDMA,
              .event_data = QCom::Events::SignalUpdateEvent{.signal = std::move(sig)},
          });
        }
        envs.push_back(QCom::Events::RrcEventEnvelope{
            .key = key,
            .rat = QCom::RatType::WCDMA,
            .event_data = QCom::Events::ServingChangedEvent{.is_serving = true},
        });
        ctx.engine->inject_envelopes(std::move(envs));
        ++ctx.cpsi_ok;
        ++ctx.cereg_ok;
        return ServingStamp::Full;
      }
    }
  }

  if (ctx.qmi) {
    if (auto snap = ctx.qmi->nas().snapshot_cells(); snap) {
      for (const auto& c : snap.value().cells) {
        if (c.rat != QCom::Qmi::Rat::Lte || !c.serving) continue;
        if (!c.rf_channel || !c.phy_id || *c.rf_channel == 0 || *c.phy_id > 503) continue;
        QCom::LocalCellKey key{.freq = *c.rf_channel, .pci_bsic = *c.phy_id};
        QCom::CellPassport pass;
        if (c.plmn) {
          pass.mcc = c.plmn->mcc;
          pass.mnc = c.plmn->mnc;
        }
        if (c.lac_or_tac && QCom::Utils::valid_lte_tac(*c.lac_or_tac)) pass.tac = *c.lac_or_tac;
        if (c.cell_id && QCom::Utils::valid_lte_eci(*c.cell_id))
          pass.cell_id = static_cast<uint32_t>(*c.cell_id);
        if (!pass.has_identity() && pass.mcc == 0) continue;

        std::vector<QCom::Events::RrcEventEnvelope> envs;
        QCom::Events::RadioParamsEvent<QCom::LteRadioParams> radio;
        radio.data.earfcn = key.freq;
        radio.data.pci = key.pci_bsic;
        envs.push_back(QCom::Events::RrcEventEnvelope{
            .key = key,
            .rat = QCom::RatType::LTE,
            .event_data = QCom::Events::RrcEvent{std::move(radio)},
        });
        envs.push_back(QCom::Events::RrcEventEnvelope{
            .key = key,
            .rat = QCom::RatType::LTE,
            .event_data = QCom::Events::PassportEvent{.passport = pass},
        });
        envs.push_back(QCom::Events::RrcEventEnvelope{
            .key = key,
            .rat = QCom::RatType::LTE,
            .event_data = QCom::Events::ServingChangedEvent{.is_serving = true},
        });
        ctx.engine->inject_envelopes(std::move(envs));
        ++ctx.cereg_ok;
        return pass.has_identity() && pass.tac != 0 ? ServingStamp::Full : ServingStamp::Soft;
      }
    }
  }

  if (ctx.at) {
    QCom::LocalCellKey key = diag_serving_key(ctx);
    if (key.freq != 0) {
      if (auto cnw_raw = ctx.at_cmd("AT+CNWINFO?", 1500)) {
        auto cnw = parse_cnwinfo_lte(*cnw_raw);
        if (cnw.ok) {
          QCom::CellPassport pass;
          pass.mcc = cnw.mcc;
          pass.mnc = cnw.mnc;
          pass.cell_id = cnw.cell_id;
          ctx.engine->inject_envelopes({QCom::Events::RrcEventEnvelope{
              .key = key,
              .rat = QCom::RatType::LTE,
              .event_data = QCom::Events::PassportEvent{.passport = pass},
          }});
          ++ctx.cnw_ok;
          ++ctx.cereg_ok;
          return ServingStamp::Soft;
        }
      }
    }
  }

  if (ctx.at) {
    auto cops = ctx.at_cmd("AT+COPS?", 1500);
    if (!cops) return ServingStamp::None;
    auto plmn = parse_cops_numeric_plmn(*cops);
    if (!plmn) return ServingStamp::None;
    QCom::LocalCellKey key = diag_serving_key(ctx);
    if (key.freq == 0) return ServingStamp::None;
    QCom::CellPassport pass;
    pass.mcc = plmn->first;
    pass.mnc = plmn->second;
    ctx.engine->inject_envelopes({QCom::Events::RrcEventEnvelope{
        .key = key,
        .rat = QCom::RatType::LTE,
        .event_data = QCom::Events::PassportEvent{.passport = pass},
    }});
    return ServingStamp::Soft;
  }
  return ServingStamp::None;
}

}  // namespace Observer
