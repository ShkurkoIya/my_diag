# Survey engine (`observer::engine`)

Библиотека RF-survey поверх парсера. Не фреймворк и не DSL: порты, стратегия, проекция.
Стратегия живёт в ядре; адаптер модема знает только диалект, quirks и caps.
GUI не зависит от этого слоя.

## Слои

| Слой | Target | Что это |
|------|--------|---------|
| L0 | `observer::model` | `CellIdentity`, паспорт, radio/signal (3GPP, не Qualcomm) |
| L1 | `qcom::protocol` | HDLC / DIAG framing |
| L2 | `qcom::parser` | Qualcomm log-коды → tracker |
| L3 | `observer::engine` | фасад, порты, стратегия, проекция (без OS I/O) |
| L4 | `qcom::io` | Linux SIMCOM AT, Android DCI stub, journal |
| L5 | `Observer` | installable RF-air survey (`live_scanner`); Qualcomm is one backend |

Валюта фасада — `Tower` / `Operator` / `SurveyStats`, не сырой `CellIdentity`.
FULL = тот же предикат, что hop-planner (`Lte::cell_is_full_lte`): PLMN + TAC + ECI на EARFCN|PCI.

## Как пользоваться

```cpp
#include <observer/engine/Survey.h>

using namespace QCom::Engine;

auto session = SurveySession::builder()
                   .source(std::move(diag_source))      // или NullSource + ingest()
                   .control(std::make_unique<SimcomAtControl>(at_transact))
                   .config({.active_walk = true})       // на Android без cell_lock → passive
                   .build();

session.add_sink(&json_sink);
session.start();
while (running) {
  session.poll();                       // refresh + decide + apply
  const SurveyResult& r = session.result();
  // r.stats.lte_rf_unique vs r.stats.lte_full — честные счётчики
}
```

Escape hatches: `session.engine()` / `session.tracker()` — сырые пакеты и merge.

Офлайн / тесты:

```cpp
session.ingest(QualcommPacketView{.log_code = 0xB0C2, .payload = bytes});
const auto& r = session.refresh();
```

## Caps и деградация

Стратегия объявляет `required_caps()`. Если модем не умеет `cell_lock` (типичный Android),
сессия сама ставит `PassiveMonitorStrategy` — тот же core, без actuation.

| Адаптер | Caps | Где |
|---------|------|-----|
| `SimcomAtControl` | full (lock + COPS + CFUN) | Linux SIM8300, AT через `AtTransact` |
| `AndroidControl` | только `diag_log_mask` | Snapdragon DCI, passive survey |
| `NullModemControl` | ничего | journal replay, unit-тесты |

`SimcomAtControl` не открывает tty и не спит. Dual-lock: `CLEARFCN → CCELLCFG → CLECELL`.
Политика retry/CFUN-bounce/ghost остаётся в приложении.

## Что считает проекция

`project_lte(snapshot)` — один проход:

- `lte_rf_unique` — правдоподобный EARFCN|PCI (не padding)
- `lte_full` — идентифицированные носители (= `towers.size()`)
- `lte_sites` — уникальные eNB (ECI >> 8)
- операторы, сгруппированные по PLMN

`live_scanner` пишет `rf_unique` / `full_passport` / `lte_sites` из этой проекции.
`write_towers_json_survey` штампует те же поля, если приложение их не задало.

## Приложение

Неймспейс `Observer` (не `QCom`). CMake target `observer_app` (aliases `scanner` /
`live_scanner`), бинарник `live_scanner`. Qualcomm-библиотеки — backend.

```
app/main.cpp               — CLI entry
app/observer/Options.*     — флаги и usage
app/observer/AtParse.h     — CPSI/CEREG/COPS/FPLMN
app/observer/AtSession.h   — AT tty
app/observer/Dashboard.h   — таблица + лог
app/observer/WcdmaWalk.h   — 3G hop planner
app/observer/Runtime.cpp   — воркеры DIAG/AT/QMI/hop
```

Синтетический demo: `tools/synth_demo.cpp` (`synth_demo`).

- `include/observer/engine/Survey.h` — umbrella
- `include/observer/engine/SurveyDomain.h`, `SurveyProjection.h`, `SurveyStrategy.h`, `SurveySession.h`
- `include/observer/engine/ModemControl.h`
- `include/qcom/linux/SimcomAtControl.h` — SIMCOM AT dialect
- тесты: `tests/test_survey_engine.cpp` (`[engine]`)
