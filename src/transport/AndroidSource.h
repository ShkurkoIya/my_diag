#pragma once

#include "transport/DataSourceInterface.h"

namespace QCom {

// Android DCI data source stub.
// In production: dlopen(libdiag.so), register DCI client, set log masks.
// See dia_vldos/core/DiagDciClient for reference implementation.
class AndroidSource : public IDataSource {
public:
  AndroidSource() = default;
  ~AndroidSource() override = default;

  void set_frame_callback(FrameCallback cb) override { m_callback = std::move(cb); }
  bool start() override { return false; }
  void stop() override {}

private:
  FrameCallback m_callback;
};

}  // namespace QCom
