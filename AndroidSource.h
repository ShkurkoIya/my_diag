#pragma once

#include <iostream>

#include "DataSourceInterface.h"

namespace QComScanner {
class AndroidSource : public IDataSource {
public:
  AndroidSource() = default;
  ~AndroidSource() override = default;

  void set_frame_callback(FrameCallback cb) override { m_callback = std::move(cb); }

  bool start() override {
    std::cout << "[AndroidSource] Симуляция: Подключение к локальному Unix сокету рут-демона...\n";
    return true;
  }

  void stop() override { std::cout << "[AndroidSource] Симуляция: Сессия сокета остановлена.\n"; }

private:
  FrameCallback m_callback;
};
}  // namespace QComScanner
