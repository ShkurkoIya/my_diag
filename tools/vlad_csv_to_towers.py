#!/usr/bin/env python3
"""Convert Vlad observer_cells_*.csv → qcom.towers.v4 JSON.

Default filter matches journal_replay TowerExport:
  complete_passport_rf_unique_cid_latest
  — MCC + LAC/TAC + CID + ARFCN/EARFCN/(UARFCN);
    LTE/WCDMA/NR also require PCI/PSC; GSM needs ARFCN+CID (BSIC optional);
    unique by RAT|CID, latest last_seen wins.

Use --all for previous behaviour (every unique RAT|ARFCN|CODE row).
"""
from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def S(x) -> str:
    return "" if x is None else str(x).strip()


def format_mnc(mnc) -> str:
    """Zero-pad MNC to 2 digits, or 3 if >= 100 (matches TowerExport.h)."""
    s = S(mnc)
    if not s:
        return ""
    try:
        n = int(s)
    except ValueError:
        return s
    if n >= 100:
        return f"{n:03d}"
    return f"{n:02d}"


def is_complete(r: dict) -> bool:
    rat = S(r.get("rat")).upper()
    mcc, lac, cid, arfcn = S(r.get("mcc")), S(r.get("lac")), S(r.get("cid")), S(r.get("arfcn"))
    if not mcc or not lac or not cid or cid == "0" or not arfcn:
        return False
    if rat == "GSM":
        return True
    if rat in ("LTE", "WCDMA", "NR"):
        return bool(S(r.get("pci")))
    return False


def tower_from_row(r: dict) -> tuple[str, dict]:
    rat = S(r.get("rat")).upper()
    arfcn = S(r.get("arfcn"))
    pci = S(r.get("pci"))
    bsic = S(r.get("bsic"))
    code = pci if rat in ("LTE", "WCDMA", "NR") else bsic
    key = f"{rat}|{arfcn or '0'}|{code or '0'}"
    mcc = S(r.get("mcc"))
    mnc = format_mnc(r.get("mnc")) if mcc else S(r.get("mnc"))
    mcc_mnc = f"{mcc}-{mnc}" if mcc and mnc else (mcc or "")
    lac = S(r.get("lac"))
    cid = S(r.get("cid"))
    band = S(r.get("band"))
    serving = S(r.get("serving")) or "0"
    seen = S(r.get("seen"))
    first_seen = S(r.get("first_seen"))
    last_seen = S(r.get("last_seen"))
    rxl = S(r.get("signal_dbm"))
    rsrq = S(r.get("rsrq"))
    c1, c2 = S(r.get("c1")), S(r.get("c2"))
    ncc, bcc = S(r.get("ncc")), S(r.get("bcc"))
    lat, lon = S(r.get("lat")), S(r.get("lon"))

    meta = {
        "serving": serving,
        "seen": seen,
        "first_seen": first_seen,
        "last_seen": last_seen,
    }
    if lat or lon:
        meta["lat"] = lat
        meta["lon"] = lon

    if rat == "LTE":
        identity = {
            "mcc": mcc,
            "mnc": mnc,
            "mcc_mnc": mcc_mnc,
            "tac": lac,
            "cid": cid,
            "enb_id": "",
            "ncell_id": "",
        }
        if cid.isdigit() and int(cid) > 0:
            c = int(cid)
            identity["enb_id"] = str(c >> 8)
            identity["ncell_id"] = str(c & 0xFF)
        radio = {
            "earfcn": arfcn,
            "ul_earfcn": "",
            "pci": pci,
            "dl_code": pci,
            "ul_code": pci,
            "band": band,
            "duplex_type": "",
            "dl_freq": "",
            "ul_freq": "",
            "bandwidth": "",
            "ul_bw": "",
        }
        signal = {"rxl": rxl, "rsrq": rsrq, "snr": "", "rssi": ""}
        neighbors: dict = {"nb_lte": [], "nb_gsm": [], "nb_umts": []}
    elif rat == "WCDMA":
        identity = {
            "mcc": mcc,
            "mnc": mnc,
            "mcc_mnc": mcc_mnc,
            "lac": lac,
            "cid": cid,
            "rnc_id": "",
            "cid16": "",
        }
        if cid.isdigit() and int(cid) > 0:
            c = int(cid)
            identity["rnc_id"] = str((c >> 16) & 0xFFF)
            identity["cid16"] = str(c & 0xFFFF)
        radio = {
            "uarfcn": arfcn,
            "ul_uarfcn": "",
            "psc": pci,
            "dl_code": pci,
            "ul_code": pci,
            "band": band,
            "duplex_type": "",
            "dl_freq": "",
            "ul_freq": "",
        }
        signal = {"rxl": rxl, "snr": rsrq, "ecio": rsrq}
        neighbors = {"nb_gsm": [], "nb_umts": []}
    else:
        identity = {
            "mcc": mcc,
            "mnc": mnc,
            "mcc_mnc": mcc_mnc,
            "lac": lac,
            "cid": cid,
        }
        radio = {
            "arfcn": arfcn,
            "bsic": bsic,
            "ncc": ncc,
            "bcc": bcc,
            "band": band,
            "dl_code": bsic,
        }
        signal = {"rxl": rxl, "snr": "", "rxqual": "", "c1": c1, "c2": c2}
        neighbors = {"nb_gsm": []}

    return rat, {
        "key": key,
        "meta": meta,
        "identity": identity,
        "radio": radio,
        "signal": signal,
        "neighbors": neighbors,
    }


def convert(src: Path, out: Path, *, all_rows: bool) -> dict:
    with src.open(encoding="utf-8-sig", newline="") as f:
        f.readline()  # sep=,
        rows = list(csv.DictReader(f))

    # unique key: RAT|CID (complete) or RAT|tower.key (loose)
    best: dict[tuple[str, str], dict] = {}
    kept_raw = 0
    for r in rows:
        if not all_rows and not is_complete(r):
            continue
        kept_raw += 1
        rat, t = tower_from_row(r)
        uid = t["identity"].get("cid", "") if not all_rows else t["key"]
        k = (rat, uid)
        prev = best.get(k)
        if prev is None or t["meta"]["last_seen"] >= prev["meta"]["last_seen"]:
            best[k] = t

    by = {"gsm": [], "lte": [], "wcdma": [], "nr": []}
    for (rat, _), t in best.items():
        if rat == "GSM":
            by["gsm"].append(t)
        elif rat == "LTE":
            by["lte"].append(t)
        elif rat == "WCDMA":
            by["wcdma"].append(t)
        elif rat == "NR":
            by["nr"].append(t)

    for xs in by.values():
        xs.sort(key=lambda t: t["meta"].get("last_seen", ""), reverse=True)

    as_of = max(
        (t["meta"]["last_seen"] for xs in by.values() for t in xs if t["meta"]["last_seen"]),
        default="",
    )
    serving = sum(1 for xs in by.values() for t in xs if t["meta"]["serving"] == "1")
    filt = (
        "vlad_csv_all_unique_key_latest"
        if all_rows
        else "complete_passport_rf_unique_cid_latest"
    )
    doc = {
        "meta": {
            "source": str(src),
            "situation_as_of": as_of,
            "tower_count": str(sum(len(v) for v in by.values())),
            "raw_tower_count": str(len(rows)),
            "filter": filt,
            "serving_count": str(serving),
            "towers_with_neighbors": "0",
            "gsm": str(len(by["gsm"])),
            "lte": str(len(by["lte"])),
            "wcdma": str(len(by["wcdma"])),
            "nr": str(len(by["nr"])),
            "schema": "qcom.towers.v4",
            "origin": "vlad",
        },
        "towers": by,
    }
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n")
    return doc


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("csv", type=Path)
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument(
        "--all",
        action="store_true",
        help="keep all rows unique by RAT|ARFCN|CODE (old behaviour)",
    )
    args = ap.parse_args()
    doc = convert(args.csv, args.out, all_rows=args.all)
    print(
        f"wrote {args.out} towers={doc['meta']['tower_count']} "
        f"raw={doc['meta']['raw_tower_count']} filter={doc['meta']['filter']}"
    )


if __name__ == "__main__":
    main()
