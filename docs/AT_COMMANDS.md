# AT-команды survey-сканера (SIMCOM SIM8300 / live_scanner)

Документ для коллег: какие AT мы реально шлём из `app/observer/` (`live_scanner`), зачем, с какими аргументами и в каком порядке.  
Проверено на модеме **SIMCOM SIM8300G-M2** (USB: DIAG + AT + QMI). Типичные порты у нас: DIAG `/dev/ttyUSB0`, AT `/dev/ttyUSB2`, QMI `/dev/cdc-wdm0`.

> AT — не единственный источник данных. Идентичность/соседи также приходят из **DIAG** (ML1/RRC) и опционально **QMI**. AT нужен для управления RAT, lock ячеек, PLMN, FPLMN и «золотых» снапшотов вроде `CMGRMI`/`CPSI`.

Ручной зонд: `tools/at_probe.py` (на `/dev/ttyUSB2`).

---

## 1. Как мы ходим в AT

- Сессия: класс `AtSession` в `app/observer/AtSession.h` (raw 115200, `ATE0`, poll/read до `OK`/`ERROR`).
- Один AT-tty на всё: hop / rat-guard / CMGRMI / COPS — **нельзя** параллельно долбить без мьютекса (у нас `AtSession::mu_`).
- Длинные команды (`AT+COPS=?`, manual `COPS=1`) могут висеть десятки секунд; при Stop парсер прерывается по `g_user_stop`.
- Пользователь должен быть в группе `dialout` (доступ к tty). QMI (`cdc-wdm`) часто `root:root 0600` — `qmi-proxy` под тем же uid тоже EACCES. Сканер делает `chmod` / один polkit `chmod a+rw`, иначе DIAG+AT. Навсегда: `sudo cp tools/udev/99-observer-cdc-wdm.rules /etc/udev/rules.d/` + `udevadm trigger -s usbmisc`. `--no-qmi` — явный opt-out.

---

## 2. Карта по ролям

| Роль | Команды |
|------|---------|
| Порт | `ATE0` |
| RAT pin | `AT+CNMP?` / `=14` / `=38` / `=54` |
| RF on/off | `AT+CFUN=4` / `=1` |
| Оператор / поиск | `AT+COPS…`, `AT+CMSSN…` |
| Serving FULL | `AT+CPSI?`, `AT+CNWINFO?`, `AT+CMGRMI=4` |
| LTE lock | `AT+CCELLCFG`, `AT+CLECELL`, `AT+CLEARFCN` |
| WCDMA lock | `AT+CLUCELL`, `AT+CLUARFCN` |
| Полосы | `AT+CSYSSEL="lte_band"|"w_band"` |
| SIM FPLMN | `AT+CRSM` на EF_FPLMN |
| Reg URC (опц.) | `AT+CEREG=2`, `AT+CREG=2` |

---

## 3. Полный справочник команд

### 3.1. База

#### `ATE0`
- **Зачем:** выключить echo, чтобы парсеры видели чистые `OK` / `+CPSI:`.
- **Когда:** один раз при открытии AT fd.
- **Ответ:** `OK`.

---

### 3.2. Режим сети — `AT+CNMP`

SIMCOM «network mode preference».

| Команда | Значение у нас |
|---------|----------------|
| `AT+CNMP?` | Прочитать текущий режим (сохраняем → restore при выходе) |
| `AT+CNMP=14` | **WCDMA-only** (3G survey / IRAT walk) |
| `AT+CNMP=38` | **LTE-only** (основной 4G survey, rat-guard) |
| `AT+CNMP=54` | Restore по умолчанию (WCDMA+LTE), если не сохранили старый |

- **Когда:** старт survey, pin после 3G drop, recover, выход из hop.
- **Важно:** без `CNMP=14` команды `CLUCELL`/`CLUARFCN` часто отвечают `NOT IN WCDMA`.

---

### 3.3. RF — `AT+CFUN`

| Команда | Зачем |
|---------|--------|
| `AT+CFUN=4` | Airplane / RF off (перед wipe FPLMN или «жёстким» recover) |
| `AT+CFUN=1` | RF on |

- **Не используем** `AT+CFUN=1,1` (soft reset): на SIM8300 часто **перечисляет USB** и рвёт DIAG.
- Флаг `--recover-cfun` включает airplane-bounce `4→1` при залипании NO_SERVICE.

---

### 3.4. Оператор — `AT+COPS` / `AT+CMSSN`

#### Форматы, которые шлём

| Команда | Смысл |
|---------|--------|
| `AT+COPS?` | Текущий выбранный оператор (для статуса / PLMN) |
| `AT+COPS=?` | **Полный PLMN scan** (очень долгий, до ~180 с). Сеет ML1/соседей в DISCOVER |
| `AT+COPS=0` | Auto select |
| `AT+COPS=2` | Deregister (раз в deep-search / перед тяжёлым `=?`) |
| `AT+COPS=3,2` | Формат ответов: **numeric** PLMN |
| `AT+COPS=1,2,"mccmnc"` | Manual select (numeric) |
| `AT+COPS=1,2,"mccmnc",7` | Manual + AcT **E-UTRAN/LTE** (`--cops-act 7`) |
| `AT+COPS=1,2,"mccmnc",2` | Manual + AcT **UTRAN/WCDMA** (`--cops-act 2`) |

Ghost-режим (`--cops-ghost-plmn`): принудительный `COPS=1,2,"99999",AcT` (или другой несуществующий PLMN) — модем дольше ищет, DIAG успевает набрать RF.

#### `AT+CMSSN` (SIMCOM)

| Команда | Смысл |
|---------|--------|
| `AT+CMSSN=<mccmnc>` | Hard-pin оператора (дополняет `COPS=1,2`) |
| `AT+CMSSN` | Снять pin |

Используется в full-walk перед ручным PLMN select.

---

### 3.5. Serving identity / измерения

#### `AT+CPSI?`
Главный «паспорт» serving:

- LTE Online/Limited → MCC-MNC, TAC, CID, PCI, EARFCN, BW, RSRQ/RSRP (tenths).
- WCDMA → UARFCN, PSC, LAC, CID, RSCP/EcIo.
- `NO SERVICE` — нормально во время CCELLCFG grind; abort только если ушли в WCDMA/GSM без LTE RF.

Парсер отвергает мусор: `EARFCN=0xFFFFFFFF`, `BAND0`, `PCI=0`, sentinel MCC.

#### `AT+CNWINFO?`
- Отдаёт EGCI (MCC|MNC|ECI) + eNB **без** PCI/EARFCN.
- Штампуем только на уже известный DIAG serving key (`--at-cereg` путь).
- **Не** используем как единственный ключ ячейки (легко приклеить CID не туда).

#### `AT+CMGRMI=4` (LTE goldmine)
Ответ (фрагменты):

```
+CMGRMI: Serving_Cell,<earfcn>,<mcc>,<mnc>,<tac>,…,<cid>,…,<pci>,<rsrq10>,<rsrp10>,<rssi10>
+CMGRMI: LTE_Intra_CellN,<pci>,…
+CMGRMI: LTE_Inter,FreqK,<earfcn>,…
+CMGRMI: LTE_InterFreqK_CellN,<pci>,…
+CMGRMI: CA_Scell,<earfcn>,<pci>,…
```

- Парсер: `include/qcom/at/Cmgrmi.h` → envelopes в CellTracker.
- Режем сентинелы `0xFFFFFFFF` / MCC `0xFFFF` (GUI-мусор `65535`).
- **SINR не отдаёт** — только RSRP/RSRQ/RSSI + BW.
- На `ERROR` (часто NO_SERVICE) — backoff 3–4 с, чтобы не жечь AT USB.
- **Не шлём `CMGRMI=4` пока CPSI `NO SERVICE` / не LTE** — `force=` это не обходит. Соседи в RF-lock идут из DIAG (SIB4/SIB5/ML1) и QMI.
- `AT+CMGRMI=3` — заготовка под WCDMA (пока raw OK, без полного парсера).

#### `AT+CEREG=2` / `AT+CREG=2`
- Включаем URC регистрации при `--at-cereg`.
- CID из CEREG **не штампуем** на «сильный» PCI без EARFCN|PCI (опасно). Serving FULL берём из CPSI/CNWINFO+DIAG.

---

### 3.6. LTE cell / freq lock

Порядок аргументов **критичен** (проверено CLI 2026-08-10):

| Команда | Аргументы | Смысл |
|---------|-----------|--------|
| `AT+CCELLCFG=1,<pci>,<earfcn>` | **PCI, EARFCN** | Sticky Qualcomm cell lock |
| `AT+CCELLCFG=0` | — | Снять |
| `AT+CCELLCFG?` | — | `+CCELLCFG: pci,earfcn` |
| `AT+CLECELL=<earfcn>,<pci>` | **EARFCN, PCI** (обратный порядок!) | SIMCOM cell lock |
| `AT+CLECELL` | — | Clear |
| `AT+CLECELL?` | — | `+CLECELL: earfcn,pci` |
| `AT+CLEARFCN=<band>,<earfcn>` | band из EARFCN (BandInfo) | Freq lock |
| `AT+CLEARFCN` | — | Clear |

#### Dual-lock порядок в hop (обязательно)

```
1) AT+CLEARFCN=<band>,<earfcn>
2) AT+CCELLCFG=1,<pci>,<earfcn>
3) AT+CLECELL=<earfcn>,<pci>
4) AT+COPS=0  (NAS на залоченной ячейке, если не COPS=1)
```

Если сделать `CLECELL` первым — `CCELLCFG?` часто не залипает.
**Не** `CFUN=4→1` после lock: на SIM8300 это вешает AT (~30 с) и сбрасывает lock. Dual-lock без CFUN — рабочий путь (сессия 184138: FDD seed → FULL).

Unlock (все сразу):

```
AT+CCELLCFG=0
AT+CLECELL
AT+CLEARFCN
AT+CLUCELL
AT+CLUARFCN
```

---

### 3.7. WCDMA lock

| Команда | Аргументы | Смысл |
|---------|-----------|--------|
| `AT+CLUCELL=<uarfcn>,<psc>` | UARFCN, PSC | Cell lock |
| `AT+CLUCELL` | — | Clear |
| `AT+CLUCELL?` | — | проверить / `NOT IN WCDMA` |
| `AT+CLUARFCN=<uarfcn>` | UARFCN | Freq-only assist |
| `AT+CLUARFCN` | — | Clear |

Перед lock: `AT+CNMP=14` и желательно уже быть в WCDMA.

---

### 3.8. Полосы — `AT+CSYSSEL`

| Команда | Смысл |
|---------|--------|
| `AT+CSYSSEL="lte_band"` | При старте hop — сохранить список |
| `AT+CSYSSEL="lte_band",1:3:7:20` | Clip LTE bands под frontier (`--hop-band-clip`) |
| `AT+CSYSSEL="w_band"` | Сохранить WCDMA bands |
| `AT+CSYSSEL="w_band",…` | Clip для 3G walk |

На выходе восстанавливаем сохранённые строки.

---

### 3.9. SIM EF_FPLMN — `AT+CRSM`

Файл **28539 / 0x6F7B**, 12 байт.

| Команда | Смысл |
|---------|--------|
| `AT+CRSM=176,28539,0,0,12` | READ — пусто = `FF…FF` |
| `AT+CRSM=214,28539,0,0,12,"FFFFFFFFFFFFFFFFFFFFFFFF"` | UPDATE — wipe |

- Нужен, когда ручной/чужой PLMN попадает в forbidden и модем перестаёт кэмпиться.
- Иногда wipe проходит только после `CFUN=4` → write → `CFUN=1`.
- Флаг `--clear-fplmn` (по умолчанию с full-walk/ghost).

---

## 4. Как это стыкуется с survey pipeline

Упрощённо для **LTE full-walk** (`--survey-mode lte --full-walk`):

```
init
  CNMP=38, COPS=0, (опц.) wipe FPLMN
  save CNMP? / CSYSSEL lte_band

DISCOVER
  COPS=?  →  wipe FPLMN  →  ghost COPS=1,2,"99999",7
  listen unlocked + CMGRMI=4 + DIAG

COMPLETE (hop)
  pick frontier EARFCN|PCI
  CMSSN pin (COPS=1 на SIM8300 обычно ERROR)
  CLEARFCN → CCELLCFG → CLECELL → COPS=0 (invite NAS, unless COPS=1 held)
  grind: CPSI? / CMGRMI=4 only if LTE camped / inject DIAG
  FULL → soak CMGRMI → unlock → next

rediscover каждые N FULL camps
exit: unlock all, restore bands/CNMP, CFUN=1, COPS=0
```

**WCDMA survey:** `CNMP=14`, `CLUCELL`/`CLUARFCN`, `CSYSSEL w_band`, `COPS` с AcT=2.

**IRAT:** LTE walk → WCDMA walk → обратно на LTE.

---

## 5. Таймауты (ориентиры из кода)

| Команда | Типичный timeout |
|---------|------------------|
| `CPSI?`, `CNWINFO?`, `CNMP?`, `*CFG?` | 1.5–2 с |
| `CMGRMI=4` | 3.5 с (+ backoff при ERROR) |
| `CCELLCFG` / `CLECELL` / `CLEARFCN` | 2.5 с |
| `CLUCELL` / `CLUARFCN` | 3 с |
| `CFUN` / `CNMP` pin | 5–10 с |
| `COPS=0` / `COPS=2` | 3–10 с |
| `COPS=1,2,…` | до 60–120 с |
| `COPS=?` | до **180 с** |

---

## 6. Чего сознательно избегаем

| Не делаем | Почему |
|-----------|--------|
| `AT+CFUN=1,1` | USB re-enumeration на SIM8300 |
| CEREG CID → «сильный» PCI без ключа | Фейковые FULL |
| `CMGRMI` spam при NO_SERVICE | ERROR storm на AT tty |
| `COPS=0` во время foreign `COPS=1,2` lock | Срывает PLMN full-walk. **CMSSN pin ≠ COPS=1** — после CCELLCFG всё равно `COPS=0`, иначе NAS остаётся NO SERVICE |
| `CLECELL` **перед** `CCELLCFG` | Lock не залипает |
| Путать порядок PCI/EARFCN | `CCELLCFG` ≠ `CLECELL` |

---

## 7. Быстрые ручные примеры

```bash
# порт AT
sudo python3 tools/at_probe.py /dev/ttyUSB2

# или minicom/picocom:
# ATE0
# AT+CPSI?
# AT+CNMP?
# AT+CMGRMI=4

# LTE dual-lock на EARFCN 375 / PCI 156, band 1:
AT+CNMP=38
AT+CLEARFCN=1,375
AT+CCELLCFG=1,156,375
AT+CLECELL=375,156
AT+CCELLCFG?
AT+CLECELL?
AT+CMGRMI=4

# снять:
AT+CCELLCFG=0
AT+CLECELL
AT+CLEARFCN

# WCDMA:
AT+CNMP=14
AT+CLUCELL=<uarfcn>,<psc>
AT+CLUARFCN=<uarfcn>
```

---

## 8. Связанный код

| Что | Где |
|-----|-----|
| AT session / все вызовы | `app/observer/AtSession.h`, `app/observer/Runtime.cpp` |
| Парсер `CMGRMI=4` | `include/qcom/at/Cmgrmi.h` |
| Band из EARFCN для `CLEARFCN` | `include/observer/model/BandInfo.h` |
| Ручной зонд | `tools/at_probe.py` |
| GUI spawn args | `tower_gui/src/scanner.rs` |

---

## 9. Чеклист «новая AT в пайплайн»

1. Проверить на живом модеме через `at_probe.py` (OK / ERROR / порядок аргументов).
2. Добавить clear-форму (без аргументов), если это lock.
3. Не слать из UI-потока GUI; только из hop/AT worker.
4. Учитывать NO_SERVICE → backoff.
5. На Stop команда должна уметь прерваться (короткий timeout или poll `g_user_stop`).
6. Дописать сюда строку в таблицу.

---

*Последнее обновление по коду: dual-lock CLECELL/CCELLCFG/CLEARFCN, CMGRMI, CMSSN, FPLMN CRSM — ветка live_scanner survey.*
