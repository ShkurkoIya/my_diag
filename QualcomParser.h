#pragma once

#include <expected>
#include <functional>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "CellIdentity.h"
#include "LogCodes.h"
#include "ParserInterface.h"

namespace QCommParser {
class QualcommParser {
public:
  using CellCallback = std::function<void(const std::vector<CellIdentity>&)>;

  QualcommParser(std::vector<std::shared_ptr<IRatParser>> initial_modules = {});
  ~QualcommParser() = default;

  QualcommParser(const QualcommParser&) = delete;
  QualcommParser& operator=(const QualcommParser&) = delete;

  void register_parser_module(std::shared_ptr<IRatParser> parser_module);

  void set_monitor_callback(CellCallback cb) { m_monitor_cb = std::move(cb); }

  std::expected<void, ParserError> on_log_packet(std::string_view raw_frame);

private:
  std::unordered_map<LogCode, std::shared_ptr<IRatParser>> m_parsers;
  CellCallback m_monitor_cb;

  void emit_update();
};
}  // namespace QCommParser
