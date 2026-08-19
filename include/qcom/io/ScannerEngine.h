/// @file ScannerEngine.h
/// @brief Qualcomm ingest glue: IDataSource → QualcomParser → CellTracker.
///
/// Lives in qcom, not observer: the survey engine is portable; this wiring is DIAG.
#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <observer/model/CellIdentity.h>
#include <observer/model/CellTracker.h>
#include <observer/model/Events.h>
#include <qcom/parser/QualcomParser.h>
#include <observer/io/DataSourceInterface.h>

namespace QCom {

class RadioScannerEngine {
public:
  using CellUpdateCallback = std::function<void(const std::vector<CellIdentity>&)>;
  using PacketObserver = std::function<void(QualcommPacketView pkt)>;

  explicit RadioScannerEngine(std::unique_ptr<IDataSource> source)
      : m_parser(std::make_unique<QualcomParser>()), m_source(std::move(source)) {
    wire_source();
  }

  ~RadioScannerEngine() { stop(); }

  RadioScannerEngine(const RadioScannerEngine&) = delete;
  RadioScannerEngine& operator=(const RadioScannerEngine&) = delete;

  void set_callback(CellUpdateCallback cb) {
    m_cell_cb = cb;
    m_parser->set_cell_callback(std::move(cb));
  }

  /// Optional tap before parse (log-code histograms, mirrors, etc.).
  void set_packet_observer(PacketObserver ob) { m_packet_observer = std::move(ob); }

  void set_disconnect_callback(IDataSource::DisconnectCallback cb) {
    if (m_source) m_source->set_disconnect_callback(std::move(cb));
  }

  [[nodiscard]] bool start() { return m_source && m_source->start(); }
  void stop() {
    if (m_source) m_source->stop();
  }

  /// Feed packets from another producer (offline / tests).
  void inject_packet(QualcommPacketView pkt) { deliver(pkt); }

  /// Feed pre-built tracker events (QMI NAS → @c to_rrc_envelopes).
  void inject_envelopes(std::vector<Events::RrcEventEnvelope> envs) {
    if (envs.empty()) return;
    for (auto& e : envs) m_parser->tracker().handle_rrc_event(std::move(e));
    if (m_cell_cb) m_cell_cb(m_parser->tracker().get_snapshot());
  }

  [[nodiscard]] IDataSource* source() noexcept { return m_source.get(); }
  [[nodiscard]] const IDataSource* source() const noexcept { return m_source.get(); }

  [[nodiscard]] QualcomParser& parser() noexcept { return *m_parser; }
  [[nodiscard]] const QualcomParser& parser() const noexcept { return *m_parser; }

  [[nodiscard]] const CellTracker& tracker() const noexcept { return m_parser->tracker(); }
  [[nodiscard]] CellTracker& tracker() noexcept { return m_parser->tracker(); }

private:
  std::unique_ptr<QualcomParser> m_parser;
  std::unique_ptr<IDataSource> m_source;
  PacketObserver m_packet_observer;
  CellUpdateCallback m_cell_cb;

  void wire_source() {
    if (!m_source) return;
    m_source->set_frame_callback([this](QualcommPacketView pkt) { deliver(pkt); });
  }

  void deliver(QualcommPacketView pkt) {
    if (m_packet_observer) m_packet_observer(pkt);
    (void)m_parser->on_packet(pkt);
  }
};

}  // namespace QCom
