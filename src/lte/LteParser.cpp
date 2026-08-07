/// @file LteParser.cpp
/// @brief LTE ASN.1 layer — srsRAN codec for RRC OTA messages (SIB1-7, MeasReport).
#include "lte/LteParser.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>

#include "lte/LteRrcOta.h"
#include "srsran/asn1/rrc/meascfg.h"
#include "srsran/asn1/rrc/rr_common.h"

namespace QCom::Lte {

// ============================================================================
// 0xB0C0 — RRC OTA
// ============================================================================

namespace {

[[nodiscard]] Events::RadioParamsEvent<LteRadioParams> radio_from_ota(const LteRrcOtaInfo& ota) {
  Events::RadioParamsEvent<LteRadioParams> ev;
  ev.data.earfcn = ota.earfcn;
  ev.data.pci = ota.pci;
  return ev;
}

[[nodiscard]] bool ota_has_physical_key(const LteRrcOtaInfo& ota) noexcept {
  return ota.earfcn != 0 && ota.pci <= 503;
}

}  // namespace

namespace {
std::atomic<uint64_t> g_lte_rrc_ota_asn1_empty{0};
}  // namespace

uint64_t lte_rrc_ota_asn1_empty_count() noexcept {
  return g_lte_rrc_ota_asn1_empty.load(std::memory_order_relaxed);
}

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_rrc_ota(
    std::span<const uint8_t> payload) {
  if (payload.empty()) return std::unexpected(ParserError::PacketTooShort);

  // Full Qualcomm wrapper (versioned header + PDU)
  if (auto ota = decode_lte_rrc_ota(payload)) {
    // v30+ segmented RRC (scat): buffer 1–6, join on 7, pass-through on 0.
    // Cache is per EARFCN|PCI — mixing cells produced corrupt ASN joins.
    std::vector<uint8_t> joined_asn1;
    if (ota->version >= 30 && ota->segment_id >= 1 && ota->segment_id <= 7) {
      const RrcSegKey sk{.earfcn = ota->earfcn, .pci = ota->pci};
      auto& slots = m_rrc_segments[sk];
      if (ota->segment_id <= 6) {
        slots[ota->segment_id].assign(ota->asn1.begin(), ota->asn1.end());
        if (!ota_has_physical_key(*ota)) return std::vector<Events::RrcEvent>{};
        return std::vector<Events::RrcEvent>{Events::RrcEvent{radio_from_ota(*ota)}};
      }
      // Require at least segment 1 before trusting a join (partial → ASN fail).
      if (slots[1].empty()) {
        for (auto& s : slots) s.clear();
        if (!ota_has_physical_key(*ota)) return std::vector<Events::RrcEvent>{};
        return std::vector<Events::RrcEvent>{Events::RrcEvent{radio_from_ota(*ota)}};
      }
      for (uint8_t i = 1; i <= 6; ++i) {
        joined_asn1.insert(joined_asn1.end(), slots[i].begin(), slots[i].end());
        slots[i].clear();
      }
      m_rrc_segments.erase(sk);
      joined_asn1.insert(joined_asn1.end(), ota->asn1.begin(), ota->asn1.end());
      ota->asn1 = joined_asn1;
      ota->pdu_len = static_cast<uint16_t>(std::min<size_t>(joined_asn1.size(), 0xFFFF));
    }

    ChannelType ch = pdu_num_to_channel(ota->version, ota->pdu_num);

    // BCCH-BCH (MIB): channel not in our RRC OTA map, but wrapper still has
    // PCI/EARFCN — enough to open a cell row; try ASN.1 MIB for BW/SFN.
    if (ch == ChannelType::UNKNOWN && is_mib_pdu(ota->version, ota->pdu_num)) {
      std::vector<Events::RrcEvent> events;
      auto ev = radio_from_ota(*ota);
      if (!ota->asn1.empty()) {
        asn1::cbit_ref bref(ota->asn1.data(), ota->asn1.size());
        asn1::rrc::mib_s mib_msg;
        if (mib_msg.unpack(bref) == asn1::SRSASN_SUCCESS) {
          uint8_t dl_mhz = rb_to_mhz(mib_msg.dl_bw);
          if (dl_mhz) ev.data.dl_bw = dl_mhz;
          ev.data.sfn = static_cast<uint16_t>(mib_msg.sys_frame_num.to_number());
          ev.data.phich_duration = static_cast<uint8_t>(mib_msg.phich_cfg.phich_dur.value);
          ev.data.phich_resource = static_cast<uint8_t>(mib_msg.phich_cfg.phich_res.value);
        }
      }
      events.push_back(Events::RrcEvent{std::move(ev)});
      return events;
    }

    // PCCH/MCCH/etc.: still open EARFCN|PCI so ML1/PLMN can merge onto the row.
    if (ch == ChannelType::UNKNOWN) {
      if (!ota_has_physical_key(*ota)) return std::vector<Events::RrcEvent>{};
      return std::vector<Events::RrcEvent>{Events::RrcEvent{radio_from_ota(*ota)}};
    }

    auto synth = synthesize_ota_header(ota->earfcn, ota->pci, ch, ota->asn1);
    auto decoded = parse_rrc_ota_base(synth);

    auto try_variants = [&](std::span<const uint8_t> asn1) {
      if (asn1.size() <= 4) return;
      // Leading pad bytes (SDX55) and optional declared-length slice.
      for (size_t skip = 0; skip <= 8; ++skip) {
        if (skip >= asn1.size()) break;
        auto slice = asn1.subspan(skip);
        auto retry = parse_rrc_ota_base(synthesize_ota_header(ota->earfcn, ota->pci, ch, slice));
        if (retry && !retry->empty()) {
          decoded = std::move(retry);
          return;
        }
      }
    };

    // Some SDX55 BCCH payloads need leading pad / benefit from declared pdu_len trim.
    if (ch == ChannelType::BCCH_DL_SCH && ota->asn1.size() > 4 && (!decoded || decoded->empty())) {
      // Prefer declared length first (fewer trailing pad ghosts), then full+skip.
      if (ota->pdu_len >= 4 && ota->pdu_len < ota->asn1.size()) {
        try_variants(ota->asn1.subspan(0, ota->pdu_len));
      }
      if (!decoded || decoded->empty()) try_variants(ota->asn1);
    }

    if (!decoded) return decoded;

    // Unpack fail / unhandled SI → still emit radio so the physical key is tracked.
    // Only count BCCH empties — UL/DL-DCCH often unpack with no survey events.
    if (decoded->empty()) {
      if (ch == ChannelType::BCCH_DL_SCH) ++g_lte_rrc_ota_asn1_empty;
      if (ota_has_physical_key(*ota)) {
        decoded->push_back(Events::RrcEvent{radio_from_ota(*ota)});
      }
      return decoded;
    }

    // Ensure EARFCN/PCI from the Qualcomm header land on the sticky key path.
    if (ota_has_physical_key(*ota)) {
      bool has_radio = false;
      for (const auto& ev : *decoded) {
        if (auto* gen = std::get_if<Events::GenericRadioParamsEvent>(&ev)) {
          if (auto* lte = std::get_if<Events::RadioParamsEvent<LteRadioParams>>(gen)) {
            if (lte->data.earfcn != 0) {
              has_radio = true;
              break;
            }
          }
        }
      }
      if (!has_radio) decoded->insert(decoded->begin(), Events::RrcEvent{radio_from_ota(*ota)});
    }
    return decoded;
  }

  // Legacy / journal-synthesized 7-byte header: channel type at offset 6.
  // Do not blind-unpack ASN.1 without a channel — PER can "succeed" on garbage
  // and leave CHOICEs in a state that crashes on access.
  if (payload.size() < QCOM_RRC_METADATA_SIZE) {
    return std::unexpected(ParserError::PacketTooShort);
  }

  uint8_t ch = payload[QCOM_RRC_CHANNEL_TYPE_OFFSET];
  if (ch >= 1 && ch <= 5) return parse_rrc_ota_base(payload);
  return std::unexpected(ParserError::UnknownChannelType);
}

// ============================================================================
// 0xB175 — formerly mislabeled "MIB"
// ============================================================================
// Live SIM8300: ver=48, ~384B histogram / LL1 metrics — no EARFCN|PCI|CID.
// Blind ASN.1 MIB unpack at +7 false-succeeded on noise and stamped fake BW
// onto the serving row. Fail-closed until a verified layout exists.
// Tiny journal-synthetic frames (legacy 7B hdr + MIB ASN.1) still accepted.

std::expected<std::vector<Events::RrcEvent>, ParserError> LteParser::parse_mib_metrics(
    std::span<const uint8_t> payload) {
  if (payload.size() < QCOM_MIB_MIN_SIZE) return std::unexpected(ParserError::PacketTooShort);

  // Modem ML1/LL1 dumps (v48 etc.) — not ASN.1.
  if (payload.size() > 64 || payload[0] >= 3) return std::vector<Events::RrcEvent>{};

  auto mib_asn1 = payload.subspan(QCOM_RRC_ASN1_DATA_OFFSET);
  if (mib_asn1.empty()) return std::unexpected(ParserError::NoAsn1Payload);

  asn1::cbit_ref bref(mib_asn1.data(), mib_asn1.size());

  asn1::rrc::mib_s mib_msg;
  if (mib_msg.unpack(bref) != asn1::SRSASN_SUCCESS) {
    return std::vector<Events::RrcEvent>{};
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
  using MsgTypes = asn1::rrc::bcch_dl_sch_msg_type_c::types;
  if (msg.msg.type().value != MsgTypes::c1) return {};
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
  using MsgTypes = asn1::rrc::ul_dcch_msg_type_c::types;
  if (msg.msg.type().value != MsgTypes::c1) return {};
  const auto msg_type = msg.msg.c1().type().value;
  if (msg_type != UlDcchMsgTypes::meas_report) return {};

  auto& meas_msg = msg.msg.c1().meas_report();
  using CritTypes = asn1::rrc::meas_report_s::crit_exts_c_::types;
  if (meas_msg.crit_exts.type().value != CritTypes::c1) return {};
  using C1Types = asn1::rrc::meas_report_s::crit_exts_c_::c1_c_::types;
  if (meas_msg.crit_exts.c1().type().value != C1Types::meas_report_r8) return {};

  auto& meas = meas_msg.crit_exts.c1().meas_report_r8().meas_results;

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
        if (entry.meas_result.rsrp_result_present) {
          nr.rsrp_dbm = static_cast<float>(entry.meas_result.rsrp_result) - 140.0f;
          nr.has_rsrp = true;
        }
        if (entry.meas_result.rsrq_result_present) {
          nr.rsrq_db = static_cast<float>(entry.meas_result.rsrq_result) * 0.5f - 19.5f;
          nr.has_rsrq = true;
        }
        // reportCGI → full passport on this PCI (same EARFCN as MeasReport sticky key).
        if (entry.cgi_info_present) {
          CellPassport cgi;
          cgi.cell_id = static_cast<uint32_t>(entry.cgi_info.cell_global_id.cell_id.to_number());
          cgi.tac = static_cast<uint32_t>(entry.cgi_info.tac.to_number());
          const auto& plmn = entry.cgi_info.cell_global_id.plmn_id;
          if (plmn.mcc_present) cgi.mcc = Utils::Converter::digits_to_number(plmn.mcc);
          cgi.mnc = Utils::Converter::digits_to_number(plmn.mnc);
          if (cgi.has_identity() && cgi.mcc >= 100 && cgi.mcc <= 999) {
            nr.has_cgi = true;
            nr.cgi = cgi;
          }
        }
        // Keep CGI-only rows even without RSRP (NW asked for CGI of a specific PCI).
        if (nr.has_rsrp || nr.has_cgi) nev.neighbors.push_back(nr);
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

  // Emit one passport per PLMN in SIB1 (primary first). Tracker keeps last non-zero
  // MCC/MNC on the same row; multi-PLMN still surfaces via successive merges / B0C4.
  if (info.plmn_id_list.size() == 0) {
    events.push_back(Events::PassportEvent{.passport = passport});
  } else {
    for (size_t i = 0; i < info.plmn_id_list.size(); ++i) {
      CellPassport p = passport;
      const auto& plmn = info.plmn_id_list[i].plmn_id;
      if (plmn.mcc_present) p.mcc = Utils::Converter::digits_to_number(plmn.mcc);
      p.mnc = Utils::Converter::digits_to_number(plmn.mnc);
      events.push_back(Events::PassportEvent{.passport = std::move(p)});
    }
  }

  Events::RadioParamsEvent<LteRadioParams> rev;
  rev.data.freq_band_ind = sib1.freq_band_ind;
  if (sib1.p_max_present) {
    rev.data.p_max_present = true;
    rev.data.p_max = sib1.p_max;
  }
  events.push_back(Events::RrcEvent{std::move(rev)});

  return events;
}

// ============================================================================
// SystemInformation: SIB2 through SIB7
// ============================================================================

std::vector<Events::RrcEvent> LteParser::extract_sys_info(const asn1::rrc::sys_info_s& sys_info) {
  std::vector<Events::RrcEvent> events;
  using CritTypes = asn1::rrc::sys_info_s::crit_exts_c_::types;
  if (sys_info.crit_exts.type().value != CritTypes::sys_info_r8) return {};

  auto& sib_list = sys_info.crit_exts.sys_info_r8().sib_type_and_info;

  for (size_t i = 0; i < sib_list.size(); ++i) {
    auto& item = sib_list[i];
    const auto item_type = item.type().value;
    if (item_type == SibItemTypes::nulltype) continue;

    if (item_type == SibItemTypes::sib2) {
      auto& sib2 = item.sib2();
      Events::RadioParamsEvent<LteRadioParams> ev;
      if (sib2.freq_info.ul_bw_present) {
        // ASN.1 ul_bw is RB count (6/15/25/50/75/100) — store as MHz
        switch (sib2.freq_info.ul_bw.to_number()) {
          case 6: ev.data.ul_bw = 1; break;
          case 15: ev.data.ul_bw = 3; break;
          case 25: ev.data.ul_bw = 5; break;
          case 50: ev.data.ul_bw = 10; break;
          case 75: ev.data.ul_bw = 15; break;
          case 100: ev.data.ul_bw = 20; break;
          default: break;
        }
      }
      if (sib2.freq_info.ul_carrier_freq_present)
        ev.data.ul_earfcn = sib2.freq_info.ul_carrier_freq;
      if (sib2.ac_barr_info_present) {
        ev.data.ac_barr_emergency = sib2.ac_barr_info.ac_barr_for_emergency;
        ev.data.ac_barr_mo_signaling = sib2.ac_barr_info.ac_barr_for_mo_sig_present;
        ev.data.ac_barr_mo_data = sib2.ac_barr_info.ac_barr_for_mo_data_present;
      }
      events.push_back(Events::RrcEvent{std::move(ev)});
    }

    else if (item_type == SibItemTypes::sib3) {
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

    else if (item_type == SibItemTypes::sib4) {
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

    else if (item_type == SibItemTypes::sib5) {
      auto& sib5 = item.sib5();
      Events::InterFreqCarriersEvent ev;
      for (size_t j = 0; j < sib5.inter_freq_carrier_freq_list.size(); ++j) {
        auto& cf = sib5.inter_freq_carrier_freq_list[j];
        InterFreqCarrier car{
            .earfcn = cf.dl_carrier_freq,
            .q_rx_lev_min = cf.q_rx_lev_min,
            .thresh_x_high = cf.thresh_x_high,
            .thresh_x_low = cf.thresh_x_low,
            .cell_resel_prio = cf.cell_resel_prio_present ? cf.cell_resel_prio : uint8_t{0},
            .allowed_meas_bw = static_cast<uint8_t>(cf.allowed_meas_bw.to_number()),
        };
        if (cf.inter_freq_neigh_cell_list_present) {
          for (size_t k = 0; k < cf.inter_freq_neigh_cell_list.size(); ++k) {
            uint16_t pci = cf.inter_freq_neigh_cell_list[k].pci;
            if (pci <= 503) car.neigh_pcis.push_back(pci);
          }
        }
        ev.carriers.push_back(std::move(car));
      }
      events.push_back(std::move(ev));
    }

    else if (item_type == SibItemTypes::sib6) {
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

    else if (item_type == SibItemTypes::sib7) {
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
