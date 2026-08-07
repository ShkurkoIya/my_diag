#include "qmi_observer/types.hpp"

#include <unordered_set>

namespace qmi_observer {
namespace {

std::string opt_u(const std::optional<uint32_t>& v) {
  return v ? std::to_string(*v) : "-";
}
std::string opt_u64(const std::optional<uint64_t>& v) {
  return v ? std::to_string(*v) : "-";
}
std::string opt_u16(const std::optional<uint16_t>& v) {
  return v ? std::to_string(*v) : "-";
}

}  // namespace

std::string cell_merge_key(const CellObservation& c) {
  return std::string{to_string(c.rat)} + '|' + opt_u(c.rf_channel) + '|' + opt_u16(c.phy_id) +
         '|' + opt_u64(c.cell_id);
}

void merge_snapshot(AggregatedCells& dst, const CellSnapshot& src) {
  std::unordered_set<std::string> seen;
  seen.reserve(dst.cells.size() + src.cells.size());
  for (const auto& c : dst.cells) {
    seen.insert(cell_merge_key(c));
  }
  for (const auto& c : src.cells) {
    if (seen.insert(cell_merge_key(c)).second) {
      dst.cells.push_back(c);
    }
  }
}

}  // namespace qmi_observer
