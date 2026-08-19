/// @file main.cpp
/// @brief Observer — RF-air survey process (the installable product).
///
/// Qualcomm DIAG/AT/QMI is the current backend (`QCom::*`). This binary is not
/// a Qualcomm library: later radios can plug in without renaming the app.
#include "observer/ModemSelect.h"
#include "observer/Options.h"
#include "observer/Runtime.h"

#include <utility>

int main(int argc, char** argv) {
  Observer::Options opt;
  const int st = Observer::parse_options(argc, argv, opt);
  if (st != 0) return st;
  if (opt.help_only) return 0;
  if (opt.list_only) return Observer::list_modems();
  return Observer::run_survey(std::move(opt));
}
