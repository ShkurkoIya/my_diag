/// @file BinaryCursor.h
/// @brief Bounds-aware LE view over a byte span — shared binary-parse API.
///
/// Prefer this over raw `Converter::read_le(ptr, off)` in new code: offsets stay
/// relative to a window (`at` / `slice`), and OOB reads return zero (fail-closed
/// callers still validate semantically).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <observer/model/Utils.h>

namespace QCom::Utils {

class BinaryCursor {
public:
  constexpr BinaryCursor() noexcept = default;
  explicit constexpr BinaryCursor(std::span<const uint8_t> data) noexcept : data_(data) {}
  BinaryCursor(const uint8_t* p, size_t n) noexcept : data_(p, n) {}

  [[nodiscard]] constexpr size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] constexpr bool empty() const noexcept { return data_.empty(); }
  [[nodiscard]] constexpr const uint8_t* data() const noexcept { return data_.data(); }
  [[nodiscard]] constexpr std::span<const uint8_t> span() const noexcept { return data_; }

  [[nodiscard]] constexpr bool has(size_t off, size_t nbytes) const noexcept {
    return nbytes <= data_.size() && off <= data_.size() - nbytes;
  }

  /// Window starting at `off` (empty if out of range).
  [[nodiscard]] constexpr BinaryCursor at(size_t off) const noexcept {
    if (off > data_.size()) return {};
    return BinaryCursor{data_.subspan(off)};
  }

  /// Fixed-length window `[off, off+n)` (empty if out of range).
  [[nodiscard]] constexpr BinaryCursor slice(size_t off, size_t n) const noexcept {
    if (!has(off, n)) return {};
    return BinaryCursor{data_.subspan(off, n)};
  }

  [[nodiscard]] uint8_t u8(size_t off) const noexcept {
    return has(off, 1) ? data_[off] : uint8_t{0};
  }

  [[nodiscard]] uint16_t le16(size_t off) const noexcept {
    return Converter::read_le<uint16_t>(data_, off);
  }

  [[nodiscard]] uint32_t le32(size_t off) const noexcept {
    return Converter::read_le<uint32_t>(data_, off);
  }

  /// Bitfield inside a LE u16 at `off` (LSB-first within the word).
  [[nodiscard]] uint16_t le16_bits(size_t off, unsigned lsb, unsigned nbits) const noexcept {
    return static_cast<uint16_t>(bits(le16(off), lsb, nbits));
  }

  /// Bitfield inside a LE u32 at `off` (LSB-first within the word).
  [[nodiscard]] uint32_t le32_bits(size_t off, unsigned lsb, unsigned nbits) const noexcept {
    return bits(le32(off), lsb, nbits);
  }

private:
  std::span<const uint8_t> data_{};
};

}  // namespace QCom::Utils
