/// @file Runtime.h
/// @brief Survey process: DIAG + AT + QMI workers, hop walk, live JSON.
#pragma once

#include "observer/Options.h"

namespace Observer {

[[nodiscard]] int run_survey(Options opt);

}  // namespace Observer
