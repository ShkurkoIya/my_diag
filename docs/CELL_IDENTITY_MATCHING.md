# LTE cell identity matching (fail-closed)

Problem we hit in live scans: **many EARFCN|PCI RADIO rows**, few FULL passports,
and soft heuristics made it look like “everyone belongs to operator X / eNB Y”.

Registry key stays physical: **`{EARFCN, PCI}`**. Passport (PLMN/TAC/ECI) attaches
only under strict rules.

## Confidence tiers

| Tier | What | When allowed |
|------|------|----------------|
| **HARD FULL** | PLMN + TAC + ECI on `EARFCN\|PCI` | SIB1 / B0C2 / CPSI / CMGRMI serving with **same** EARFCN+PCI (or unique attach) |
| **HARD PLMN** | PLMN only, `plmn_soft=false` | Passport event on that exact key without soft flag |
| **SOFT PLMN** | PLMN only, `plmn_soft=true` | Same-EARFCN fan-out from a HARD row — **hint only** |
| **RF-ONLY** | no PLMN / no CID | ML1/CER/CMGRMI neighbor meas |

Never promote SOFT → FULL. Never copy **CID/TAC** via fan-out.

## Bind rules (LTE)

1. **Exact RF key** — Passport/Radio with `EARFCN≠0` and `PCI∈[1..503]` → that row only.
2. **EARFCN|0 weak row** — strip ECI/TAC before merge; absorb into `EARFCN|PCI` when PCI appears (`promote_lte_weak_row`).
3. **Unique PCI on EARFCN** — if passport arrives on `EARFCN|0` and exactly one PCI row exists → attach there.
4. **Ambiguous EARFCN|0** (several PCIs) — keep weak row / PLMN-only fan-out; **do not** invent CID.
5. **ECI uniqueness** — `claim_lte_eci`: one ECI → one physical key; strip CID/TAC from impostors (PLMN may remain soft/hard).
6. **Serving sticky** — orphan identity without key may bind to serving only if serving has no CID or same CID (`resolve_passport_key`). No “match by RSRP/band alone”.

## Secondary parameters — only if exact

Allowed as **confirm**, not as sole bind:

| Secondary | Use |
|-----------|-----|
| Band / EARFCN | must already match key |
| TAC | confirm; never assign CID because TAC matched another cell |
| eNB-id (ECI>>8) | GUI grouping of **FULL** rows only |
| RSRP / TA / SFN | ranking / geo / anomaly — **not** identity merge |

Forbidden sole keys: “same band”, “same TAC”, “closest RSRP”, “same soft PLMN”.

## GUI

- `plmn_soft=1` and no CID → **ungrouped** Heard RF (`plmn_trusted()` empty).
- Operator / eNB tree: only trusted PLMN; eNB nodes only when CID present.
- Show soft PLMN in details as “inferred (same EARFCN)” if useful later.

## How to get more FULL towers (ops, not heuristics)

1. Camp / hop onto `EARFCN|PCI` until SIB1/B0C2/CPSI (`--earfcn-hop` grind).
2. `AT+CMGRMI=4` while camped — serving FULL + neigh RF (neigh usually RF-only).
3. Re-verify B0C2 / B0C0 on QXDM (see `docs/QXDM_LTE_VIEWS.md`).
4. Keep raw DIAG journal — re-parse later beats guessing identity in the tracker.

## Code touchpoints

- `CellPassport::plmn_soft` — `include/observer/model/CellIdentity.h`
- Fan-out / merge — `CellTracker::fanout_lte_plmn_same_earfcn`, `merge_passport`
- Export — `identity.plmn_soft` via `TowerExport.h`
- GUI — `Tower::plmn_trusted()` / live operator rebuild
