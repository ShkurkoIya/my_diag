/// @file JournalTxtReader.h
/// @brief Stream parser for dia_vldos observer_journal_*.txt (offline tool only).
#pragma once

#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <observer/model/Types.h>
#include <qcom/lte/LteRrcOta.h>

namespace QCom::Tools {

struct JournalEntry {
  uint64_t seq{0};
  LogCode log_code{0};
  std::vector<uint8_t> payload;
  // Optional metadata from journal summary (header already stripped in RAW)
  std::optional<uint32_t> earfcn;
  std::optional<uint16_t> pci;
  std::optional<uint8_t> pdu_num;
  std::string wall_time;  // `YYYY-MM-DD HH:MM:SS` from `[…]`
};

class JournalTxtReader {
public:
  explicit JournalTxtReader(std::string path) : m_path(std::move(path)) {}

  [[nodiscard]] bool open() {
    m_in.open(m_path);
    return m_in.is_open();
  }

  [[nodiscard]] std::optional<JournalEntry> next() {
    std::string line;
    std::optional<LogCode> pending_code;
    std::optional<uint32_t> pending_earfcn;
    std::optional<uint16_t> pending_pci;
    std::optional<uint8_t> pending_pdu;
    std::string pending_wall;

    while (std::getline(m_in, line)) {
      if (line.empty() || line[0] == '#') continue;

      if (line[0] == '[') {
        pending_code = parse_header_code(line);
        pending_earfcn = parse_u32_field(line, "earfcn=");
        if (!pending_earfcn) pending_earfcn = parse_u32_field(line, "EARFCN=");
        if (!pending_earfcn) pending_earfcn = parse_u32_field(line, "arfcn=");
        pending_pci = parse_u16_field(line, "pci=");
        if (!pending_pci) pending_pci = parse_u16_field(line, "PCI=");
        pending_pdu = parse_u8_field(line, "pdu=");
        pending_wall = parse_wall_time(line);
        continue;
      }

      constexpr std::string_view kRawPrefix = "RAW:";
      auto trimmed = ltrim(line);
      if (!trimmed.starts_with(kRawPrefix)) continue;

      auto hex = ltrim(trimmed.substr(kRawPrefix.size()));
      auto bytes = parse_hex(hex);
      if (!bytes || bytes->empty() || !pending_code) {
        ++m_errors;
        pending_code.reset();
        continue;
      }

      JournalEntry e;
      e.seq = ++m_seq;
      e.log_code = *pending_code;
      e.earfcn = pending_earfcn;
      e.pci = pending_pci;
      e.pdu_num = pending_pdu;
      e.wall_time = std::move(pending_wall);
      e.payload = std::move(*bytes);

      // dia_vldos journals strip the Qualcomm OTA header and keep ASN.1 only.
      // Text line carries earfcn/pci/pdu — trust that over byte heuristics:
      // ASN.1 often starts with a byte that collides with OTA version IDs.
      if (e.log_code == 0xB0C0) {
        const bool has_text_meta = e.pdu_num || e.earfcn || e.pci;
        const bool already_wrapped =
            !has_text_meta && Lte::decode_lte_rrc_ota(e.payload).has_value();
        if (!already_wrapped) {
          ChannelType ch = ChannelType::UNKNOWN;
          // Text journals omit OTA version; 0x19 (v25-style) matches modern QC maps.
          if (e.pdu_num) ch = Lte::pdu_num_to_channel(/*version=*/0x19, *e.pdu_num);
          uint32_t earfcn = e.earfcn.value_or(0);
          uint16_t pci = e.pci.value_or(0);
          e.payload = Lte::synthesize_ota_header(earfcn, pci, ch, e.payload);
        }
      }

      // Text arfcn= on EVENT lines as fallback if RAW parse misses it.
      if (e.log_code == 0x0000 && e.earfcn && *e.earfcn > 0 && *e.earfcn <= 1023) {
        // Stash in pci unused? Better leave for QualcomParser binary path.
        // Also expose via earfcn field — main can set hint.
      }

      pending_code.reset();
      pending_earfcn.reset();
      pending_pci.reset();
      pending_pdu.reset();
      pending_wall.clear();
      ++m_entries;
      return e;
    }
    return std::nullopt;
  }

  [[nodiscard]] uint64_t entries() const noexcept { return m_entries; }
  [[nodiscard]] uint64_t errors() const noexcept { return m_errors; }

private:
  std::string m_path;
  std::ifstream m_in;
  uint64_t m_seq{0};
  uint64_t m_entries{0};
  uint64_t m_errors{0};

  static std::string_view ltrim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    return s;
  }

  static std::optional<LogCode> parse_header_code(std::string_view line) {
    auto pos = line.find("0x");
    if (pos == std::string_view::npos) pos = line.find("0X");
    if (pos == std::string_view::npos) return std::nullopt;

    size_t end = pos + 2;
    while (end < line.size()) {
      char c = line[end];
      bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
      if (!hex) break;
      ++end;
    }
    if (end <= pos + 2) return std::nullopt;

    unsigned long v = std::stoul(std::string(line.substr(pos + 2, end - pos - 2)), nullptr, 16);
    return static_cast<LogCode>(v & 0xFFFF);
  }

  static std::optional<uint32_t> parse_u32_field(std::string_view line, std::string_view key) {
    auto pos = line.find(key);
    if (pos == std::string_view::npos) return std::nullopt;
    pos += key.size();
    size_t end = pos;
    while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) ++end;
    if (end == pos) return std::nullopt;
    try {
      return static_cast<uint32_t>(std::stoul(std::string(line.substr(pos, end - pos))));
    } catch (...) {
      return std::nullopt;
    }
  }

  static std::optional<uint16_t> parse_u16_field(std::string_view line, std::string_view key) {
    auto v = parse_u32_field(line, key);
    if (!v) return std::nullopt;
    return static_cast<uint16_t>(*v);
  }

  static std::optional<uint8_t> parse_u8_field(std::string_view line, std::string_view key) {
    auto v = parse_u32_field(line, key);
    if (!v) return std::nullopt;
    return static_cast<uint8_t>(*v);
  }

  static std::string parse_wall_time(std::string_view line) {
    // `[2026-07-29 16:56:36] …`
    if (line.size() < 21 || line[0] != '[') return {};
    auto close = line.find(']');
    if (close == std::string_view::npos || close < 20) return {};
    return std::string(line.substr(1, close - 1));
  }

  static std::optional<std::vector<uint8_t>> parse_hex(std::string_view hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);

    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };

    for (size_t i = 0; i < hex.size();) {
      if (std::isspace(static_cast<unsigned char>(hex[i]))) {
        ++i;
        continue;
      }
      if (i + 1 >= hex.size()) return std::nullopt;
      int hi = nibble(hex[i]);
      int lo = nibble(hex[i + 1]);
      if (hi < 0 || lo < 0) return std::nullopt;
      out.push_back(static_cast<uint8_t>((hi << 4) | lo));
      i += 2;
    }
    return out;
  }
};

}  // namespace QCom::Tools
