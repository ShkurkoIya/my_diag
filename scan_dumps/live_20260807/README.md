# Live scan snapshot — 2026-08-07

Saved multi-hour `live_scanner` session so you do not need to rescan.

## Files

| File | Meaning |
|------|---------|
| `qcom_live_towers.raw.json` | Exact dump as produced by scanner (~3.1 MB) |
| `qcom_live_towers.json` | Same cells; sticky `meta.camped=1` cleared when **no CID** (GUI-friendly) |
| `qcom_live_towers.normalized.json` | Copy of the normalized feed |
| `qcom_live_towers_400_v4_20260807_203831.json` | Later snapshot: **400** RF rows (v4, before export-all / v5). LTE 399 + WCDMA 1, complete 54 |

Point GUI at either file:

```bash
# default watches /tmp — already refreshed from normalized copy
./tower_gui/target/release/tower_gui
```

## How to read the numbers (raw analysis)

| Metric | Value | Meaning |
|--------|------:|---------|
| Unique `LTE\|EARFCN\|PCI` | **286** | Real distinct RF cells — **not** 286 duplicate towers |
| FULL (CID+TAC+PLMN) | **48** | Real identified carriers |
| eNB sites | **14** | Physical sites (eNB ID groups) |
| Sticky `camped` without CID (raw) | **~200** | Hop/CCELLCFG/ML1 lock marked serving without SIB1 — **misleading** |
| Neighbor stubs under FULL | **~2670** | Mostly SIB5 **EARFCN-only** (no PCI) — not towers |

So the honest summary is closer to:

**14 sites · 48 FULL carriers · ~240 RADIO PCI detections · SIB5 earfcn hints**

not “285 towers”.

## Roles

- **SERVING** — modem camped here *now*
- **CAMPED** — had FULL identity and was serving at least once
- **RADIO** — EARFCN+PCI seen (meas / SIB4 / hop), **no CID yet** (often neighbor of a known eNB on same EARFCN)
- SIB5 lines in inspector — frequency candidates, not cells

## Operators in this dump

250-01 MTS, 250-02 MegaFon, 250-20 t2, 250-99 Beeline, 250-21 (+ unknown PLMN orphans).
