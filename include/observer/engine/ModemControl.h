/// @file ModemControl.h
/// @brief Control port (actuation) — the OUT direction, symmetric to IDataSource.
///
/// Ports & Adapters: the engine talks to the modem through this interface; each
/// platform provides an adapter (SIM8300 AT+DIAG, Android DCI, offline null).
/// Platforms differ wildly in what they allow, so control is paired with a
/// capability descriptor. A survey strategy declares what it *needs*; at startup
/// the runner negotiates against what the modem *offers* and degrades gracefully
/// (full active walk on SIM8300 → passive monitor on a locked-down Android phone).
///
/// The engine emits high-level *intents* ("lock this cell", "select this PLMN"),
/// never raw AT/QMI/DCI bytes — the adapter owns the dialect and the quirks.
#pragma once

#include <cstdint>
#include <string_view>

namespace QCom::Engine {

/// What a modem/platform can actually do. Read paths are handled by IDataSource;
/// this describes the *control* surface only.
struct ModemCaps {
  bool diag_log_mask{false};  ///< can (re)program which DIAG log codes stream
  bool at_commands{false};    ///< raw AT channel available (SIM8300 tty)
  bool cell_lock{false};      ///< can pin EARFCN|PCI (CCELLCFG/CLECELL)
  bool plmn_select{false};    ///< can force operator (COPS)
  bool rf_control{false};     ///< can toggle RF / airplane (CFUN)

  [[nodiscard]] constexpr bool can_active_walk() const noexcept {
    return cell_lock && rf_control;  // minimum needed to hop+camp
  }
  static constexpr ModemCaps none() noexcept { return {}; }
  static constexpr ModemCaps full() noexcept {
    return {.diag_log_mask = true,
            .at_commands = true,
            .cell_lock = true,
            .plmn_select = true,
            .rf_control = true};
  }
};

/// A high-level command the strategy asks the runner to perform. Payload is a
/// small flat struct (no polymorphism) — trivially copyable and testable.
struct SurveyIntent {
  enum class Kind : uint8_t {
    Idle,        ///< nothing to do (passive / satisfied)
    LockCell,    ///< pin earfcn+pci and camp
    Unlock,      ///< release any cell/freq lock
    SelectPlmn,  ///< force PLMN (mcc/mnc)
    Deregister,  ///< COPS=2 style detach (before deep search)
    Rediscover,  ///< drop locks and re-scan the band
    SetRf,       ///< toggle RF (rf_on)
  };

  Kind kind{Kind::Idle};
  uint32_t earfcn{0};
  uint16_t pci{0};
  uint16_t mcc{0};
  uint16_t mnc{0};
  bool rf_on{true};

  static constexpr SurveyIntent idle() noexcept { return {}; }
  static constexpr SurveyIntent lock(uint32_t e, uint16_t p) noexcept {
    return {.kind = Kind::LockCell, .earfcn = e, .pci = p};
  }
  static constexpr SurveyIntent unlock() noexcept { return {.kind = Kind::Unlock}; }
  static constexpr SurveyIntent select_plmn(uint16_t mcc_, uint16_t mnc_) noexcept {
    return {.kind = Kind::SelectPlmn, .mcc = mcc_, .mnc = mnc_};
  }
  static constexpr SurveyIntent deregister() noexcept { return {.kind = Kind::Deregister}; }
  static constexpr SurveyIntent rediscover() noexcept { return {.kind = Kind::Rediscover}; }
  static constexpr SurveyIntent set_rf(bool on) noexcept {
    return {.kind = Kind::SetRf, .rf_on = on};
  }
};

/// Result of executing an intent (adapters return this; strategies may inspect).
enum class ControlStatus : uint8_t { Ok, Unsupported, Failed, Busy };

/// Control port. Adapters live at the platform edge (see SimcomAtControl,
/// AndroidControl, NullModemControl). The engine only ever sees this interface.
class IModemControl {
public:
  virtual ~IModemControl() = default;

  [[nodiscard]] virtual ModemCaps caps() const noexcept = 0;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  /// Execute one high-level intent. Adapters MUST return Unsupported (not crash)
  /// for anything outside their caps — the runner relies on graceful degrade.
  virtual ControlStatus apply(const SurveyIntent& intent) = 0;
};

/// Offline / read-only adapter: advertises no control, ignores every intent.
/// Used for journal replay and unit tests. Android phones that cannot even set
/// a DIAG mask should use this; phones that can program DCI use AndroidControl.
class NullModemControl final : public IModemControl {
public:
  [[nodiscard]] ModemCaps caps() const noexcept override { return ModemCaps::none(); }
  [[nodiscard]] std::string_view name() const noexcept override { return "null-control"; }
  ControlStatus apply(const SurveyIntent& intent) override {
    return intent.kind == SurveyIntent::Kind::Idle ? ControlStatus::Ok : ControlStatus::Unsupported;
  }
};

/// Typical locked-down Android Snapdragon: DIAG/DCI log-mask yes, cell lock no.
/// Pair with PassiveMonitorStrategy (or let SurveySession negotiate the downgrade).
class AndroidControl final : public IModemControl {
public:
  [[nodiscard]] ModemCaps caps() const noexcept override { return {.diag_log_mask = true}; }
  [[nodiscard]] std::string_view name() const noexcept override { return "android-dci"; }
  ControlStatus apply(const SurveyIntent& intent) override {
    return intent.kind == SurveyIntent::Kind::Idle ? ControlStatus::Ok : ControlStatus::Unsupported;
  }
};

/// True when the modem offers everything the strategy needs. When false the
/// runner should fall back to a passive strategy instead of failing.
[[nodiscard]] constexpr bool caps_satisfy(ModemCaps have, ModemCaps need) noexcept {
  return (!need.diag_log_mask || have.diag_log_mask) && (!need.at_commands || have.at_commands) &&
         (!need.cell_lock || have.cell_lock) && (!need.plmn_select || have.plmn_select) &&
         (!need.rf_control || have.rf_control);
}

}  // namespace QCom::Engine
