#pragma once

/**
 * @file nas_reader.hpp
 * @brief Read-plane: NAS cell location → @ref CellSnapshot (serving + neighbors).
 */

#include <qcom/qmi/error.hpp>
#include <qcom/qmi/types.hpp>

namespace QCom::Qmi {

class Session;

class NasReader {
 public:
  explicit NasReader(Session& session);

  /**
   * @brief NAS Get Cell Location Info → serving + intra/interfreq LTE (+ UMTS nbrs).
   *
   * @retval NoNetwork when modem is mid-search ( QuMI NoNetworkFound ).
   * @retval NotReady when session closed.
   */
  [[nodiscard]] Result<CellSnapshot> snapshot_cells();

  /**
   * @brief NAS Get Serving System + Get Signal Info (registration / PLMN / RSRP…).
   *
   * Best-effort: missing TLVs leave optionals empty. Does not require a cell list.
   */
  [[nodiscard]] Result<NasRadioStatus> snapshot_status();

 private:
  Session& session_;
};

}  // namespace QCom::Qmi
