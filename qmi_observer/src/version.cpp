#include "qmi_observer/version.hpp"

#include <libqmi-glib.h>

#include <sstream>

namespace qmi_observer {

std::string_view version() noexcept { return kVersion; }

std::string libqmi_version_string() {
  std::ostringstream oss;
  oss << QMI_MAJOR_VERSION << '.' << QMI_MINOR_VERSION << '.' << QMI_MICRO_VERSION;
  return oss.str();
}

}  // namespace qmi_observer
