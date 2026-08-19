#pragma once

/**
 * @file error.hpp
 * @brief Error vocabulary for qmi_observer (no libqmi types leak here).
 */

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace QCom::Qmi {

/**
 * @brief Stable error codes for scanner / GUI branching.
 *
 * Prefer matching on @ref Errc rather than parsing @ref Error::message.
 */
enum class Errc : int {
  Ok = 0,
  NotOpen,          ///< Session not open / clients missing.
  AlreadyOpen,
  DeviceOpenFailed,
  ClientAllocFailed,
  RequestFailed,
  Unsupported,
  InvalidArgument,
  Timeout,
  NotReady,         ///< Health gate rejected the operation.
  WrongConfig,      ///< Mode preference / settings mismatch.
  NoNetwork,        ///< QMI NoNetworkFound (searching / no serving).
  NeedsRecover,     ///< Offline / stuck RF — run recover path.
  Internal,
};

[[nodiscard]] std::string_view to_string(Errc e) noexcept;

struct Error {
  Errc code{Errc::Internal};
  std::string message;

  [[nodiscard]] static Error from(Errc code, std::string message = {}) {
    if (message.empty()) {
      message = std::string{to_string(code)};
    }
    return Error{code, std::move(message)};
  }
};

template <typename T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}
  Result(Error error) : storage_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] T& value() & { return std::get<T>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }

  [[nodiscard]] const Error& error() const { return std::get<Error>(storage_); }

 private:
  std::variant<T, Error> storage_;
};

template <>
class Result<void> {
 public:
  Result() : error_(std::nullopt) {}
  Result(Error error) : error_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Error& error() const { return *error_; }

  static Result success() { return Result{}; }

 private:
  std::optional<Error> error_;
};

}  // namespace QCom::Qmi
