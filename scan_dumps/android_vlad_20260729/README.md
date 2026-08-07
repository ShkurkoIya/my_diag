# Android Vlad scan dump — 2026-07-29

Extracted from `scan.rar` (Telegram) via `unar` (RAR5).

```
android_vlad_20260729/
  files/
    observer_cells_gsm-wcdma-lte_20260729_172858.csv   # 526 cells (GSM/LTE/WCDMA)
    journal/observer_journal_20260729_172906.{csv,txt} # 21807 DIAG packets + RAW
    logs/cpp_*.log, scan_*.log                         # lock/sweep session logs
```

## Notes for our parser

- No NR packets in this capture (GSM/WCDMA/LTE sweep only).
- GSM on this device is mostly **DSDS** codes: `0x5A6C/5A71/5A7A/5A7B/5B34` (not `0x506C/5071/...`).
- `0x512F` SI uses compact body after `chan|PD|msg_type` (CID starts at offset 3).
- LTE RSRP: trust `0xB197` for PCI; `0xB17F` PCI is unreliable (correct from serving).
- Cells CSV columns: `rat,arfcn,band,pci,bsic,ncc,bcc,mcc,mnc,lac,cid,signal_dbm,rsrq,c1,c2,serving,...`

## Tower JSON (qcom.towers.v4)

Same filter for both dumps: `complete_passport_rf_unique_cid_latest`
(MCC+LAC/TAC+CID+channel; LTE/UMTS also PCI/PSC; unique by RAT|CID, latest `last_seen`).

| file | source | notes |
|------|--------|-------|
| `ours_towers.json` | journal replay | ~73 complete unique |
| `vlad_towers.json` | cells CSV via `tools/vlad_csv_to_towers.py` | ~90 complete unique (of 526 raw; most rows lack MCC) |

```bash
python3 tools/vlad_csv_to_towers.py \
  scan_dumps/android_vlad_20260729/files/observer_cells_gsm-wcdma-lte_20260729_172858.csv \
  -o scan_dumps/android_vlad_20260729/vlad_towers.json
# optional: --all  → old behaviour (all unique RAT|ARFCN|CODE, ~475)
```
