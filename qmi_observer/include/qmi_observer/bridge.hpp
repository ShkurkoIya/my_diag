#pragma once

/**
 * @file bridge.hpp
 * @brief Map QMI @ref CellSnapshot into @c QCom::Events for CellTracker.
 */

#include "qmi_observer/types.hpp"

#include "core/Events.h"

#include <vector>

namespace qmi_observer {

[[nodiscard]] QCom::RatType to_qcom_rat(Rat r) noexcept;

/**
 * @brief Convert one snapshot into tracker envelopes (passport + signal + serving).
 *
 * Physical key is @c LocalCellKey{rf_channel, phy_id}. Rows without RF+phy are skipped
 * unless cell_id alone can form a deferred passport (not done here — keep honest).
 */
[[nodiscard]] std::vector<QCom::Events::RrcEventEnvelope> to_rrc_envelopes(
    const CellSnapshot& snap, uint64_t timestamp = 0);

}  // namespace qmi_observer
