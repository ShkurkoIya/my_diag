/// @file SimcomAtControl.h
/// @brief Linux SIMCOM (SIM8300/SIM8200) IModemControl adapter — AT dialect only.
///
/// No tty / sleep / retry policy lives here. The adapter turns SurveyIntent into
/// the SIMCOM command sequence (CLEARFCN → CCELLCFG → CLECELL) and parses replies.
/// The app supplies AtTransact (live_scanner wraps AtSession); tests supply a fake.
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <observer/model/BandInfo.h>
#include <observer/engine/ModemControl.h>

namespace QCom::Engine {

/// Command in, raw modem reply out. nullopt = transport failure (no tty / timeout).
using AtTransact = std::function<std::optional<std::string>(std::string_view cmd, int timeout_ms)>;

/// Pure SIMCOM AT dialect helpers (command strings + reply parsers). Testable
/// without a transact callback.
namespace SimcomAt {

[[nodiscard]] inline bool reply_ok(std::string_view resp) noexcept {
  if (resp.find("ERROR") != std::string_view::npos) return false;
  return resp.find("\nOK") != std::string_view::npos || resp.find("\rOK") != std::string_view::npos ||
         resp.find("OK\n") != std::string_view::npos || resp.ends_with("OK") ||
         resp.find("\nOK\r") != std::string_view::npos;
}

[[nodiscard]] inline std::vector<std::string> csv_tokens(std::string_view line) {
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

/// Tokens on the first line that starts with `tag` (e.g. "+CCELLCFG:").
[[nodiscard]] inline std::vector<std::string> urc_csv(std::string_view resp, std::string_view tag) {
  const auto pos = resp.find(tag);
  if (pos == std::string_view::npos) return {};
  std::string line(resp.substr(pos + tag.size()));
  if (const auto nl = line.find_first_of("\r\n"); nl != std::string::npos) line.resize(nl);
  return csv_tokens(line);
}

[[nodiscard]] inline std::string cmd_ccellcfg_lock(uint16_t pci, uint32_t earfcn) {
  return "AT+CCELLCFG=1," + std::to_string(pci) + "," + std::to_string(earfcn);
}
[[nodiscard]] inline std::string cmd_clecell_lock(uint32_t earfcn, uint16_t pci) {
  return "AT+CLECELL=" + std::to_string(earfcn) + "," + std::to_string(pci);
}
[[nodiscard]] inline std::string cmd_clearfcn_lock(uint8_t band, uint32_t earfcn) {
  return "AT+CLEARFCN=" + std::to_string(band) + "," + std::to_string(earfcn);
}
[[nodiscard]] inline std::string cmd_cops_manual(uint16_t mcc, uint16_t mnc) {
  char plmn[8];
  if (mnc >= 100)
    std::snprintf(plmn, sizeof(plmn), "%03u%03u", static_cast<unsigned>(mcc),
                  static_cast<unsigned>(mnc));
  else
    std::snprintf(plmn, sizeof(plmn), "%03u%02u", static_cast<unsigned>(mcc),
                  static_cast<unsigned>(mnc));
  return std::string("AT+COPS=1,2,\"") + plmn + '"';
}

[[nodiscard]] inline bool parse_ccellcfg(std::string_view resp, uint16_t& pci, uint32_t& earfcn) {
  const auto toks = urc_csv(resp, "+CCELLCFG:");
  if (toks.size() < 2) return false;
  try {
    pci = static_cast<uint16_t>(std::stoul(toks[0]));
    earfcn = static_cast<uint32_t>(std::stoul(toks[1]));
    return true;
  } catch (...) {
    return false;
  }
}

[[nodiscard]] inline bool parse_clecell(std::string_view resp, uint32_t& earfcn, uint16_t& pci) {
  if (resp.find("NOT IN WCDMA") != std::string_view::npos) return false;
  const auto toks = urc_csv(resp, "+CLECELL:");
  if (toks.size() < 2) return false;
  try {
    earfcn = static_cast<uint32_t>(std::stoul(toks[0]));
    pci = static_cast<uint16_t>(std::stoul(toks[1]));
    return true;
  } catch (...) {
    return false;
  }
}

}  // namespace SimcomAt

/// Linux SIM8300 control adapter. Caps = full active walk. I/O is injected.
class SimcomAtControl final : public IModemControl {
public:
  explicit SimcomAtControl(AtTransact tx) : m_tx(std::move(tx)) {}

  [[nodiscard]] ModemCaps caps() const noexcept override { return ModemCaps::full(); }
  [[nodiscard]] std::string_view name() const noexcept override { return "simcom-at"; }

  ControlStatus apply(const SurveyIntent& intent) override {
    switch (intent.kind) {
      case SurveyIntent::Kind::Idle:
        return ControlStatus::Ok;
      case SurveyIntent::Kind::LockCell:
        return lock_lte(intent.earfcn, intent.pci) ? ControlStatus::Ok : ControlStatus::Failed;
      case SurveyIntent::Kind::Unlock:
        (void)unlock_lte();  // best-effort; hop continues even if a clear ERROR's
        return ControlStatus::Ok;
      case SurveyIntent::Kind::SelectPlmn:
        return ok_cmd(SimcomAt::cmd_cops_manual(intent.mcc, intent.mnc), 15000);
      case SurveyIntent::Kind::Deregister:
        return ok_cmd("AT+COPS=2", 8000);
      case SurveyIntent::Kind::Rediscover:
        (void)unlock_lte();
        return ok_cmd("AT+COPS=0", 3000);
      case SurveyIntent::Kind::SetRf:
        return ok_cmd(intent.rf_on ? "AT+CFUN=1" : "AT+CFUN=4", 10000);
    }
    return ControlStatus::Unsupported;
  }

  /// Dual-lock: CLEARFCN(band,earfcn) → CCELLCFG(pci,earfcn) → CLECELL(earfcn,pci).
  /// Success if either sticky lock verifies (same rule as live_scanner).
  bool lock_lte(uint32_t earfcn, uint16_t pci) {
    if (pci == 0 || earfcn == 0) return false;
    (void)lock_earfcn(earfcn);
    const bool cfg = ok_cmd(SimcomAt::cmd_ccellcfg_lock(pci, earfcn), 2500) == ControlStatus::Ok &&
                     ccellcfg_matches(pci, earfcn);
    const bool cle = lock_clecell(earfcn, pci);
    return cfg || cle;
  }

  bool unlock_lte() {
    (void)ok_cmd("AT+CCELLCFG=0", 2000);
    (void)ok_cmd("AT+CLECELL", 2000);
    (void)ok_cmd("AT+CLEARFCN", 2000);
    return true;
  }

  bool lock_earfcn(uint32_t earfcn) {
    const auto bi = BandInfo::lte_from_earfcn(earfcn);
    if (!bi.band) return false;
    return ok_cmd(SimcomAt::cmd_clearfcn_lock(static_cast<uint8_t>(bi.band), earfcn), 2500) ==
           ControlStatus::Ok;
  }

  bool lock_clecell(uint32_t earfcn, uint16_t pci) {
    if (pci == 0 || earfcn == 0) return false;
    if (ok_cmd(SimcomAt::cmd_clecell_lock(earfcn, pci), 2500) != ControlStatus::Ok) return false;
    return clecell_matches(earfcn, pci);
  }

  [[nodiscard]] bool ccellcfg_matches(uint16_t pci, uint32_t earfcn) {
    const auto rsp = tx("AT+CCELLCFG?", 1500);
    if (!rsp) return false;
    uint16_t got_pci = 0;
    uint32_t got_earfcn = 0;
    return SimcomAt::parse_ccellcfg(*rsp, got_pci, got_earfcn) && got_pci == pci &&
           got_earfcn == earfcn;
  }

  [[nodiscard]] bool clecell_matches(uint32_t earfcn, uint16_t pci) {
    const auto rsp = tx("AT+CLECELL?", 1500);
    if (!rsp) return false;
    uint32_t got_earfcn = 0;
    uint16_t got_pci = 0;
    return SimcomAt::parse_clecell(*rsp, got_earfcn, got_pci) && got_earfcn == earfcn &&
           got_pci == pci;
  }

  [[nodiscard]] bool lte_lock_held(uint32_t earfcn, uint16_t pci) {
    return ccellcfg_matches(pci, earfcn) || clecell_matches(earfcn, pci);
  }

private:
  ControlStatus ok_cmd(std::string_view cmd, int timeout_ms) {
    const auto rsp = tx(cmd, timeout_ms);
    if (!rsp) return ControlStatus::Failed;
    return SimcomAt::reply_ok(*rsp) ? ControlStatus::Ok : ControlStatus::Failed;
  }

  std::optional<std::string> tx(std::string_view cmd, int timeout_ms) {
    if (!m_tx) return std::nullopt;
    return m_tx(cmd, timeout_ms);
  }

  AtTransact m_tx;
};

}  // namespace QCom::Engine
