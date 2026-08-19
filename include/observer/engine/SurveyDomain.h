/// @file SurveyDomain.h
/// @brief Task-oriented RF-survey domain types (what the app actually wants).
///
/// The parser/tracker speak in physical `CellIdentity` rows keyed by {EARFCN,PCI}.
/// Applications doing an RF survey think in higher-level terms: identified
/// *towers*, *operators*, *sites* (eNB groups) and honest *stats*. These types
/// are the currency of the survey facade (SurveySession) and are shared by every
/// output sink (JSON, GUI feed, console) so they all agree on the same domain.
///
/// Pure data, zero I/O. LTE-first (the current survey focus); other RATs can grow
/// their own domain views later without touching this one.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace QCom::Engine {

/// Live survey walk phase (JSON `survey_phase` + dashboard PHASE lines).
enum class SurveyPhase : uint8_t { Init = 0, Discover, Complete, Rediscover, Wcdma };

[[nodiscard]] constexpr std::string_view to_string(SurveyPhase p) noexcept {
  switch (p) {
    case SurveyPhase::Init:
      return "init";
    case SurveyPhase::Discover:
      return "discover";
    case SurveyPhase::Complete:
      return "complete";
    case SurveyPhase::Rediscover:
      return "rediscover";
    case SurveyPhase::Wcdma:
      return "wcdma";
  }
  return "init";
}

/// A FULLY identified LTE cell: PLMN + TAC + ECI on a physical EARFCN|PCI.
/// This is a "tower" in survey terms — a real, mappable carrier.
struct Tower {
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint8_t mnc_digits{0};  ///< 2 or 3 (for correct MNC string formatting)
  uint32_t tac{0};
  uint64_t eci{0};  ///< 28-bit E-UTRAN Cell Identity
  uint32_t earfcn{0};
  uint16_t pci{0};
  uint8_t band{0};
  float rsrp_dbm{0.0f};
  bool has_rsrp{false};
  bool serving{false};  ///< modem is camped here right now
  bool ever_serving{false};

  /// eNB / site id — the top 20 bits of the ECI (sector = low 8 bits).
  [[nodiscard]] constexpr uint32_t enb_id() const noexcept {
    return static_cast<uint32_t>(eci >> 8);
  }
  [[nodiscard]] constexpr uint8_t sector() const noexcept {
    return static_cast<uint8_t>(eci & 0xFF);
  }
};

/// One PLMN seen in the survey, with how much of it we mapped.
struct Operator {
  uint16_t mcc{0};
  uint16_t mnc{0};
  uint8_t mnc_digits{0};
  std::size_t towers{0};  ///< FULL carriers attributed to this PLMN
  std::size_t sites{0};   ///< distinct eNB ids for this PLMN
};

/// Honest survey counters — the numbers the app displays / logs.
/// Deliberately distinguishes "RF detections" from "identified towers" so the
/// UI never claims N towers when most are PCI-only RADIO rows.
struct SurveyStats {
  std::size_t lte_rf_unique{0};  ///< distinct EARFCN|PCI with plausible RF
  std::size_t lte_full{0};       ///< FULL identified carriers (== towers.size())
  std::size_t lte_sites{0};      ///< distinct eNB ids among FULL carriers
  std::size_t lte_serving{0};    ///< currently-serving LTE cells (0 or 1 normally)
};

/// Everything the facade projects from a tracker snapshot in one shot.
struct SurveyResult {
  std::vector<Tower> towers;
  std::vector<Operator> operators;
  SurveyStats stats;
};

}  // namespace QCom::Engine
