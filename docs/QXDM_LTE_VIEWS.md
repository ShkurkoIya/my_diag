# QXDM LTE Views map (QXDM 4 / SIM8300)

Path: **View → LTE → …**  
Always confirm the real log code in **Item View → Key** (views can share codes).

Oracle workflow: open view → catch packet → hex + QXDM decode → diff parser → `[real]` test.

## Management Layer 1

| View | Domains | Typical codes | Ours | Priority |
|------|---------|---------------|------|----------|
| Serving Cell Measurements Display | RF / forensics / geo | **0xB193** | ✅ oracle | P0 done |
| Serving Cell Measurements RSSI SINR | same | often B193 | ✅ | UI dupe |
| Connected Mode RSRP RSRQ SINR | connected RF | B193 (± B179) | B193✅ B179 present | re-check Item View |
| Real Time RSRP | high-rate RSRP | B193 / B17F | B17F present | re-verify B17F |
| Carrier Agg Summary | CA / SCell | CA + B193 idx | started | P1 if CA live |
| SCell State Change | CA | SCell events | weak | P2 |
| DL Throughput Estimation | operator KPI | thr | — | P2 |
| DL Throughput and BLER | operator / ML | BLER/thr | — | P2 gold |
| UL Throughput and BLER | operator | UL | — | P2 |
| Uplink HARQ Info | link | HARQ | — | P3 |
| Uplink Tx Display | Tx power / anomaly | Tx | — | P2 |
| PMCH Throughput and BLER | eMBMS | — | — | skip |
| Sleep State | DRX timeline | sleep | — | P3 |
| TuneAway Graph | IRAT | tuneaway | — | P3 |
| LTED Scheduling Pattern | D2D | — | — | skip |

## Lower Layer 1

| View | Domains | Typical codes | Ours | Priority |
|------|---------|---------------|------|----------|
| Timing Advance Display | geo / fake-BS | **0xB114** | ✅ | P0 done |
| CER Rx Tx Pair | search / PHY | B113 / B123? | stubs | P1 re-verify |
| CQI RI MCS Display | operator / ML | CQI/MCS | — | P2 gold |
| PUSCH PHY Throughput | UL PHY | PUSCH | — | P2 |
| eMBMS SNR | MBMS | — | — | skip |

## PDCP Layer

| View | Priority |
|------|----------|
| PDCP Config / Throughput / DL / LWA | P3 — little for BS identity/geo |

## RRC Layer (next oracle block)

| View | Domains | Typical codes | Ours | Priority |
|------|---------|---------------|------|----------|
| **RRC/NAS Status Screen** | identity + NAS | often **0xB0C2** | B0C2 present | **#1 now** |
| RRC State Change Plot | Idle↔Connected | **event 1606** | **oracle OK** (Suspend=5 assumed) | P1 |
| RRC Mobility Graph | HO / reselection | OTA + meas | weak | P1 gold |

Also via Item View (not always a dedicated view):

| Code | Role | Ours |
|------|------|------|
| **0xB0C0** RRC OTA | SIB1–7, MeasConfig/Report, HO | ASN.1 — live re-verify |
| **0xB0C1** MIB | BW / SFN | present |
| **0xB0C2** Serving Cell Info | PLMN/TAC/CID/band/BW | **oracle OK** (v3 LE; Full=wire0; BW=NRB) |
| **0xB0EE** NAS EMM State | reg state / PLMN / MME | **oracle OK** (v2; M-TMSI not exported) |
| **Evt 1606** RRC State Change | Idle/Connected/… | **oracle OK** (DIAG event, not log) |
| 0xB0C4 PLMN Search Rsp | scan / anomaly | present |
| 0xB0CB Paging | presence | stub |
| 0xB0CD CA combos | UE CA cap | stub |

## MAC / VoLTE / Video / RLC

| Section | Views | Priority |
|---------|-------|----------|
| MAC | Mac Config (+ RACH logs B061–B064) | P1 access anomaly |
| VoLTE | IMS/RTP/jitter | P3 voice QoE |
| Video LTE | RTP | skip for BS/geo |
| RLC | Config / Throughput | P3 KPI dupe |

## Outside views (must)

| Source | Role |
|--------|------|
| AT+CMGRMI=4 | neigh + serving + TA while DIAG neigh silent |
| AT+CPSI? | camp / passport check |
| QMI NAS | reg + cell-loc backup |
| Raw DIAG journal | forensics / re-parse |

## Suggested oracle order

1. RRC/NAS Status → B0C2  
2. Item View B0C0 SIB1/2/3/5  
3. B0C1 MIB  
4. B17F re-verify  
5. CER → real codes  
6. CQI / thr / BLER  
7. MAC RACH  
8. NAS EMM/ESM — **0xB0EE** EMM State oracle OK (v2; no M-TMSI in export)

Identity merge policy: see `docs/CELL_IDENTITY_MATCHING.md`.
