#include "observer/Runtime.h"

#include "observer/SurveyProc.h"

namespace Observer {

int run_survey(Options opt) { return SurveyProc{std::move(opt)}.run(); }

}  // namespace Observer
