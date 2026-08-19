# Architecture

Observer is the product: a cross-platform RF-air survey. Qualcomm DIAG/AT/QMI is
the current backend, not the domain model. Other radios plug in at the same
ports (`IDataSource`, `IModemControl`) without renaming the app.

## Layers

```
Observer process (app/)          live_scanner binary, GUI client of live JSON
        │
observer::engine                 survey session, strategy, honest FULL counts
observer::model                  CellIdentity / tracker / 3GPP bands
        │
qcom::parser / qcom::protocol    DIAG log-codes, HDLC
qcom::io                         Linux tty, Android stub, SIMCOM AT, DIAG ingest glue
qcom::qmi                        QMI NAS (`<qcom/qmi/...>`, namespace `QCom::Qmi`)
```

## Public headers

| Client sees | Prefix | Whose domain |
|-------------|--------|----------------|
| Cells, tracker, bands, hop planner | `<observer/model/...>`, `<observer/lte/LteHopPlanner.h>` | 3GPP / RF |
| Survey facade + control port | `<observer/engine/Survey.h>` | Observer |
| Byte source contract | `<observer/io/DataSourceInterface.h>` | Observer (packet currency is still `QualcommPacketView`) |
| DIAG ingest glue | `<qcom/io/ScannerEngine.h>` | Qualcomm |
| Log-code parsers, 0xB0C0 wrapper | `<qcom/parser/...>`, `<qcom/lte/LteRrcOta.h>` | Qualcomm |
| HDLC / DIAG wire | `<qcom/protocol/...>` | Qualcomm |
| Linux / Android / SIMCOM adapters | `<qcom/linux/...>`, `<qcom/android/...>`, `<qcom/at/...>` | Qualcomm |
| QMI NAS | `<qcom/qmi/...>` | Qualcomm (`QCom::Qmi`, CMake `qcom::qmi`) |
| Private (tests / parser .cpp / dump export) | `"lte/LteQcomLayouts.h"`, `"core/BinaryCursor.h"` | via `src/` |

srsRAN ASN.1 headers are **PRIVATE** to `qcom::parser`. Do not include them from `app/`.

`observer::engine` currently ingest-wires through `RadioScannerEngine` (Qualcomm). That is the
known seam for a second radio: swap the parser behind `IDataSource`, not the survey policy.

## Source layout

```
include/observer/     public Observer API
include/qcom/         public Qualcomm backend API
src/{core,lte,gsm,nr,wcdma}/   private parser impl (layouts, srsRAN)
app/observer/         live_scanner process
qmi_observer/         QMI NAS impl (`qcom::qmi`; public headers in include/qcom/qmi/)
```

## CMake targets

- `observer::model` / `observer::engine`
- `qcom::protocol` / `qcom::parser` / `qcom::io` / `qcom::qmi`
- `observer_app` → binary `live_scanner` (aliases `scanner`, `live_scanner`)

## Daemon seam

`Observer::run_survey()` → `SurveyProc::run()` is the long-running process:
boot / pin / feeds / hop / pump / restore. Config is Job + Recipe (`Options`).
The hop walk still lives in `SurveyHop` (not `SurveySession` yet). GUI consumes
`/tmp/qcom_live_towers.json`. A future `observerd` is a rename of this process,
not a new IPC stack.

## Namespaces (C++)

`QCom` still names the existing types (`CellIdentity`, parsers). New product
code lives in `Observer`. Migrating `QCom::CellIdentity` → a neutral namespace
is a follow-up, not a requirement to add a radio backend.
