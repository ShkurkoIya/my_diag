#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fmt/format.h>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <observer/model/CellIdentity.h>

namespace Observer {

struct CeregIdentity {
  uint32_t tac{0};
  uint32_t cell_id{0};
  bool ok{false};
};

[[nodiscard]] inline std::vector<std::string> split_csv_tokens(std::string_view line) {
  std::vector<std::string> toks;
  std::string cur;
  for (char c : line) {
    if (c == ',') {
      toks.push_back(cur);
      cur.clear();
    } else if (c != ' ' && c != '"') {
      cur.push_back(c);
    }
  }
  toks.push_back(cur);
  return toks;
}

[[nodiscard]] inline CeregIdentity parse_cereg_or_creg(std::string_view resp) {
  CeregIdentity out;
  auto pos = resp.find("+CEREG:");
  size_t prefix = 7;
  if (pos == std::string_view::npos) {
    pos = resp.find("+CREG:");
    prefix = 6;
  }
  if (pos == std::string_view::npos) return out;
  std::string line(resp.substr(pos + prefix));
  if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);

  auto toks = split_csv_tokens(line);
  if (toks.size() < 4) return out;
  try {
    out.tac = static_cast<uint32_t>(std::stoul(toks[2], nullptr, 16));
    out.cell_id = static_cast<uint32_t>(std::stoul(toks[3], nullptr, 16));
    out.ok = (out.tac != 0 && out.cell_id != 0);
  } catch (...) {
    out.ok = false;
  }
  return out;
}

/// SIMCOM AT+CPSI? LTE line — full serving passport + RF key in one shot.
/// Example: +CPSI: LTE,Online,250-20,0x4D07,200468759,468,EUTRAN-BAND7,3400,3,3,-69,-900,-661,20
struct CpsiServing {
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint32_t tac{0};
  uint32_t cell_id{0};
  uint16_t pci{0};
  uint32_t earfcn{0};
  uint8_t dl_bw_mhz{0};
  uint8_t ul_bw_mhz{0};
  float rsrp{-999.0f};
  float rsrq{-999.0f};
  bool ok{false};
};

/// SIMCOM AT+CPSI? WCDMA line.
/// Example: +CPSI: WCDMA,Online,250-20,0x4D07,4487261,WCDMA IMT 2000,38,10563,0,4.0,69,32,46,500
struct CpsiWcdmaServing {
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint32_t lac{0};
  uint32_t cell_id{0};
  uint16_t psc{0};
  uint32_t uarfcn{0};
  float rscp{-999.0f};
  float ecio{-999.0f};
  bool ok{false};
};

constexpr int kCopsActUtran = 2;  // 3GPP TS 27.007 AcT = UTRAN

[[nodiscard]] inline uint8_t cpsi_bw_index_to_mhz(int idx) noexcept {
  switch (idx) {
    case 0: return 1;   // 1.4
    case 1: return 3;
    case 2: return 5;
    case 3: return 10;
    case 4: return 15;
    case 5: return 20;
    default: return 0;
  }
}

[[nodiscard]] inline bool at_reply_ok(std::string_view resp) noexcept {
  return resp.find("ERROR") == std::string_view::npos &&
         (resp.find("\nOK") != std::string_view::npos || resp.find("\rOK") != std::string_view::npos ||
          resp.find("OK\n") != std::string_view::npos || resp.ends_with("OK") ||
          resp.find("\nOK\r") != std::string_view::npos);
}

/// AT+CNWINFO? LTE — EGCI (= MCC|MNC|ECI) + eNB. No PCI/EARFCN (needs serving key).
struct CnwServing {
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint32_t cell_id{0};
  bool ok{false};
};

[[nodiscard]] inline CnwServing parse_cnwinfo_lte(std::string_view resp) {
  CnwServing out;
  auto pos = resp.find("+CNWINFO:");
  if (pos == std::string_view::npos) return out;
  std::string line(resp.substr(pos + 9));
  if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);
  auto toks = split_csv_tokens(line);
  if (toks.size() < 2 || toks[0] != "LTE") return out;
  const std::string& egci = toks[1];
  if (egci.size() < 8) return out;
  try {
    const uint16_t mcc = static_cast<uint16_t>(std::stoul(egci.substr(0, 3)));
    // Prefer 2-digit MNC (RU/EU); fall back to 3-digit.
    for (int mnc_digits : {2, 3}) {
      if (egci.size() <= static_cast<size_t>(3 + mnc_digits)) continue;
      const uint16_t mnc =
          static_cast<uint16_t>(std::stoul(egci.substr(3, static_cast<size_t>(mnc_digits))));
      const uint64_t eci = std::stoull(egci.substr(static_cast<size_t>(3 + mnc_digits)));
      if (mcc < 100 || mcc > 999 || eci == 0 || eci > 0xFFFFFFFu) continue;
      out.mcc = mcc;
      out.mnc = mnc;
      out.cell_id = static_cast<uint32_t>(eci);
      out.ok = true;
      break;
    }
  } catch (...) {
    out.ok = false;
  }
  return out;
}

[[nodiscard]] inline CpsiServing parse_cpsi_lte(std::string_view resp) {
  // Modem may emit several +CPSI URC lines; pick the last valid LTE Online/Limited.
  CpsiServing best;
  for (std::size_t pos = 0; pos < resp.size();) {
    const auto at = resp.find("+CPSI:", pos);
    if (at == std::string_view::npos) break;
    pos = at + 6;
    std::string line(resp.substr(pos));
    if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);
    auto toks = split_csv_tokens(line);
    if (toks.size() < 8 || toks[0] != "LTE") continue;
    if (toks[1].find("Online") == std::string::npos &&
        toks[1].find("Limited") == std::string::npos)
      continue;
    CpsiServing out;
    try {
      std::string plmn = toks[2];
      auto dash = plmn.find('-');
      if (dash != std::string::npos) {
        out.mcc = static_cast<uint16_t>(std::stoul(plmn.substr(0, dash)));
        out.mnc = static_cast<uint16_t>(std::stoul(plmn.substr(dash + 1)));
      } else if (plmn.size() >= 5) {
        out.mcc = static_cast<uint16_t>(std::stoul(plmn.substr(0, 3)));
        out.mnc = static_cast<uint16_t>(std::stoul(plmn.substr(3)));
      } else {
        continue;
      }
      std::string tac_s = toks[3];
      if (tac_s.size() > 2 && tac_s[0] == '0' && (tac_s[1] == 'x' || tac_s[1] == 'X'))
        tac_s = tac_s.substr(2);
      out.tac = static_cast<uint32_t>(std::stoul(tac_s, nullptr, 16));
      out.cell_id = static_cast<uint32_t>(std::stoul(toks[4], nullptr, 10));
      out.pci = static_cast<uint16_t>(std::stoul(toks[5], nullptr, 10));
      out.earfcn = static_cast<uint32_t>(std::stoul(toks[7], nullptr, 10));
      if (toks.size() > 8) out.dl_bw_mhz = cpsi_bw_index_to_mhz(std::stoi(toks[8]));
      if (toks.size() > 9) out.ul_bw_mhz = cpsi_bw_index_to_mhz(std::stoi(toks[9]));
      if (toks.size() > 10) out.rsrq = static_cast<float>(std::stoi(toks[10])) / 10.0f;
      if (toks.size() > 11) out.rsrp = static_cast<float>(std::stoi(toks[11])) / 10.0f;
    } catch (...) {
      continue;
    }
    // Reject transitional garbage: PCI=0 / EARFCN=0xFFFFFFFF / BAND0 during reselection.
    out.ok = (out.earfcn > 0 && out.earfcn <= 262143 && out.pci >= 1 && out.pci <= 503 &&
              out.cell_id != 0 && out.tac != 0 && out.mcc >= 100);
    if (out.ok) best = out;
  }
  return best;
}

[[nodiscard]] inline CpsiWcdmaServing parse_cpsi_wcdma(std::string_view resp) {
  CpsiWcdmaServing best;
  for (std::size_t pos = 0; pos < resp.size();) {
    const auto at = resp.find("+CPSI:", pos);
    if (at == std::string_view::npos) break;
    pos = at + 6;
    std::string line(resp.substr(pos));
    if (auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);
    auto toks = split_csv_tokens(line);
    // RAT, status, PLMN, LAC, CID, Band, PSC, UARFCN, SSC, EcIo, RSCP, …
    if (toks.size() < 8 || toks[0] != "WCDMA") continue;
    if (toks[1].find("Online") == std::string::npos &&
        toks[1].find("Limited") == std::string::npos)
      continue;
    CpsiWcdmaServing out;
    try {
      std::string plmn = toks[2];
      auto dash = plmn.find('-');
      if (dash != std::string::npos) {
        out.mcc = static_cast<uint16_t>(std::stoul(plmn.substr(0, dash)));
        out.mnc = static_cast<uint16_t>(std::stoul(plmn.substr(dash + 1)));
      } else if (plmn.size() >= 5) {
        out.mcc = static_cast<uint16_t>(std::stoul(plmn.substr(0, 3)));
        out.mnc = static_cast<uint16_t>(std::stoul(plmn.substr(3)));
      } else {
        continue;
      }
      std::string lac_s = toks[3];
      if (lac_s.size() > 2 && lac_s[0] == '0' && (lac_s[1] == 'x' || lac_s[1] == 'X'))
        lac_s = lac_s.substr(2);
      out.lac = static_cast<uint32_t>(std::stoul(lac_s, nullptr, 16));
      out.cell_id = static_cast<uint32_t>(std::stoul(toks[4], nullptr, 10));
      out.psc = static_cast<uint16_t>(std::stoul(toks[6], nullptr, 10));
      out.uarfcn = static_cast<uint32_t>(std::stoul(toks[7], nullptr, 10));
      if (toks.size() > 9) {
        const float e = std::stof(toks[9]);
        out.ecio = (e > 0.0f) ? -e : e;
      }
      if (toks.size() > 10) {
        const float r = std::stof(toks[10]);
        // SIMCOM often reports RSCP as positive ASU-like; map to dBm.
        out.rscp = (r > 0.0f && r < 120.0f) ? (r - 116.0f) : r;
      }
    } catch (...) {
      continue;
    }
    out.ok = (out.uarfcn > 0 && out.uarfcn <= 16383 && out.psc <= 511 && out.cell_id != 0 &&
              out.lac != 0 && out.mcc >= 100);
    if (out.ok) best = out;
  }
  return best;
}

[[nodiscard]] inline bool cpsi_is_wcdma_or_noservice(std::string_view resp) noexcept {
  // Valid LTE passport present → still on LTE (ignore WCDMA URC noise).
  if (parse_cpsi_lte(resp).ok) return false;
  return resp.find("WCDMA") != std::string_view::npos ||
         resp.find("NO SERVICE") != std::string_view::npos ||
         resp.find("No service") != std::string_view::npos;
}

/// Modem RAT as seen by AT+CPSI? — used by rat-guard vs scan policy.
enum class ObservedRat : uint8_t { Unknown = 0, NoService, Gsm, Wcdma, Lte, Nr };

[[nodiscard]] inline const char* to_string(ObservedRat r) noexcept {
  switch (r) {
    case ObservedRat::NoService: return "NO_SERVICE";
    case ObservedRat::Gsm: return "GSM";
    case ObservedRat::Wcdma: return "WCDMA";
    case ObservedRat::Lte: return "LTE";
    case ObservedRat::Nr: return "NR";
    default: return "UNKNOWN";
  }
}

[[nodiscard]] inline ObservedRat observe_rat_from_cpsi(std::string_view resp) noexcept {
  if (parse_cpsi_lte(resp).ok) return ObservedRat::Lte;
  if (parse_cpsi_wcdma(resp).ok) return ObservedRat::Wcdma;
  // Order matters: NR/LTE tokens before generic.
  if (resp.find("NR") != std::string_view::npos &&
      (resp.find("NR5G") != std::string_view::npos || resp.find("NR,") != std::string_view::npos))
    return ObservedRat::Nr;
  if (resp.find("LTE") != std::string_view::npos) return ObservedRat::Lte;  // transitional
  if (resp.find("WCDMA") != std::string_view::npos || resp.find("UMTS") != std::string_view::npos)
    return ObservedRat::Wcdma;
  if (resp.find("GSM") != std::string_view::npos) return ObservedRat::Gsm;
  if (resp.find("NO SERVICE") != std::string_view::npos ||
      resp.find("No service") != std::string_view::npos)
    return ObservedRat::NoService;
  return ObservedRat::Unknown;
}

/// AT+CMGRMI=4 is a NAS report. RF-lock with `NO SERVICE` / 3G always ERROR.
/// Empty raw = unknown (caller has not polled CPSI) → allow one try.
[[nodiscard]] inline bool cpsi_cmgrmi_ready(std::string_view resp) noexcept {
  if (resp.empty()) return true;
  if (parse_cpsi_lte(resp).ok) return true;
  return observe_rat_from_cpsi(resp) == ObservedRat::Lte;
}

[[nodiscard]] inline bool observed_matches_scan(ObservedRat obs, QCom::RatType want) noexcept {
  if (want == QCom::RatType::LTE) return obs == ObservedRat::Lte;
  if (want == QCom::RatType::WCDMA) return obs == ObservedRat::Wcdma;
  if (want == QCom::RatType::GSM) return obs == ObservedRat::Gsm;
  if (want == QCom::RatType::NR) return obs == ObservedRat::Nr;
  return true;
}

[[nodiscard]] inline std::optional<std::pair<uint16_t, uint16_t>> parse_cops_numeric_plmn(
    std::string_view resp) {
  auto pos = resp.find("+COPS:");
  if (pos == std::string_view::npos) return std::nullopt;
  auto q1 = resp.find('"', pos);
  if (q1 == std::string_view::npos) return std::nullopt;
  auto q2 = resp.find('"', q1 + 1);
  if (q2 == std::string_view::npos || q2 <= q1 + 1) return std::nullopt;
  std::string plmn(resp.substr(q1 + 1, q2 - q1 - 1));
  // Allow "250-20" or "25020"
  plmn.erase(std::remove(plmn.begin(), plmn.end(), '-'), plmn.end());
  if (plmn.size() < 5 || plmn.size() > 6) return std::nullopt;
  try {
    const uint16_t mcc = static_cast<uint16_t>(std::stoul(plmn.substr(0, 3)));
    const uint16_t mnc = static_cast<uint16_t>(std::stoul(plmn.substr(3)));
    if (mcc < 100 || mcc > 999) return std::nullopt;
    return std::make_pair(mcc, mnc);
  } catch (...) {
    return std::nullopt;
  }
}

/// All numeric PLMNs quoted in AT+COPS=? / AT+COPS? replies.
[[nodiscard]] inline std::vector<std::pair<uint16_t, uint16_t>> parse_cops_plmn_list(
    std::string_view resp) {
  std::set<std::pair<uint16_t, uint16_t>> uniq;
  for (std::size_t i = 0; i < resp.size(); ++i) {
    if (resp[i] != '"') continue;
    const auto j = resp.find('"', i + 1);
    if (j == std::string_view::npos) break;
    std::string tok(resp.substr(i + 1, j - i - 1));
    tok.erase(std::remove(tok.begin(), tok.end(), '-'), tok.end());
    i = j;
    if (tok.size() < 5 || tok.size() > 6) continue;
    bool digits = true;
    for (char c : tok) {
      if (!std::isdigit(static_cast<unsigned char>(c))) {
        digits = false;
        break;
      }
    }
    if (!digits) continue;
    try {
      const uint16_t mcc = static_cast<uint16_t>(std::stoul(tok.substr(0, 3)));
      const uint16_t mnc = static_cast<uint16_t>(std::stoul(tok.substr(3)));
      if (mcc >= 100 && mcc <= 999) uniq.insert({mcc, mnc});
    } catch (...) {
    }
  }
  return {uniq.begin(), uniq.end()};
}

[[nodiscard]] inline std::string format_plmn_numeric(uint16_t mcc, uint16_t mnc) {
  if (mnc >= 100) return fmt::format("{:03}{:03}", mcc, mnc);
  return fmt::format("{:03}{:02}", mcc, mnc);
}

/// 3GPP TS 27.007 +COPS AcT: 7 = E-UTRAN (LTE). Pin selects to 4G so ghost/foreign
/// PLMN does not fall into WCDMA/GSM search.
constexpr int kCopsActLte = 7;

/// AT+COPS=1,2,"mccmnc"[,AcT] — manual numeric PLMN (+ optional RAT).
[[nodiscard]] inline std::string format_cops_manual_select(std::string_view plmn_numeric,
                                                    int act = kCopsActLte) {
  if (act >= 0) return fmt::format("AT+COPS=1,2,\"{}\",{}", plmn_numeric, act);
  return fmt::format("AT+COPS=1,2,\"{}\"", plmn_numeric);
}

/// Parse CLI MCCMNC (`00101`, `250-01`, `25001`) for ghost / manual PLMN.
[[nodiscard]] inline std::optional<std::pair<uint16_t, uint16_t>> parse_plmn_arg(
    std::string_view s) {
  std::string d;
  d.reserve(s.size());
  for (char c : s) {
    if (std::isdigit(static_cast<unsigned char>(c))) d.push_back(c);
  }
  if (d.size() < 5 || d.size() > 6) return std::nullopt;
  try {
    const uint16_t mcc = static_cast<uint16_t>(std::stoul(d.substr(0, 3)));
    const uint16_t mnc = static_cast<uint16_t>(std::stoul(d.substr(3)));
    if (mcc < 100 || mcc > 999) return std::nullopt;
    return std::make_pair(mcc, mnc);
  } catch (...) {
    return std::nullopt;
  }
}

/// EF_FPLMN (28539 / 0x6F7B) via AT+CRSM — empty when payload is 12×0xFF.
[[nodiscard]] inline bool crsm_fplmn_empty(std::string_view resp) noexcept {
  return resp.find("FFFFFFFFFFFFFFFFFFFFFFFF") != std::string_view::npos;
}

[[nodiscard]] inline bool crsm_sw_ok(std::string_view resp) noexcept {
  // Success SW1/SW2 usually 144,0 (0x9000) or 145,* (0x91xx).
  auto pos = resp.find("+CRSM:");
  if (pos == std::string_view::npos) return false;
  return resp.find("144,", pos) != std::string_view::npos ||
         resp.find("145,", pos) != std::string_view::npos;
}

}  // namespace Observer
