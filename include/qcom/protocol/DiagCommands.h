/// @file DiagCommands.h
/// @brief Qualcomm DIAG log-mask builders (scat / dia_vldos compatible).
///
/// Mask format matches fgsect/scat `create_log_config_set_mask`:
///   [u32 opcode=0x73][u32 SET_MASK=3][u32 equip][u32 last_item][bitfield…]
/// Item IDs are the low 12 bits of the log code (e.g. 0xC0 → 0xB0C0).
#pragma once

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <span>
#include <thread>
#include <vector>

#include <qcom/protocol/DiagSourceConfig.h>
#include <qcom/protocol/HdlcCodec.h>

namespace QCom {

namespace DiagOpcode {
constexpr uint8_t LOG_CONFIG = 0x73;
constexpr uint8_t EXT_MSG_CONFIG = 0x7D;
}  // namespace DiagOpcode

namespace DiagLogOp {
constexpr uint32_t DISABLE_ALL = 0;
constexpr uint32_t RETRIEVE_ID_RANGES = 1;
constexpr uint32_t SET_MASK = 3;
}  // namespace DiagLogOp

namespace DiagEquip {
constexpr uint32_t WCDMA = 0x04;
constexpr uint32_t GSM = 0x05;
constexpr uint32_t UMTS = 0x07;
constexpr uint32_t LTE = 0x0B;  // also carries NR item IDs ≥ 0x800
}  // namespace DiagEquip

/// Log item IDs within an equipment (scat / dia_vldos diagcmd.h).
namespace DiagItem {
// LTE (equip 0x0B) → full code 0xB000|id
constexpr uint32_t LTE_MAC_RACH_TRIGGER = 0x061;       // 0xB061
constexpr uint32_t LTE_MAC_RACH_RESPONSE = 0x062;      // 0xB062
constexpr uint32_t LTE_MAC_DL_TB = 0x063;              // 0xB063
constexpr uint32_t LTE_MAC_UL_TB = 0x064;              // 0xB064

constexpr uint32_t LTE_ML1_MAC_RAR_MSG1 = 0x167;       // 0xB167
constexpr uint32_t LTE_ML1_MAC_RAR_MSG2 = 0x168;       // 0xB168
constexpr uint32_t LTE_ML1_MAC_MSG3 = 0x169;           // 0xB169
constexpr uint32_t LTE_ML1_MAC_MSG4 = 0x16A;           // 0xB16A
constexpr uint32_t LTE_ML1_CONN_INTRA_MEAS = 0x179;    // 0xB179
constexpr uint32_t LTE_ML1_SERVING_MEAS = 0x17F;       // 0xB17F ★ RSRP
constexpr uint32_t LTE_ML1_NEIGHBOR_MEAS = 0x180;      // 0xB180
constexpr uint32_t LTE_ML1_INTRA_RESEL = 0x181;        // 0xB181
constexpr uint32_t LTE_ML1_NEIGHBOR_MEAS_REQ = 0x192;  // 0xB192
constexpr uint32_t LTE_ML1_SERVING_MEAS_RSP = 0x193;   // 0xB193
constexpr uint32_t LTE_ML1_SEARCH_REQ_RSP = 0x194;     // 0xB194 ★ PLMN/search
constexpr uint32_t LTE_ML1_CONN_NEIGH_MEAS = 0x195;    // 0xB195
constexpr uint32_t LTE_ML1_SERVING_INFO = 0x197;       // 0xB197 ★ PCI/EARFCN

constexpr uint32_t LTE_RRC_OTA = 0xC0;                 // 0xB0C0 ★ SIB1 ASN.1
constexpr uint32_t LTE_RRC_MIB = 0xC1;                 // 0xB0C1
constexpr uint32_t LTE_RRC_SERVING_CELL = 0xC2;        // 0xB0C2 ★ PLMN/TAC/CID
constexpr uint32_t LTE_RRC_PLMN_SEARCH_REQ = 0xC3;     // 0xB0C3 (QXDM drive-test filter)
constexpr uint32_t LTE_RRC_PLMN_SEARCH_RSP = 0xC4;     // 0xB0C4 ★ PLMN search results
constexpr uint32_t LTE_RRC_LOG_MEAS = 0xCA;            // 0xB0CA
constexpr uint32_t LTE_RRC_PAGING = 0xCB;              // 0xB0CB
constexpr uint32_t LTE_RRC_CA_COMBOS = 0xCD;           // 0xB0CD

constexpr uint32_t LTE_LL1_PSS_RESULTS = 0x113;        // 0xB113
constexpr uint32_t LTE_LL1_FRAME_TIMING = 0x114;       // 0xB114 ★ Serving UL TA
constexpr uint32_t LTE_LL1_SSS_RESULTS = 0x115;        // 0xB115
constexpr uint32_t LTE_LL1_NCELL_CER = 0x123;          // 0xB123
constexpr uint32_t LTE_CELL_INFO_MIB = 0x175;         // 0xB175
constexpr uint32_t LTE_INITIAL_ACQ = 0x176;            // 0xB176 ★ acquisition

// NAS — IDs from dia_vldos/scat (NOT 0xE8/E9)
constexpr uint32_t LTE_NAS_ESM_SEC_IN = 0xE0;          // 0xB0E0
constexpr uint32_t LTE_NAS_ESM_SEC_OUT = 0xE1;         // 0xB0E1
constexpr uint32_t LTE_NAS_ESM_PLAIN_IN = 0xE2;        // 0xB0E2
constexpr uint32_t LTE_NAS_ESM_PLAIN_OUT = 0xE3;       // 0xB0E3
constexpr uint32_t LTE_NAS_EMM_SEC_IN = 0xEA;          // 0xB0EA
constexpr uint32_t LTE_NAS_EMM_SEC_OUT = 0xEB;         // 0xB0EB
constexpr uint32_t LTE_NAS_EMM_PLAIN_IN = 0xEC;        // 0xB0EC
constexpr uint32_t LTE_NAS_EMM_PLAIN_OUT = 0xED;       // 0xB0ED
constexpr uint32_t LTE_NAS_EMM_STATE = 0xEE;           // 0xB0EE ★ reg state / GUTI meta

// NR (same equip) when last_item ≥ 0x800
constexpr uint32_t NR_RRC_OTA = 0x821;                 // 0xB821
constexpr uint32_t NR_RRC_MIB = 0x822;                 // 0xB822
constexpr uint32_t NR_RRC_SERVING = 0x823;             // 0xB823
constexpr uint32_t NR_RRC_CONFIG = 0x825;              // 0xB825
constexpr uint32_t NR_RRC_CA = 0x826;                  // 0xB826
constexpr uint32_t NR_RRC_PLMN_SEARCH_REQ = 0x827;     // 0xB827
constexpr uint32_t NR_RRC_PLMN_SEARCH_RSP = 0x828;     // 0xB828
constexpr uint32_t NR_RRC_DETECTED_CELL = 0x82B;       // 0xB82B ★ detected cells
constexpr uint32_t NR_ML1_SEARCHER_ACQ = 0x96D;        // 0xB96D
constexpr uint32_t NR_ML1_SEARCHER_MEAS_CFG = 0x96E;   // 0xB96E
constexpr uint32_t NR_ML1_MEAS_DB = 0x97F;             // 0xB97F

// WCDMA (equip 0x04)
constexpr uint32_t WCDMA_RESEL_RANK = 0x005;           // 0x4005
constexpr uint32_t WCDMA_CELL_ID_LEGACY = 0x027;       // 0x4027 ★ serving identity
constexpr uint32_t WCDMA_ACTIVE_SET = 0x111;           // 0x4111
constexpr uint32_t WCDMA_CELL_ID = 0x127;              // 0x4127 serving cell info
constexpr uint32_t WCDMA_SIB = 0x12B;                  // 0x412B
constexpr uint32_t WCDMA_SIGNALING = 0x12F;            // 0x412F
constexpr uint32_t WCDMA_PN_SEARCH = 0x179;            // 0x4179
constexpr uint32_t WCDMA_FREQ_SCAN = 0x1B0;            // 0x41B0

// UMTS NAS (equip 0x07)
constexpr uint32_t UMTS_NAS_OTA = 0x13A;               // 0x713A

// GSM (equip 0x05)
constexpr uint32_t GSM_L1_FCCH = 0x065;                // 0x5065
constexpr uint32_t GSM_L1_SCH = 0x066;                 // 0x5066
constexpr uint32_t GSM_L1_BURST_METRICS = 0x06C;       // 0x506C
constexpr uint32_t GSM_L1_SCELL_BA_LIST = 0x071;       // 0x5071
constexpr uint32_t GSM_L1_SCELL_AUX = 0x07A;           // 0x507A
constexpr uint32_t GSM_L1_NCELL_AUX = 0x07B;           // 0x507B
constexpr uint32_t GSM_RR_SIGNALING = 0x12F;           // 0x512F ★ SI-3 passport
constexpr uint32_t GSM_RR_CELL_INFORMATION = 0x134;    // 0x5134
constexpr uint32_t GSM_DSDS_RR_SIGNALING = 0xB2F;      // 0x5B2F
constexpr uint32_t GSM_DSDS_RR_CELL_INFO = 0xB34;      // 0x5B34
}  // namespace DiagItem

constexpr uint32_t kLteMaskLastItem = 0x09FF;

/// LTE log items for SET_MASK. Search omits B113/B114/B123 (stubs that flood the
/// MPSS ring). SSS B115 stays — it mints EARFCN|PCI during COPS=?. Serving adds
/// B114 TA. Items above kLteMaskLastItem are skipped in the bitfield.
[[nodiscard]] inline std::vector<uint32_t> lte_diag_item_ids(LteDiagPack pack) {
  using namespace DiagItem;
  std::vector<uint32_t> ids = {
      LTE_ML1_CONN_INTRA_MEAS,
      LTE_ML1_SERVING_MEAS,
      LTE_ML1_NEIGHBOR_MEAS,
      LTE_ML1_INTRA_RESEL,
      LTE_ML1_NEIGHBOR_MEAS_REQ,
      LTE_ML1_SERVING_MEAS_RSP,
      LTE_ML1_SEARCH_REQ_RSP,
      LTE_ML1_CONN_NEIGH_MEAS,
      LTE_ML1_SERVING_INFO,
      LTE_RRC_OTA,
      LTE_RRC_MIB,
      LTE_RRC_SERVING_CELL,
      LTE_RRC_PLMN_SEARCH_REQ,
      LTE_RRC_PLMN_SEARCH_RSP,
      LTE_RRC_LOG_MEAS,
      LTE_RRC_PAGING,
      LTE_RRC_CA_COMBOS,
      LTE_LL1_SSS_RESULTS,
      LTE_CELL_INFO_MIB,
      LTE_INITIAL_ACQ,
      LTE_NAS_ESM_SEC_IN,
      LTE_NAS_ESM_SEC_OUT,
      LTE_NAS_ESM_PLAIN_IN,
      LTE_NAS_ESM_PLAIN_OUT,
      LTE_NAS_EMM_SEC_IN,
      LTE_NAS_EMM_SEC_OUT,
      LTE_NAS_EMM_PLAIN_IN,
      LTE_NAS_EMM_PLAIN_OUT,
      LTE_NAS_EMM_STATE,
      NR_RRC_OTA,
      NR_RRC_MIB,
      NR_RRC_SERVING,
      NR_RRC_CONFIG,
      NR_RRC_CA,
      NR_RRC_PLMN_SEARCH_REQ,
      NR_RRC_PLMN_SEARCH_RSP,
      NR_RRC_DETECTED_CELL,
      NR_ML1_SEARCHER_ACQ,
      NR_ML1_SEARCHER_MEAS_CFG,
      NR_ML1_MEAS_DB,
  };
  if (pack == LteDiagPack::Serving) {
    ids.push_back(LTE_LL1_FRAME_TIMING);
    ids.push_back(LTE_MAC_RACH_TRIGGER);
    ids.push_back(LTE_MAC_RACH_RESPONSE);
    ids.push_back(LTE_MAC_DL_TB);
    ids.push_back(LTE_MAC_UL_TB);
    ids.push_back(LTE_ML1_MAC_RAR_MSG1);
    ids.push_back(LTE_ML1_MAC_RAR_MSG2);
    ids.push_back(LTE_ML1_MAC_MSG3);
    ids.push_back(LTE_ML1_MAC_MSG4);
  }
  return ids;
}

class DiagSession {
public:
  using SendFn = std::function<bool(std::span<const uint8_t> frame)>;

  explicit DiagSession(SendFn send) : m_send(std::move(send)) {}

  /// Enable cell-relevant LTE/GSM/WCDMA masks (scat-style), then commit.
  /// Does not DISABLE_ALL first (partial disable can permanently mute DIAG).
  bool init_modem() { return init_modem(DiagMaskProfile::AllRats); }

  bool init_modem(DiagMaskProfile profile) {
    switch (profile) {
      case DiagMaskProfile::WcdmaOnly:
        // Explicit empty LTE mask — otherwise previous session / firmware keeps B175/B113.
        if (!clear_lte_mask()) return false;
        if (!set_gsm_mask()) return false;  // IRAT 2G from 0x4005
        if (!set_wcdma_mask()) return false;
        if (!set_umts_nas_mask()) return false;
        break;
      case DiagMaskProfile::LteOnly:
        if (!set_lte_mask(LteDiagPack::Search)) return false;
        if (!clear_wcdma_mask()) return false;
        if (!set_gsm_mask()) return false;
        break;
      case DiagMaskProfile::AllRats:
      default:
        if (!set_lte_mask(LteDiagPack::Search)) return false;
        if (!set_gsm_mask()) return false;
        if (!set_wcdma_mask()) return false;
        if (!set_umts_nas_mask()) return false;
        break;
    }
    if (!commit_masks()) return false;
    (void)quiet_f3_messages();
    return true;
  }

  bool zero_log() {
    if (!send_log_config_op(DiagLogOp::DISABLE_ALL)) return false;
    delay(10);
    if (!quiet_f3_messages()) return false;
    if (!send_log_config_op(DiagLogOp::RETRIEVE_ID_RANGES)) return false;
    delay(20);
    return true;
  }

  bool quiet_f3_messages() {
    uint8_t msg_payload[8] = {};
    msg_payload[0] = 4;  // MSG_EXT_SUBCMD_SET_ALL_RT_MASKS
    if (!send_cmd(DiagOpcode::EXT_MSG_CONFIG, {msg_payload, 7})) return false;
    delay(10);
    return true;
  }

  /// LTE SET_MASK only (no DISABLE_ALL). Safe to call mid-session to switch packs.
  bool apply_lte_pack(LteDiagPack pack) {
    if (!set_lte_mask(pack)) return false;
    return commit_masks();
  }

  /// RRC/ML1/NAS (+ SSS). Serving pack adds B114 TA. Never includes PSS/CER flood.
  bool set_lte_mask(LteDiagPack pack = LteDiagPack::Search) {
    return send_set_mask(DiagEquip::LTE, kLteMaskLastItem, lte_diag_item_ids(pack));
  }

  bool set_gsm_mask() {
    return send_set_mask(DiagEquip::GSM, 0x0FF7,
                         {
                             DiagItem::GSM_L1_FCCH,
                             DiagItem::GSM_L1_SCH,
                             DiagItem::GSM_L1_BURST_METRICS,
                             DiagItem::GSM_L1_SCELL_BA_LIST,
                             DiagItem::GSM_L1_SCELL_AUX,
                             DiagItem::GSM_L1_NCELL_AUX,
                             DiagItem::GSM_RR_SIGNALING,
                             DiagItem::GSM_RR_CELL_INFORMATION,
                             DiagItem::GSM_DSDS_RR_SIGNALING,
                             DiagItem::GSM_DSDS_RR_CELL_INFO,
                         });
  }

  bool set_wcdma_mask() {
    return send_set_mask(DiagEquip::WCDMA, 0x0FF7,
                         {
                             DiagItem::WCDMA_RESEL_RANK,
                             DiagItem::WCDMA_CELL_ID_LEGACY,
                             DiagItem::WCDMA_ACTIVE_SET,
                             DiagItem::WCDMA_CELL_ID,
                             DiagItem::WCDMA_SIB,
                             DiagItem::WCDMA_SIGNALING,
                             DiagItem::WCDMA_PN_SEARCH,
                             DiagItem::WCDMA_FREQ_SCAN,
                         });
  }

  bool set_umts_nas_mask() {
    return send_set_mask(DiagEquip::UMTS, 0x0FF7, {DiagItem::UMTS_NAS_OTA});
  }

  /// Empty bitfield = mute that equipment (stops LTE ML1 flood in 3G survey).
  bool clear_lte_mask() { return send_set_mask(DiagEquip::LTE, kLteMaskLastItem, {}); }
  bool clear_wcdma_mask() { return send_set_mask(DiagEquip::WCDMA, 0x0FF7, {}); }

  bool commit_masks() { return send_log_config_op(DiagLogOp::RETRIEVE_ID_RANGES); }

  static std::vector<uint8_t> build_set_mask(uint32_t equip, uint32_t last_item, const uint32_t* bits,
                                            size_t n) {
    const size_t nbit_bytes = bytes_for_bitfield(last_item);
    std::vector<uint8_t> pkt(16 + nbit_bytes, 0);
    write_le32(pkt.data(), 0, DiagOpcode::LOG_CONFIG);
    write_le32(pkt.data(), 4, DiagLogOp::SET_MASK);
    write_le32(pkt.data(), 8, equip);
    write_le32(pkt.data(), 12, last_item);
    for (size_t i = 0; i < n; ++i) {
      const uint32_t bit = bits[i];
      if (bit > last_item) continue;
      pkt[16 + bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
    }
    return pkt;
  }

  static std::vector<uint8_t> build_set_mask(uint32_t equip, uint32_t last_item,
                                            const std::vector<uint32_t>& bits) {
    return build_set_mask(equip, last_item, bits.data(), bits.size());
  }

  [[nodiscard]] static bool set_mask_has_item(std::span<const uint8_t> pkt, uint32_t item) noexcept {
    if (pkt.size() < 16) return false;
    uint32_t last_item = 0;
    std::memcpy(&last_item, pkt.data() + 12, 4);
    if (item > last_item) return false;
    const size_t idx = 16 + item / 8;
    if (idx >= pkt.size()) return false;
    return (pkt[idx] & static_cast<uint8_t>(1u << (item % 8))) != 0;
  }

private:
  SendFn m_send;

  static size_t bytes_for_bitfield(uint32_t last_item) noexcept {
    const uint32_t nbits = last_item + 1;
    return (nbits + 7) / 8;
  }

  static void write_le32(uint8_t* dst, size_t offset, uint32_t val) noexcept {
    std::memcpy(dst + offset, &val, 4);
  }

  bool send_diag_packet(std::span<const uint8_t> pkt) {
    if (pkt.empty()) return false;
    return send_cmd(pkt[0], pkt.subspan(1));
  }

  bool send_set_mask(uint32_t equip, uint32_t last_item, std::initializer_list<uint32_t> bits) {
    auto pkt = build_set_mask(equip, last_item, bits.begin(), bits.size());
    const bool ok = send_diag_packet(pkt);
    delay(15);
    return ok;
  }

  bool send_set_mask(uint32_t equip, uint32_t last_item, const std::vector<uint32_t>& bits) {
    auto pkt = build_set_mask(equip, last_item, bits);
    const bool ok = send_diag_packet(pkt);
    delay(15);
    return ok;
  }

  bool send_cmd(uint8_t opcode, std::span<const uint8_t> payload) {
    auto frame = HdlcCodec::serialize(opcode, payload);
    std::vector<uint8_t> wire;
    wire.reserve(frame.size() + 1);
    wire.push_back(HdlcCodec::FLAG);
    wire.insert(wire.end(), frame.begin(), frame.end());
    return m_send({wire.data(), wire.size()});
  }

  bool send_log_config_op(uint32_t op) {
    uint8_t buf[8] = {};
    write_le32(buf, 0, DiagOpcode::LOG_CONFIG);
    write_le32(buf, 4, op);
    return send_diag_packet(buf);
  }

  static void delay(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
};

}  // namespace QCom
