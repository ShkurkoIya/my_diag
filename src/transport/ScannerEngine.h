#pragma once

#include <concepts>
#include <functional>
#include <memory>

#include "core/CellIdentity.h"
#include "core/CellTracker.h"
#include "transport/DataSourceInterface.h"
#include "core/QualcomParser.h"

namespace QCom {

template <typename T>
concept IsValidSource = std::derived_from<T, IDataSource>;

template <typename SourceType>
  requires IsValidSource<SourceType>
class RadioScannerEngine {
public:
  using CellUpdateCallback = std::function<void(const std::vector<CellIdentity>&)>;

  RadioScannerEngine()
      : m_parser(std::make_unique<QualcomParser>()), m_source(std::make_unique<SourceType>()) {
    m_source->set_frame_callback([this](QualcommPacketView pkt) { m_parser->on_packet(pkt); });
  }

  ~RadioScannerEngine() { stop(); }

  RadioScannerEngine(const RadioScannerEngine&) = delete;
  RadioScannerEngine& operator=(const RadioScannerEngine&) = delete;

  void set_callback(CellUpdateCallback cb) { m_parser->set_cell_callback(std::move(cb)); }
  bool start() { return m_source->start(); }
  void stop() { m_source->stop(); }

  [[nodiscard]] const CellTracker& tracker() const noexcept { return m_parser->tracker(); }

private:
  std::unique_ptr<QualcomParser> m_parser;
  std::unique_ptr<SourceType> m_source;
};

}  // namespace QCom
