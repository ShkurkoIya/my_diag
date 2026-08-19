#pragma once

#include "observer/SurveyCtx.h"

#include <thread>

namespace Observer {

/// Discover / Complete / Rediscover / Wcdma grind. Same policy as the old hop lambda.
class SurveyHop {
public:
  explicit SurveyHop(SurveyCtx& ctx) : ctx_(ctx) {}
  SurveyHop(const SurveyHop&) = delete;
  SurveyHop& operator=(const SurveyHop&) = delete;
  ~SurveyHop() { join(); }

  void start();
  void join();

private:
  void run();

  SurveyCtx& ctx_;
  std::thread th_;
};

}  // namespace Observer
