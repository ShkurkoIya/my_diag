#pragma once

#include <fmt/format.h>
#include <string>
#include <string_view>
#include <utility>

namespace Observer {

/// Runtime format — GCC 16 consteval would otherwise promote lambdas to immediate functions.
template <typename... Args>
std::string sfmt(std::string_view fs, Args&&... args) {
  return fmt::format(fmt::runtime(fs), std::forward<Args>(args)...);
}

}  // namespace Observer
