#include "qmi_observer/error.hpp"

namespace qmi_observer {

std::string_view to_string(Errc e) noexcept {
  switch (e) {
    case Errc::Ok: return "ok";
    case Errc::NotOpen: return "not open";
    case Errc::AlreadyOpen: return "already open";
    case Errc::DeviceOpenFailed: return "device open failed";
    case Errc::ClientAllocFailed: return "client alloc failed";
    case Errc::RequestFailed: return "request failed";
    case Errc::Unsupported: return "unsupported";
    case Errc::InvalidArgument: return "invalid argument";
    case Errc::Timeout: return "timeout";
    case Errc::NotReady: return "not ready";
    case Errc::WrongConfig: return "wrong config";
    case Errc::NoNetwork: return "no network";
    case Errc::NeedsRecover: return "needs recover";
    case Errc::Internal: return "internal error";
  }
  return "unknown error";
}

}  // namespace qmi_observer
