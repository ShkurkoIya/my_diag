/// @file LteParser.cpp
/// @brief LTE ASN.1 layer — srsRAN codec for RRC OTA messages (SIB1-7, MeasReport).
#include "LteParser.h"

#include <cstdint>

#include "srsran/asn1/rrc/meascfg.h"
#include "srsran/asn1/rrc/rr_common.h"

namespace QCom::Lte {

// ============================================================================
// MIB / Cell Info (0xB175)
// ============================================================================

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_mib_metrics(
    std::string_view payload) {
  if (payload.size() < QCOM_MIB_MIN_SIZE) return std::unexpected(ParserError::PacketTooShort);

  std::string_view mib_asn1 = payload.substr(QCOM_RRC_ASN1_DATA_OFFSET);
  if (mib_asn1.empty()) return std::unexpected(ParserError::NoAsn1Payload);

  asn1::cbit_ref bref(reinterpret_cast<const uint8_t*>(mib_asn1.data()), mib_asn1.size());

  asn1::rrc::mib_s mib_msg;
  if (mib_msg.unpack(bref) != asn1::SRSASN_SUCCESS) {
    return std::unexpected(ParserError::SrsranUnpackFailed);
  }

  uint8_t dl_mhz = rb_to_mhz(mib_msg.dl_bw);
  if (dl_mhz == 0) return std::vector<Events::RrcEvent>{};

  Events::RadioParamsEvent<LteRadioParams> ev;
  ev.data.dl_bw = dl_mhz;
  ev.data.sfn = static_cast<uint16_t>(mib_msg.sys_frame_num.to_number());
  ev.data.phich_duration = static_cast<uint8_t>(mib_msg.phich_cfg.phich_dur.value);
  ev.data.phich_resource = static_cast<uint8_t>(mib_msg.phich_cfg.phich_res.value);

  std::vector<Events::RrcEvent> events;
  events.push_back(Events::RrcEvent{std::move(ev)});
  return events;
}

// ============================================================================
// BCCH-DL-SCH dispatch
// ============================================================================

std::vector<Events::RrcEvent> LteParser::on_message_unpacked(BcchMsg& msg) {
  const auto msg_type = msg.msg.c1().type().value;
  if (msg_type == BcchMsgTypes::sib_type1) return extract_sib1(msg.msg.c1().sib_type1());
  if (msg_type == BcchMsgTypes::sys_info) return extract_sys_info(msg.msg.c1().sys_info());
  return {};
}

std::vector<Events::RrcEvent> LteParser::on_message_unpacked(DlCcchMsg&) { return {}; }

std::vector<Events::RrcEvent> LteParser::on_message_unpacked(DlDcchMsg& msg) {
  const auto msg_type = msg.msg.c1().type().value;
  if (msg_type != DlDcchMsgTypes::rrc_conn_recfg) return {};
  auto& r8 = msg.msg.c1().rrc_conn_recfg().crit_exts.c1().rrc_conn_recfg_r8();
  (void)r8;
  return {};
}

std::vector<Events::RrcEvent> LteParser::on_message_unpacked(UlCcchMsg&) { return {}; }

std::vector<Events::RrcEvent> LteParser::on_message_unpacked(UlDcchMsg& msg) {
  const auto msg_type = msg.msg.c1().type().value;
  if (msg_type != UlDcchMsgTypes::meas_report) return {};

  auto& meas = msg.msg.c1().meas_report().crit_exts.c1().meas_report_r8().meas_results;

  std::vector<Events::RrcEvent> events;

  LteSignalParams serving_sig;
  serving_sig.rsrp = static_cast<float>(meas.meas_result_pcell.rsrp_result) - 140.0f;
  serving_sig.rsrq = static_cast<float>(meas.meas_result_pcell.rsrq_result) * 0.5f - 19.5f;

  CellSignal sig;
  sig.signal_data = serving_sig;
  events.push_back(Events::SignalUpdateEvent{.signal = std::move(sig)});

  if (meas.meas_result_neigh_cells_present) {
    auto& neigh = meas.meas_result_neigh_cells;
    using NeighTypes = asn1::rrc::meas_results_s::meas_result_neigh_cells_c_::types;

    if (neigh.type().value == NeighTypes::meas_result_list_eutra) {
      auto& eutra_list = neigh.meas_result_list_eutra();
      Events::NeighborMeasEvent nev;

      for (size_t i = 0; i < eutra_list.size(); ++i) {
        auto& entry = eutra_list[i];
        NeighborMeasResult nr;
        nr.pci = entry.pci;
        if (entry.meas_result.rsrp_result_present) nr.rsrp = entry.meas_result.rsrp_result;
        if (entry.meas_result.rsrq_result_present) nr.rsrq = entry.meas_result.rsrq_result;
        nev.neighbors.push_back(nr);
      }

      if (!nev.neighbors.empty()) events.push_back(std::move(nev));
    }
  }

  return events;
}

// ============================================================================
// SIB1: Cell passport
// ============================================================================

std::vector<Events::RrcEvent> LteParser::extract_sib1(const asn1::rrc::sib_type1_s& sib1) {
  std::vector<Events::RrcEvent> events;
  CellPassport passport;

  const auto& info = sib1.cell_access_related_info;
  passport.tac = static_cast<uint32_t>(info.tac.to_number());
  passport.cell_id = static_cast<uint32_t>(info.cell_id.to_number());

  if (info.plmn_id_list.size() > 0) {
    const auto& plmn = info.plmn_id_list[0].plmn_id;
    if (plmn.mcc_present) passport.mcc = Utils::Converter::digits_to_number(plmn.mcc);
    passport.mnc = Utils::Converter::digits_to_number(plmn.mnc);
  }

  using BarredOpts = asn1::rrc::sib_type1_s::cell_access_related_info_s_::cell_barred_opts;
  passport.cell_barred = (info.cell_barred.value == BarredOpts::barred);

  using ReselOpts = asn1::rrc::sib_type1_s::cell_access_related_info_s_::intra_freq_resel_opts;
  passport.intra_freq_reselection_allowed = (info.intra_freq_resel.value == ReselOpts::allowed);

  passport.q_rx_lev_min = static_cast<int8_t>(sib1.cell_sel_info.q_rx_lev_min * 2);
  if (sib1.cell_sel_info.q_rx_lev_min_offset_present) {
    passport.q_rx_lev_min_offset = static_cast<uint8_t>(sib1.cell_sel_info.q_rx_lev_min_offset * 2);
  }

  passport.csg_ind = info.csg_ind;
  if (info.csg_id_present) passport.csg_id = static_cast<uint32_t>(info.csg_id.to_number());
  passport.freq_band_ind = sib1.freq_band_ind;

  events.push_back(Events::PassportEvent{.passport = std::move(passport)});

  Events::RadioParamsEvent<LteRadioParams> rev;
  rev.data.freq_band_ind = sib1.freq_band_ind;
  events.push_back(Events::RrcEvent{std::move(rev)});

  return events;
}

// ============================================================================
// SystemInformation: SIB2 through SIB7
// ============================================================================

std::vector<Events::RrcEvent> LteParser::extract_sys_info(const asn1::rrc::sys_info_s& sys_info) {
  std::vector<Events::RrcEvent> events;
  auto& sib_list = sys_info.crit_exts.sys_info_r8().sib_type_and_info;

  for (size_t i = 0; i < sib_list.size(); ++i) {
    auto& item = sib_list[i];

    if (item.type().value == SibItemTypes::sib2) {
      auto& sib2 = item.sib2();
      Events::RadioParamsEvent<LteRadioParams> ev;
      if (sib2.freq_info.ul_bw_present) ev.data.ul_bw = sib2.freq_info.ul_bw.to_number();
      if (sib2.freq_info.ul_carrier_freq_present)
        ev.data.ul_earfcn = sib2.freq_info.ul_carrier_freq;
      if (sib2.ac_barr_info_present) {
        ev.data.ac_barr_emergency = sib2.ac_barr_info.ac_barr_for_emergency;
        ev.data.ac_barr_mo_signaling = sib2.ac_barr_info.ac_barr_for_mo_sig_present;
        ev.data.ac_barr_mo_data = sib2.ac_barr_info.ac_barr_for_mo_data_present;
      }
      events.push_back(Events::RrcEvent{std::move(ev)});
    }

    else if (item.type().value == SibItemTypes::sib3) {
      auto& sib3 = item.sib3();
      Events::RadioParamsEvent<LteRadioParams> ev;
      ev.data.q_hyst = sib3.cell_resel_info_common.q_hyst.to_number();
      ev.data.t_resel_eutra = sib3.intra_freq_cell_resel_info.t_resel_eutra;
      if (sib3.intra_freq_cell_resel_info.s_intra_search_present)
        ev.data.s_intra_search =
            static_cast<int8_t>(sib3.intra_freq_cell_resel_info.s_intra_search);
      ev.data.thresh_serving_low = sib3.cell_resel_serving_freq_info.thresh_serving_low;
      if (sib3.cell_resel_serving_freq_info.s_non_intra_search_present)
        ev.data.s_non_intra_search =
            static_cast<int8_t>(sib3.cell_resel_serving_freq_info.s_non_intra_search);
      events.push_back(Events::RrcEvent{std::move(ev)});
    }

    else if (item.type().value == SibItemTypes::sib4) {
      auto& sib4 = item.sib4();
      Events::IntraNeighborsEvent ev;
      if (sib4.intra_freq_neigh_cell_list_present) {
        for (size_t j = 0; j < sib4.intra_freq_neigh_cell_list.size(); ++j) {
          auto& nc = sib4.intra_freq_neigh_cell_list[j];
          ev.neighbors.push_back(IntraFreqNeighbor{
              .pci = nc.pci,
              .q_offset = static_cast<int8_t>(nc.q_offset_cell.to_number()),
          });
        }
      }
      events.push_back(std::move(ev));
    }

    else if (item.type().value == SibItemTypes::sib5) {
      auto& sib5 = item.sib5();
      Events::InterFreqCarriersEvent ev;
      for (size_t j = 0; j < sib5.inter_freq_carrier_freq_list.size(); ++j) {
        auto& cf = sib5.inter_freq_carrier_freq_list[j];
        ev.carriers.push_back(InterFreqCarrier{
            .earfcn = cf.dl_carrier_freq,
            .q_rx_lev_min = cf.q_rx_lev_min,
            .thresh_x_high = cf.thresh_x_high,
            .thresh_x_low = cf.thresh_x_low,
            .cell_resel_prio = cf.cell_resel_prio_present ? cf.cell_resel_prio : uint8_t{0},
            .allowed_meas_bw = static_cast<uint8_t>(cf.allowed_meas_bw.to_number()),
        });
      }
      events.push_back(std::move(ev));
    }

    else if (item.type().value == SibItemTypes::sib6) {
      auto& sib6 = item.sib6();
      Events::UtraNeighborsEvent ev;
      if (sib6.carrier_freq_list_utra_fdd_present) {
        for (size_t j = 0; j < sib6.carrier_freq_list_utra_fdd.size(); ++j) {
          auto& cf = sib6.carrier_freq_list_utra_fdd[j];
          ev.neighbors.push_back(UtraNeighborFreq{
              .uarfcn = cf.carrier_freq,
              .q_rx_lev_min = cf.q_rx_lev_min,
              .p_max_utra = cf.p_max_utra,
              .q_qual_min = cf.q_qual_min,
              .thresh_x_high = cf.thresh_x_high,
              .thresh_x_low = cf.thresh_x_low,
          });
        }
      }
      events.push_back(std::move(ev));
    }

    else if (item.type().value == SibItemTypes::sib7) {
      auto& sib7 = item.sib7();
      Events::GeranNeighborsEvent ev;
      if (sib7.carrier_freqs_info_list_present) {
        for (size_t j = 0; j < sib7.carrier_freqs_info_list.size(); ++j) {
          auto& cf = sib7.carrier_freqs_info_list[j];
          ev.neighbors.push_back(GeranNeighborFreq{
              .arfcn_start = cf.carrier_freqs.start_arfcn,
              .ncc_permitted = static_cast<uint8_t>(cf.common_info.ncc_permitted.to_number()),
              .q_rx_lev_min = static_cast<uint8_t>(cf.common_info.q_rx_lev_min),
              .thresh_x_high = cf.common_info.thresh_x_high,
              .thresh_x_low = cf.common_info.thresh_x_low,
          });
        }
      }
      events.push_back(std::move(ev));
    }
  }

  return events;
}

}  // namespace QCom::Lte
