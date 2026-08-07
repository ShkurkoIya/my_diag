#include "qmi_observer/device/dossier_store.hpp"

#include "qmi_observer/health.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace qmi_observer::device {
namespace {

std::string json_escape(std::string_view s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': o += "\\\\"; break;
      case '"': o += "\\\""; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default: o += c; break;
    }
  }
  return o;
}

void write_opt_str(std::ostringstream& os, const char* key, const std::optional<std::string>& v,
                   bool& first) {
  if (!v) {
    return;
  }
  if (!first) {
    os << ',';
  }
  first = false;
  os << '"' << key << "\":\"" << json_escape(*v) << '"';
}

void write_str(std::ostringstream& os, const char* key, std::string_view v, bool& first) {
  if (!first) {
    os << ',';
  }
  first = false;
  os << '"' << key << "\":\"" << json_escape(v) << '"';
}

void write_bool(std::ostringstream& os, const char* key, bool v, bool& first) {
  if (!first) {
    os << ',';
  }
  first = false;
  os << '"' << key << "\":" << (v ? "true" : "false");
}

void write_i64(std::ostringstream& os, const char* key, int64_t v, bool& first) {
  if (!first) {
    os << ',';
  }
  first = false;
  os << '"' << key << "\":" << v;
}

// Extremely small JSON extractor for our dossier schema (not a general parser).
std::optional<std::string> extract_string(std::string_view obj, std::string_view key) {
  const std::string pat = "\"" + std::string{key} + "\":\"";
  auto pos = obj.find(pat);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  pos += pat.size();
  std::string out;
  for (size_t i = pos; i < obj.size(); ++i) {
    if (obj[i] == '\\' && i + 1 < obj.size()) {
      out.push_back(obj[i + 1]);
      ++i;
      continue;
    }
    if (obj[i] == '"') {
      break;
    }
    out.push_back(obj[i]);
  }
  return out;
}

std::optional<bool> extract_bool(std::string_view obj, std::string_view key) {
  const std::string pat = "\"" + std::string{key} + "\":";
  auto pos = obj.find(pat);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  pos += pat.size();
  if (obj.substr(pos, 4) == "true") {
    return true;
  }
  if (obj.substr(pos, 5) == "false") {
    return false;
  }
  return std::nullopt;
}

std::optional<int64_t> extract_i64(std::string_view obj, std::string_view key) {
  const std::string pat = "\"" + std::string{key} + "\":";
  auto pos = obj.find(pat);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  pos += pat.size();
  size_t end = pos;
  while (end < obj.size() && (std::isdigit(static_cast<unsigned char>(obj[end])) || obj[end] == '-')) {
    ++end;
  }
  if (end == pos) {
    return std::nullopt;
  }
  return std::stoll(std::string{obj.substr(pos, end - pos)});
}

ModemPhase phase_from_string(std::string_view s) {
  if (s == "camped") return ModemPhase::Camped;
  if (s == "searching") return ModemPhase::Searching;
  if (s == "online_idle") return ModemPhase::OnlineIdle;
  if (s == "offline_rf") return ModemPhase::OfflineRf;
  if (s == "fault") return ModemPhase::Fault;
  if (s == "absent") return ModemPhase::Absent;
  if (s == "unavailable") return ModemPhase::Unavailable;
  return ModemPhase::OnlineIdle;
}

}  // namespace

std::string serialize_dossiers(const std::unordered_map<std::string, ModemDossier>& map) {
  std::ostringstream os;
  os << "{\"version\":1,\"dossiers\":[";
  bool first_obj = true;
  for (const auto& [_, d] : map) {
    if (!first_obj) {
      os << ',';
    }
    first_obj = false;
    os << '{';
    bool first = true;
    write_str(os, "endpoint_id", d.endpoint_id, first);
    write_str(os, "matched_profile_id", d.matched_profile_id, first);
    write_opt_str(os, "qmi_path", d.qmi_path, first);
    if (!d.at_paths.empty()) {
      if (!first) {
        os << ',';
      }
      first = false;
      os << "\"at_paths\":[";
      for (size_t i = 0; i < d.at_paths.size(); ++i) {
        if (i) {
          os << ',';
        }
        os << '"' << json_escape(d.at_paths[i]) << '"';
      }
      os << ']';
    }
    write_bool(os, "qmi_open_ok", d.qmi_open_ok, first);
    write_bool(os, "at_ok", d.at_ok, first);
    write_opt_str(os, "dms_manufacturer", d.dms_manufacturer, first);
    write_opt_str(os, "dms_model", d.dms_model, first);
    write_opt_str(os, "dms_revision", d.dms_revision, first);
    write_opt_str(os, "at_identity", d.at_identity, first);
    if (d.last_phase) {
      write_str(os, "last_phase", to_string(*d.last_phase), first);
    }
    write_opt_str(os, "last_health_summary", d.last_health_summary, first);
    write_bool(os, "last_snapshot_ok", d.last_snapshot_ok, first);
    write_str(os, "deepest_probe", to_string(d.deepest_probe), first);
    write_i64(os, "probed_at_unix", d.probed_at_unix, first);
    if (!d.last_error.empty()) {
      write_str(os, "last_error", d.last_error, first);
    }
    os << '}';
  }
  os << "]}";
  return os.str();
}

Result<std::unordered_map<std::string, ModemDossier>> parse_dossiers(std::string_view json) {
  std::unordered_map<std::string, ModemDossier> map;
  // Split by "{\"endpoint_id\"" occurrences — good enough for our writer.
  size_t pos = 0;
  while (true) {
    auto start = json.find("{\"endpoint_id\"", pos);
    if (start == std::string_view::npos) {
      start = json.find("{\"endpoint_id\":", pos);
    }
    if (start == std::string_view::npos) {
      break;
    }
    auto end = json.find("}", start);
    if (end == std::string_view::npos) {
      break;
    }
    // extend to matching — dossiers may contain nested none; our objects are flat
    const auto obj = json.substr(start, end - start + 1);
    ModemDossier d;
    if (auto id = extract_string(obj, "endpoint_id")) {
      d.endpoint_id = *id;
    } else {
      pos = end + 1;
      continue;
    }
    if (auto v = extract_string(obj, "matched_profile_id")) {
      d.matched_profile_id = *v;
    }
    d.qmi_path = extract_string(obj, "qmi_path");
    if (auto v = extract_bool(obj, "qmi_open_ok")) {
      d.qmi_open_ok = *v;
    }
    if (auto v = extract_bool(obj, "at_ok")) {
      d.at_ok = *v;
    }
    d.dms_manufacturer = extract_string(obj, "dms_manufacturer");
    d.dms_model = extract_string(obj, "dms_model");
    d.dms_revision = extract_string(obj, "dms_revision");
    d.at_identity = extract_string(obj, "at_identity");
    if (auto ph = extract_string(obj, "last_phase")) {
      d.last_phase = phase_from_string(*ph);
    }
    d.last_health_summary = extract_string(obj, "last_health_summary");
    if (auto v = extract_bool(obj, "last_snapshot_ok")) {
      d.last_snapshot_ok = *v;
    }
    if (auto dp = extract_string(obj, "deepest_probe")) {
      if (*dp == "radio") d.deepest_probe = ProbeLevel::Radio;
      else if (*dp == "identity") d.deepest_probe = ProbeLevel::Identity;
      else if (*dp == "transport") d.deepest_probe = ProbeLevel::Transport;
      else d.deepest_probe = ProbeLevel::Presence;
    }
    if (auto t = extract_i64(obj, "probed_at_unix")) {
      d.probed_at_unix = *t;
    }
    if (auto e = extract_string(obj, "last_error")) {
      d.last_error = *e;
    }
    // at_paths: crude
    auto ap = obj.find("\"at_paths\":[");
    if (ap != std::string_view::npos) {
      ap = obj.find('[', ap);
      auto ae = obj.find(']', ap);
      if (ae != std::string_view::npos) {
        auto arr = obj.substr(ap + 1, ae - ap - 1);
        size_t i = 0;
        while (i < arr.size()) {
          auto q1 = arr.find('"', i);
          if (q1 == std::string_view::npos) {
            break;
          }
          auto q2 = arr.find('"', q1 + 1);
          if (q2 == std::string_view::npos) {
            break;
          }
          d.at_paths.emplace_back(arr.substr(q1 + 1, q2 - q1 - 1));
          i = q2 + 1;
        }
      }
    }
    map[d.endpoint_id] = std::move(d);
    pos = end + 1;
  }
  return map;
}

Result<void> write_dossiers_file(const std::filesystem::path& path,
                                 const std::unordered_map<std::string, ModemDossier>& map) {
  std::ofstream out(path);
  if (!out) {
    return Error::from(Errc::Internal, "cannot write dossier file: " + path.string());
  }
  out << serialize_dossiers(map);
  return Result<void>::success();
}

Result<std::unordered_map<std::string, ModemDossier>> read_dossiers_file(
    const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    return Error::from(Errc::Internal, "cannot read dossier file: " + path.string());
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return parse_dossiers(ss.str());
}

}  // namespace qmi_observer::device
