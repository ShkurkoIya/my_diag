/// @file SurveySession.h
/// @brief Task-oriented RF-survey facade (progressive disclosure).
///
/// The 90% app path: pick a source + control + strategy, add a sink, poll. The
/// facade wires the ingest pipeline (IDataSource → parser → CellTracker), runs
/// the survey policy, executes intents through IModemControl, and emits *domain*
/// events (towers, operators, stats) to sinks — no raw-cell boilerplate in apps.
///
/// Escape hatches stay open: `engine()` / `tracker()` expose the lower layers for
/// custom low-level handlers (raw packet taps, direct tracker queries).
///
/// Deterministic + offline-testable: `ingest()` feeds packets without a live
/// device; `poll()` advances the policy one step for a given clock value.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include <observer/engine/ModemControl.h>
#include <observer/engine/SurveyDomain.h>
#include <observer/engine/SurveyProjection.h>
#include <observer/engine/SurveyStrategy.h>
#include <observer/io/DataSourceInterface.h>
#include <qcom/io/ScannerEngine.h>

namespace QCom::Engine {

/// Offline / inject-only data source: satisfies IDataSource without any device.
/// Packets arrive via SurveySession::ingest(), not from hardware.
class NullSource final : public IDataSource {
public:
  void set_frame_callback(FrameCallback cb) override { m_cb = std::move(cb); }
  [[nodiscard]] bool start() override {
    m_running = true;
    return true;
  }
  void stop() override { m_running = false; }
  [[nodiscard]] std::string_view name() const noexcept override { return "null-source"; }
  [[nodiscard]] bool is_running() const noexcept override { return m_running.load(); }

private:
  FrameCallback m_cb;
  std::atomic<bool> m_running{false};
};

/// Where survey output goes. Implement one per destination (JSON, GUI, console).
/// All sinks see the same domain types, so outputs never disagree.
class ISurveySink {
public:
  virtual ~ISurveySink() = default;
  /// Full projected result after each refresh (throttle inside the sink if noisy).
  virtual void on_result(const SurveyResult& /*result*/) {}
  /// A carrier that became FULL for the first time this session.
  virtual void on_tower_identified(const Tower& /*tower*/) {}
};

class SurveySession {
public:
  struct Config {
    bool active_walk{true};          ///< desired mode; downgraded if caps lack it
    LteWalkStrategy::Params walk{};  ///< tuning for the active walk
  };

  SurveySession(std::unique_ptr<IDataSource> source, std::unique_ptr<IModemControl> control,
                std::unique_ptr<ISurveyStrategy> strategy)
      : m_engine(std::make_unique<RadioScannerEngine>(std::move(source)))
      , m_control(std::move(control))
      , m_strategy(std::move(strategy)) {
    negotiate();
  }

  // --- lifecycle -----------------------------------------------------------
  [[nodiscard]] bool start() { return m_engine->start(); }
  void stop() { m_engine->stop(); }

  /// Feed a packet directly (offline / tests / secondary producers).
  void ingest(QualcommPacketView pkt) { m_engine->inject_packet(pkt); }

  /// Advance the survey one step: refresh domain, decide, actuate. Returns the
  /// intent that was issued (Idle when passive or satisfied).
  SurveyIntent poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    refresh();
    const auto snap = m_engine->tracker().get_snapshot();
    SurveyIntent intent = m_strategy->decide(StrategyContext{.snapshot = snap, .now = now});
    if (intent.kind != SurveyIntent::Kind::Idle) {
      const ControlStatus st = m_control->apply(intent);
      m_strategy->on_status(intent, st);
    }
    return intent;
  }

  /// Project the current tracker state into the survey domain (also notifies
  /// sinks and fires on_tower_identified for newly-FULL carriers).
  const SurveyResult& refresh() {
    m_last = project_lte(m_engine->tracker().get_snapshot());
    for (const auto& t : m_last.towers) {
      if (m_seen_eci.insert(t.eci).second) {
        for (auto* s : m_sinks) s->on_tower_identified(t);
      }
    }
    for (auto* s : m_sinks) s->on_result(m_last);
    return m_last;
  }

  [[nodiscard]] const SurveyResult& result() const noexcept { return m_last; }

  // --- wiring --------------------------------------------------------------
  void add_sink(ISurveySink* sink) {
    if (sink) m_sinks.push_back(sink);
  }

  [[nodiscard]] bool downgraded_to_passive() const noexcept { return m_downgraded; }
  [[nodiscard]] std::string_view strategy_name() const noexcept { return m_strategy->name(); }
  [[nodiscard]] ModemCaps caps() const noexcept { return m_control->caps(); }

  // --- escape hatches (lower layers stay reachable) ------------------------
  [[nodiscard]] RadioScannerEngine& engine() noexcept { return *m_engine; }
  [[nodiscard]] CellTracker& tracker() noexcept { return m_engine->tracker(); }

  // --- ergonomic builder ---------------------------------------------------
  class Builder {
  public:
    Builder& source(std::unique_ptr<IDataSource> s) {
      m_source = std::move(s);
      return *this;
    }
    Builder& control(std::unique_ptr<IModemControl> c) {
      m_control = std::move(c);
      return *this;
    }
    Builder& strategy(std::unique_ptr<ISurveyStrategy> st) {
      m_strategy = std::move(st);
      return *this;
    }
    Builder& config(Config c) {
      m_cfg = c;
      return *this;
    }
    /// If no strategy was set: active walk when the modem can hop, else passive.
    [[nodiscard]] SurveySession build() {
      if (!m_source) m_source = std::make_unique<NullSource>();
      if (!m_control) m_control = std::make_unique<NullModemControl>();
      if (!m_strategy) {
        if (m_cfg.active_walk && m_control->caps().can_active_walk())
          m_strategy = std::make_unique<LteWalkStrategy>(m_cfg.walk);
        else
          m_strategy = std::make_unique<PassiveMonitorStrategy>();
      }
      return SurveySession(std::move(m_source), std::move(m_control), std::move(m_strategy));
    }

  private:
    std::unique_ptr<IDataSource> m_source;
    std::unique_ptr<IModemControl> m_control;
    std::unique_ptr<ISurveyStrategy> m_strategy;
    Config m_cfg{};
  };

  static Builder builder() { return {}; }

private:
  /// If the modem cannot honour the strategy's control needs, fall back to
  /// passive monitoring instead of failing — same core, degraded actuation.
  void negotiate() {
    if (!caps_satisfy(m_control->caps(), m_strategy->required_caps())) {
      m_strategy = std::make_unique<PassiveMonitorStrategy>();
      m_downgraded = true;
    }
  }

  std::unique_ptr<RadioScannerEngine> m_engine;
  std::unique_ptr<IModemControl> m_control;
  std::unique_ptr<ISurveyStrategy> m_strategy;
  std::vector<ISurveySink*> m_sinks;
  SurveyResult m_last;
  std::set<uint64_t> m_seen_eci;
  bool m_downgraded{false};
};

}  // namespace QCom::Engine
