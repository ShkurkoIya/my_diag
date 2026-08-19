#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <observer/model/CellIdentity.h>
#include <observer/model/CellTracker.h>
#include <observer/model/ParserInterface.h>
#include <observer/model/Utils.h>

namespace QCom {

namespace Lte {
/// B0C0 mapped-channel parses that produced no ASN.1 events (unpack fail / unhandled).
[[nodiscard]] uint64_t lte_rrc_ota_asn1_empty_count() noexcept;
}  // namespace Lte

struct LogCodeStats {
  uint64_t seen{0};
  uint64_t with_events{0};  ///< parse OK and produced ≥1 tracker event
  uint64_t empty{0};        ///< parse OK but no events (unsupported PDU / fail-closed)
  uint64_t error{0};        ///< parse returned ParserError
};

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
  std::expected<void, ParserError> on_diag_frame(std::span<const uint8_t> raw_frame);

  // Direct entry: pre-parsed packet (for testing or when header is already stripped)
  std::expected<void, ParserError> on_packet(QualcommPacketView pkt);

  [[nodiscard]] const CellTracker& tracker() const noexcept { return m_tracker; }
  [[nodiscard]] CellTracker& tracker() noexcept { return m_tracker; }

  [[nodiscard]] const std::unordered_map<LogCode, LogCodeStats>& code_stats() const noexcept {
    return m_code_stats;
  }
  [[nodiscard]] bool is_supported(LogCode code) const noexcept {
    return m_dispatch.contains(code);
  }
  [[nodiscard]] std::string_view code_name(LogCode code) const noexcept;

private:
  std::unordered_map<LogCode, std::shared_ptr<IRatParser>> m_dispatch;
  std::unordered_map<LogCode, LogCodeStats> m_code_stats;
  CellTracker m_tracker;
  CellCallback m_cell_cb;

  [[nodiscard]] RatType classify_rat(LogCode code) noexcept;
  [[nodiscard]] LocalCellKey extract_cell_key(const QualcommPacketView& pkt, RatType rat) noexcept;
  void handle_diag_event(std::span<const uint8_t> buf);

  Utils::LtePciPackOrder m_pci_pack;
};

}  // namespace QCom
