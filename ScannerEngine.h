#pragma once

#include <concepts>

#include "CellIdentity.h"
#include "CellTracker.h"
#include "DataSourceInterface.h"
#include "QualcomParser.h"

namespace QComScanner {
template <typename T>
concept IsValidSource = std::derived_from<T, IDataSource>;

template <typename SourceType>
  requires IsValidSource<SourceType>
class RadioScannerEngine {
public:
  using GlobalUpdateCallback = std::function<void(const std::vector<CellIdentity>&)>;

  using LteCellCallback = std::function<void(const CellIdentity&)>;
  using NrCellCallback = std::function<void(const CellIdentity&)>;
  using GsmCellCallback = std::function<void(const CellIdentity&)>;
  using WcdmaCellCallback = std::function<void(const CellIdentity&)>;

  RadioScannerEngine()
      : m_tracker(std::make_unique<QComParser::CellTracker>())
      : m_parser(std::make_unique < QComParser::QualcomParser())
      : m_source(std::make_unique<SourceType>) {
    m_source->set_frame_callback([this](QualcommPacketView pkt)) {
      this->process_incoming_packet(pkt);
    });
  }
  ~RadioScannerEngine() { stop(); }

  RadioScannerEngine(const RadioScannerEngine&) = delete;
  RadioScannerEngine& operator=(const RadioScannerEngine) = delete;

private:
  std::unique_ptr<QComParser::CellTracker> m_tracker;
  std::unique_ptr<QComParser::QualcomParser> m_parser;
  std::unique_ptr<SourceType> m_source;

  GlobalUpdateCallback m_global_cb;
  LteCellCallback m_lte_cb;
  NrCellCallback m_nr_cb;
  GsmCellCallback m_gsm_cb;
  WcdmaCellCallback m_wcdma_cb;
};
}  // namespace QComScanner
