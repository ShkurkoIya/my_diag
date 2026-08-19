#include <qcom/qmi/device/probe.hpp>

#include <qcom/qmi/nas_reader.hpp>
#include <qcom/qmi/session.hpp>

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>

namespace QCom::Qmi::device {

Result<std::string> probe_at_port(const std::string& path, std::chrono::milliseconds timeout) {
  const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
  if (fd < 0) {
    return Error::from(Errc::RequestFailed, "AT open failed: " + path);
  }

  termios tio{};
  if (tcgetattr(fd, &tio) != 0) {
    ::close(fd);
    return Error::from(Errc::RequestFailed, "tcgetattr failed: " + path);
  }
  cfmakeraw(&tio);
  cfsetispeed(&tio, B115200);
  cfsetospeed(&tio, B115200);
  tio.c_cflag |= (CLOCAL | CREAD);
  tio.c_cc[VMIN] = 0;
  tio.c_cc[VTIME] = 0;
  if (tcsetattr(fd, TCSANOW, &tio) != 0) {
    ::close(fd);
    return Error::from(Errc::RequestFailed, "tcsetattr failed: " + path);
  }
  tcflush(fd, TCIOFLUSH);

  const char* cmd = "AT\r";
  if (::write(fd, cmd, std::strlen(cmd)) < 0) {
    ::close(fd);
    return Error::from(Errc::RequestFailed, "AT write failed");
  }

  std::string buf;
  buf.reserve(256);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const int pr = ::poll(&pfd, 1, static_cast<int>(std::max(left.count(), 1L)));
    if (pr <= 0) {
      continue;
    }
    char tmp[128];
    const ssize_t n = ::read(fd, tmp, sizeof(tmp));
    if (n > 0) {
      buf.append(tmp, static_cast<size_t>(n));
      if (buf.find("OK") != std::string::npos) {
        ::close(fd);
        return buf;
      }
      if (buf.find("ERROR") != std::string::npos) {
        ::close(fd);
        return Error::from(Errc::RequestFailed, "AT ERROR: " + buf);
      }
    }
  }
  ::close(fd);
  if (buf.empty()) {
    return Error::from(Errc::Timeout, "AT timeout: " + path);
  }
  return Error::from(Errc::Timeout, "AT no OK: " + buf);
}

Result<ModemDossier> probe_endpoint(const ModemEndpoint& ep, const ModemProfile* profile,
                                    const ProbeOptions& opts, const ModemDossier* previous) {
  ModemDossier d;
  if (previous) {
    d = *previous;
  }
  d.endpoint_id = ep.id;
  d.matched_profile_id = ep.matched_profile_id;
  d.qmi_path = ep.qmi_path();
  d.at_paths = ep.paths_with_role(PortRole::At);
  d.probed_at_unix = static_cast<int64_t>(std::time(nullptr));
  d.deepest_probe = ProbeLevel::Presence;
  d.last_error.clear();

  if (opts.level == ProbeLevel::Presence) {
    return d;
  }

  // Transport / Identity via QMI Session
  if (d.qmi_path) {
    Settings s = ep.to_qmi_settings(profile);
    s.device_path = *d.qmi_path;
    s.use_proxy = opts.use_proxy;
    s.request_timeout = opts.qmi_timeout;

    Session session(s);
    if (auto opened = session.open(); !opened) {
      d.qmi_open_ok = false;
      d.last_error = opened.error().message;
      return d;  // partial dossier still useful
    }
    d.qmi_open_ok = true;
    d.deepest_probe = ProbeLevel::Transport;

    if (opts.level >= ProbeLevel::Identity) {
      if (auto id = session.identity(); id) {
        d.dms_manufacturer = id.value().manufacturer;
        d.dms_model = id.value().model;
        d.dms_revision = id.value().revision;
        d.deepest_probe = ProbeLevel::Identity;
      } else {
        d.last_error = id.error().message;
      }
    }

    if (opts.level >= ProbeLevel::Radio) {
      if (auto h = session.refresh_health(); h) {
        d.last_phase = h.value().phase;
        d.last_health_summary = h.value().summary;
        d.deepest_probe = ProbeLevel::Radio;
      }
      if (auto snap = session.nas().snapshot_cells(); snap) {
        d.last_snapshot_ok = !snap.value().cells.empty();
      } else if (snap.error().code == Errc::NoNetwork) {
        d.last_snapshot_ok = false;
      } else {
        d.last_error = snap.error().message;
      }
    }
    session.close();
  } else {
    d.qmi_open_ok = false;
    d.last_error = "no QMI path";
  }

  if (opts.probe_at && opts.level >= ProbeLevel::Transport) {
    const bool allow_at = !profile || profile->quirks.at_probe_safe;
    if (allow_at) {
      if (auto at = ep.preferred_at_path()) {
        if (auto r = probe_at_port(*at, opts.at_timeout); r) {
          d.at_ok = true;
          d.at_identity = r.value();
          if (d.at_paths.empty()) {
            d.at_paths.push_back(*at);
          }
        } else {
          d.at_ok = false;
          if (d.last_error.empty()) {
            d.last_error = r.error().message;
          }
        }
      }
    }
  }

  return d;
}

}  // namespace QCom::Qmi::device
