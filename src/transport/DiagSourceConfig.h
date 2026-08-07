/// @file DiagSourceConfig.h
/// @brief Platform-neutral config for opening a DIAG data source.
#pragma once

#include <cstdint>
#include <string>

namespace QCom {

struct DiagSourceConfig {
  std::string device_path{"/dev/ttyUSB0"};  ///< Linux DIAG tty; unused on Android DCI today
  int baud_rate{921600};
  bool init_masks{true};  ///< Linux: run DiagSession::init_modem(); Android: set via libdiag
};

}  // namespace QCom
