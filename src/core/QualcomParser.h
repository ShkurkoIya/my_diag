#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/CellIdentity.h"
#include "core/CellTracker.h"
#include "core/ParserInterface.h"

namespace QCom {

// Top-level Qualcomm DIAG packet router.
// Receives raw DIAG frames, extracts LogCode, dispatches to the correct
// RAT parser, and forwards resulting events to CellTracker.
class QualcomParser {
public:
  using CellCallback = std::function<void(const std::vector<CellIdentity>&)>;

  QualcomParser();
  ~QualcomParser() = default;

  QualcomParser(const QualcomParser&) = delete;
  QualcomParser& operator=(const QualcomParser&) = delete;

  void register_parser(std::shared_ptr<IRatParser> parser);
  void set_cell_callback(CellCallback cb) { m_cell_cb = std::move(cb); }

  // Main entry: raw DIAG frame with 14-byte header
  std::expected<void, ParserError> on_diag_frame(std::string_view raw_frame);

  // Direct entry: pre-parsed packet (for testing or when header is already stripped)
  std::expected<void, ParserError> on_packet(QualcommPacketView pkt);

  [[nodiscard]] const CellTracker& tracker() const noexcept { return m_tracker; }

private:
  std::unordered_map<LogCode, std::shared_ptr<IRatParser>> m_dispatch;
  CellTracker m_tracker;
  CellCallback m_cell_cb;

  [[nodiscard]] static RatType classify_rat(LogCode code) noexcept;
  [[nodiscard]] static LocalCellKey extract_cell_key(const QualcommPacketView& pkt,
                                                     RatType rat) noexcept;
};

}  // namespace QCom
