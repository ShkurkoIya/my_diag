#include <qcom/qmi/device/catalog.hpp>

#include <qcom/qmi/device/dossier_store.hpp>
#include <qcom/qmi/device/probe.hpp>

#include <unordered_set>

namespace QCom::Qmi::device {

DeviceCatalog::DeviceCatalog(ProfileRegistry profiles) : profiles_(std::move(profiles)) {}

void DeviceCatalog::set_callbacks(CatalogCallbacks cb) { callbacks_ = std::move(cb); }

void DeviceCatalog::set_dossier_path(std::filesystem::path path) {
  dossier_path_ = std::move(path);
}

void DeviceCatalog::emit_diff(const std::vector<ModemEndpoint>& previous) {
  std::unordered_set<std::string> prev_ids;
  prev_ids.reserve(previous.size());
  for (const auto& e : previous) {
    prev_ids.insert(e.id);
  }
  std::unordered_set<std::string> cur_ids;
  for (const auto& e : endpoints_) {
    cur_ids.insert(e.id);
    if (!prev_ids.count(e.id) && callbacks_.on_added) {
      callbacks_.on_added(e);
    }
  }
  for (const auto& id : prev_ids) {
    if (!cur_ids.count(id) && callbacks_.on_removed) {
      callbacks_.on_removed(id);
    }
  }
}

Result<std::vector<ModemEndpoint>> DeviceCatalog::refresh(EnumerateOptions opts) {
  auto previous = endpoints_;
  auto got = enumerate_sysfs(opts, profiles_);
  if (!got) {
    return got.error();
  }
  endpoints_ = std::move(got.value());
  emit_diff(previous);
  return endpoints_;
}

const ModemEndpoint* DeviceCatalog::find(std::string_view id) const noexcept {
  for (const auto& e : endpoints_) {
    if (e.id == id) {
      return &e;
    }
  }
  return nullptr;
}

std::vector<ModemReport> DeviceCatalog::list_reports() const {
  std::vector<ModemReport> out;
  out.reserve(endpoints_.size());
  for (const auto& e : endpoints_) {
    ModemReport r;
    r.endpoint = e;
    r.profile = profiles_.find_by_id(e.matched_profile_id);
    if (!r.profile && e.qmi_path()) {
      r.profile = &ProfileRegistry::generic_qmi_profile();
    }
    if (auto it = dossiers_.find(e.id); it != dossiers_.end()) {
      r.dossier = it->second;
    }
    out.push_back(std::move(r));
  }
  return out;
}

Result<ModemReport> DeviceCatalog::report(std::string_view id) const {
  const auto* ep = find(id);
  if (!ep) {
    return Error::from(Errc::InvalidArgument, "unknown endpoint id");
  }
  ModemReport r;
  r.endpoint = *ep;
  r.profile = profiles_.find_by_id(ep->matched_profile_id);
  if (!r.profile && ep->qmi_path()) {
    r.profile = &ProfileRegistry::generic_qmi_profile();
  }
  if (auto it = dossiers_.find(ep->id); it != dossiers_.end()) {
    r.dossier = it->second;
  }
  return r;
}

Result<ModemDossier> DeviceCatalog::probe(std::string_view id, ProbeOptions opts) {
  const auto* ep = find(id);
  if (!ep) {
    // allow probe after refresh miss: try refresh once? No — explicit.
    return Error::from(Errc::InvalidArgument, "unknown endpoint id — call refresh() first");
  }
  const ModemProfile* profile = profiles_.find_by_id(ep->matched_profile_id);
  if (!profile && ep->qmi_path()) {
    profile = &ProfileRegistry::generic_qmi_profile();
  }
  const ModemDossier* prev = nullptr;
  if (auto it = dossiers_.find(ep->id); it != dossiers_.end()) {
    prev = &it->second;
  }
  auto d = probe_endpoint(*ep, profile, opts, prev);
  if (!d) {
    return d.error();
  }
  dossiers_[ep->id] = d.value();
  if (!dossier_path_.empty()) {
    (void)save_dossiers();
  }
  if (callbacks_.on_updated) {
    if (auto r = report(id); r) {
      callbacks_.on_updated(r.value());
    }
  }
  return d.value();
}

Result<void> DeviceCatalog::load_dossiers() {
  if (dossier_path_.empty()) {
    return Error::from(Errc::InvalidArgument, "dossier path not set");
  }
  if (!std::filesystem::exists(dossier_path_)) {
    return Result<void>::success();
  }
  auto got = read_dossiers_file(dossier_path_);
  if (!got) {
    return got.error();
  }
  dossiers_ = std::move(got.value());
  return Result<void>::success();
}

Result<void> DeviceCatalog::save_dossiers() const {
  if (dossier_path_.empty()) {
    return Error::from(Errc::InvalidArgument, "dossier path not set");
  }
  return write_dossiers_file(dossier_path_, dossiers_);
}

Settings DeviceCatalog::to_qmi_settings(std::string_view id) const {
  const auto* ep = find(id);
  if (!ep) {
    return {};
  }
  const ModemProfile* profile = profiles_.find_by_id(ep->matched_profile_id);
  return ep->to_qmi_settings(profile);
}

}  // namespace QCom::Qmi::device
