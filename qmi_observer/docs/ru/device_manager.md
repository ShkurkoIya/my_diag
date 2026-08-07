# Менеджер устройств модемов (`DeviceCatalog`)

Документ описывает, как программа сканирования должна жить на хосте вроде Raspberry Pi с **горячей сменой USB-модемов**: втыкай / вытыкай SimCom и аналоги, не падай, отдавай GUI быстрый список и по запросу — простуканные профили.

## Зачем это

`Session` / `ScanController` работают с **уже выбранным** QMI-портом. Им нельзя заниматься поиском железа.

`DeviceCatalog` отвечает за:

1. **Быстрый инвентарь** (sysfs, миллисекунды) — что воткнуто сейчас.
2. **Статические профили** — рецепты «как жить с этим семейством» (раскладка tty, quirks).
3. **Runtime dossier** — результат простукивания AT/QMI, сохраняется на диск и отдаётся по запросу.
4. **Сборку `Settings`** для открытия `Session`.

## Два вида знаний

| Вид | Где | Что |
|-----|-----|-----|
| **Static profile** | код (`ProfileRegistry`) | VID/PID, роли USB iface, preferred AT, quirks (`no_dms_offline`) |
| **Dossier** | файл JSON | «на этой малине для id=… QMI ок, AT ок, DMS rev=…» |

GUI/сканер на запрос получают **merge**: `ModemReport { endpoint, profile*, dossier? }`.

## Быстрый vs медленный путь

| Операция | Блокирует GUI? | Что делает |
|----------|----------------|------------|
| `refresh()` | нет (если не на UI-потоке) | только sysfs |
| `list_reports()` | нет | память |
| `probe(id, Identity)` | да, секунды | open QMI + DMS (+ AT) |
| `collect_once` | да | отдельный scan, не каталог |

**Правило:** на старте и hotplug — только `refresh`. Probe — кнопка / фон / первый select.

## Идентификатор endpoint

Предпочтительно: `vid:pid:iSerial` (hex vid/pid).

Если serial пустой или заглушка `0123456789ABCDEF` (часто у Qualcomm):

`vid:pid@usb_path` например `1e0e:9001@1-4` — **меняется при перетыкании в другой порт**.

## Профиль SimCom 83xx (встроенный)

- Match: VID `1e0e`, PID `9001`, product содержит `SDXPRAIRIE`
- Типичные iface: 0 DIAG, 1 NMEA (GNSS), 2 **AT**, 3 modem/PPP, 4 audio, 5 **QMI** (`cdc-wdm`)
- Quirks: `allow_dms_offline=false`, proxy по умолчанию off (exclusive на малине)

## Типовой цикл сканера

```text
DeviceCatalog catalog;
catalog.set_dossier_path("/var/lib/scanner/modems.json");
catalog.load_dossiers();           // молча ok, если файла нет

catalog.refresh();                 // hotplug tick / udev / таймер 1–2с
auto reports = catalog.list_reports();

# выбор пользователя
auto settings = catalog.to_qmi_settings(id);
Session session(settings);
session.open();

# глубокое знание по запросу
catalog.probe(id, {.level=ProbeLevel::Identity, .use_proxy=false});
catalog.save_dossiers();
```

## Hotplug

`refresh()` сравнивает предыдущий список и вызывает:

- `on_added(endpoint)`
- `on_removed(id)`

Если выбранный модем исчез — сканер **обязан** закрыть `Session` и сбросить selection. Не пытайтесь переиспользовать fd после replug.

## Уровни probe

- `Presence` — только то, что уже в endpoint
- `Transport` — QMI open (+ AT AT\\r при `at_probe_safe`)
- `Identity` — DMS manufacturer/model/revision
- `Radio` — health + попытка snapshot (NoNetwork ≠ смерть)

## Файл dossier

Минимальный JSON (version 1), пишет `serialize_dossiers`. Парсер намеренно узкий — не общий JSON library.

Путь задаёте вы (`set_dossier_path`). На малине логично `/var/lib/<app>/modems.json`.

## CLI пример

```bash
# список без probe
./qmi_observer_devices

# probe выбранного (нужен sudo для /dev)
sudo ./qmi_observer_devices --probe --dossier /tmp/modems.json
```

## Тесты

- `make_endpoint_id` / fake serial
- profile match SimCom
- dossier serialize/parse roundtrip
- sysfs fixture enumerate (synthetic tree)
- merge reports

Живой модем в unit-тестах **не обязателен**.
