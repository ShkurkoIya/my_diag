#pragma once

#include "observer/AtBus.h"
#include "observer/Dashboard.h"
#include "observer/LiveJson.h"
#include "observer/Options.h"
#include "observer/SurveyCtx.h"
#include "observer/SurveyHop.h"

#include <qcom/io/ScannerEngine.h>
#include <qcom/linux/SimcomAtControl.h>
#include <qcom/qmi/Qmi.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <thread>

namespace Observer {

/// Process supervisor: open ports, start I/O feeds, pump until stop, restore, report.
/// Policy (which cell to lock) lives in SurveyHop; this class does not grind.
class SurveyProc {
public:
  explicit SurveyProc(Options opt);
  SurveyProc(const SurveyProc&) = delete;
  SurveyProc& operator=(const SurveyProc&) = delete;
  ~SurveyProc();

  [[nodiscard]] int run();

private:
  [[nodiscard]] int boot();
  void pin_radio();
  void start_feeds();
  void set_banner();
  void pump();
  [[nodiscard]] bool reconnect_diag();
  void stop_feeds();
  void restore_radio();
  void report();

  void rat_guard_loop();
  void qmi_loop();
  void search_loop();
  void serving_loop();

  SurveyCtx ctx_;
  std::unique_ptr<QCom::RadioScannerEngine> engine_;
  LiveDashboard dash_;
  std::unique_ptr<LiveJson> live_;
  std::unique_ptr<AtBus> at_;
  std::optional<QCom::Engine::SimcomAtControl> simcom_;
  std::unique_ptr<QCom::Qmi::Session> qmi_;
  std::optional<SurveyHop> hop_;

  std::thread rat_guard_th_;
  std::thread qmi_th_;
  std::thread search_th_;
  std::thread serving_th_;
  std::chrono::steady_clock::time_point last_reconnect_{};
  bool stopped_{false};
};

}  // namespace Observer
