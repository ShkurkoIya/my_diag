/// @file SurveyStrategy.h
/// @brief Survey policy as a pure, testable state machine: state → intent.
///
/// A strategy never touches AT/DIAG/sleep. It looks at a tracker snapshot and
/// returns the next SurveyIntent; the runner executes it via IModemControl. This
/// is the single biggest lever on map completeness, so it lives behind an
/// interface and is unit-tested by feeding synthetic snapshot sequences.
///
/// Two concrete strategies bracket the capability spectrum:
///   - LteWalkStrategy    — active hop/camp (needs cell_lock + rf_control).
///   - PassiveMonitorStrategy — observe only (needs nothing; Android default).
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <observer/model/CellIdentity.h>
#include <observer/engine/ModemControl.h>
#include <observer/engine/SurveyProjection.h>
#include <observer/lte/LteHopPlanner.h>

namespace QCom::Engine {

/// Read-only inputs a strategy is allowed to see when deciding.
struct StrategyContext {
  const std::vector<CellIdentity>& snapshot;
  std::chrono::steady_clock::time_point now{};
};

class ISurveyStrategy {
public:
  virtual ~ISurveyStrategy() = default;

  /// Control capabilities this strategy needs to function at all.
  [[nodiscard]] virtual ModemCaps required_caps() const noexcept = 0;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  /// Decide the next action from the current state. Pure w.r.t. the outside world
  /// (may mutate the strategy's own bookkeeping).
  [[nodiscard]] virtual SurveyIntent decide(const StrategyContext& ctx) = 0;

  /// Optional feedback after the runner executes an intent.
  virtual void on_status(const SurveyIntent& /*intent*/, ControlStatus /*status*/) {}
};

/// Passive monitor: never actuates. Works anywhere (locked-down Android, journal
/// replay). The map fills only from what the modem naturally camps/reselects on.
class PassiveMonitorStrategy final : public ISurveyStrategy {
public:
  [[nodiscard]] ModemCaps required_caps() const noexcept override { return ModemCaps::none(); }
  [[nodiscard]] std::string_view name() const noexcept override { return "passive-monitor"; }
  [[nodiscard]] SurveyIntent decide(const StrategyContext&) override {
    return SurveyIntent::idle();
  }
};

/// Active LTE survey walk: lock the best incomplete EARFCN|PCI, camp until it
/// becomes FULL (SIB1/B0C2) or dwell expires, then advance to the next target.
///
/// State machine (per decide() tick):
///   no target  → pick next unvisited target from the planner → LockCell
///   have target, now FULL     → mark done, clear (next tick locks the next)
///   have target, dwell expired → mark visited, clear (advance)
///   have target, still camping → Idle (keep the lock)
///   no targets left            → Idle
class LteWalkStrategy final : public ISurveyStrategy {
public:
  struct Params {
    std::chrono::milliseconds dwell{std::chrono::seconds(6)};  ///< max camp per target
    std::size_t frontier{16};  ///< how many candidates the planner ranks
    bool full_walk{true};      ///< true: pick_full_walk_targets; false: pick_hop_targets
  };

  LteWalkStrategy() = default;
  explicit LteWalkStrategy(Params p) : m_p(p) {}

  [[nodiscard]] ModemCaps required_caps() const noexcept override {
    return {.cell_lock = true, .rf_control = true};
  }
  [[nodiscard]] std::string_view name() const noexcept override { return "lte-active-walk"; }

  [[nodiscard]] SurveyIntent decide(const StrategyContext& ctx) override {
    // 1) If we hold a target, evaluate it.
    if (m_target) {
      const bool became_full = target_is_full(ctx.snapshot, *m_target);
      const bool expired = (ctx.now - m_locked_at) >= m_p.dwell;
      if (became_full || expired) {
        m_visited.insert(*m_target);
        m_target.reset();
        // fall through to pick the next target this same tick
      } else {
        return SurveyIntent::idle();  // keep camping
      }
    }

    // 2) Pick the next unvisited target from the ranked planner output.
    const auto targets = m_p.full_walk ? Lte::pick_full_walk_targets(ctx.snapshot, m_p.frontier)
                                       : Lte::pick_hop_targets(ctx.snapshot, m_p.frontier);
    for (const auto& t : targets) {
      const Key k{t.earfcn, t.pci};
      if (m_visited.contains(k)) continue;
      m_target = k;
      m_locked_at = ctx.now;
      return SurveyIntent::lock(t.earfcn, t.pci);
    }

    return SurveyIntent::idle();  // nothing new to camp on
  }

  void on_status(const SurveyIntent& intent, ControlStatus status) override {
    // A lock the modem could not honour shouldn't wedge the walk — drop it.
    if (intent.kind == SurveyIntent::Kind::LockCell && status != ControlStatus::Ok) {
      m_target.reset();
    }
  }

  /// Test / introspection hooks.
  [[nodiscard]] std::size_t visited_count() const noexcept { return m_visited.size(); }
  void reset() {
    m_target.reset();
    m_visited.clear();
  }

private:
  using Key = std::pair<uint32_t, uint16_t>;

  static bool target_is_full(const std::vector<CellIdentity>& cells, Key k) {
    for (const auto& c : cells) {
      if (c.rat != RatType::LTE) continue;
      if (c.radio.freq() == k.first && c.radio.pci_bsic() == k.second) return is_lte_full(c);
    }
    return false;
  }

  Params m_p{};
  std::optional<Key> m_target;
  std::chrono::steady_clock::time_point m_locked_at{};
  std::set<Key> m_visited;
};

}  // namespace QCom::Engine
