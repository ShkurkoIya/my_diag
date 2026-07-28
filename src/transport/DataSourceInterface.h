#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "core/ParserInterface.h"

namespace QCom {

class IDataSource {
public:
  using FrameCallback = std::function<void(QualcommPacketView pkt)>;

  virtual ~IDataSource() = default;

  virtual void set_frame_callback(FrameCallback cb) = 0;
  virtual bool start() = 0;
  virtual void stop() = 0;
};

}  // namespace QCom
