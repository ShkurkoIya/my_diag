# External references (shallow clones)

Read-only upstream trees for DIAG / QXDM / survey research. Prefer these over ad-hoc web browsing.

| Directory | Upstream | Use for us |
|-----------|----------|------------|
| `QCSuper/` | [P1sec/QCSuper](https://github.com/P1sec/QCSuper) | DIAG framing, Inputs/Modules architecture, DLF, PCAP/gsmtap; see `docs/QCSuper architecture.md`, `docs/The Diag protocol.md` |
| `scat/` | [fgsect/scat](https://github.com/fgsect/scat) | Authoritative LTE/NR ML1 binary layouts for parsers |
| `qxdm_filter_merge/` | [woodstone10/…log_filter_and_merge-perl](https://github.com/woodstone10/qualcomm_automation_script_log_filter_and_merge-perl) | Log-code lists / QXDM filter merge (source of `../qxdm_filter_ref.pl`) |
| `afts_throughput/` | [woodstone10/…throughput_test-shell](https://github.com/woodstone10/qualcomm_automation_script_throughput_test-shell) | FIT session logging; `LTE_Throughput.dmc` mask set |
| `qxdm_html_views/` | [woodstone10/…qxdm_log_analysis_view-html](https://github.com/woodstone10/qualcomm_qxdm_log_analysis_view-html) | QXDM HTML UI ideas (RSRP timeline, Hz strip, CA/HO); parses QXDM text, not raw binary |
| `qcat_log_analysis/` | [woodstone10/qualcomm_qcat_log_analysis-python](https://github.com/woodstone10/qualcomm_qcat_log_analysis-python) | Offline QCAT/text log analysis patterns |
| `qxdm_5g_mmw_analysis/` | [woodstone10/qualcomm_5G_mmw_log_analysis](https://github.com/woodstone10/qualcomm_5G_mmw_log_analysis) | NR/mmWave log analysis notes |
| `mobileinsight-core/` | [mobileinsight-project/mobileinsight-core](https://github.com/mobileinsight-project/mobileinsight-core) | DIAG log Fmt layouts (`dm_collector_c/`); e.g. `lte_phy_idle_neighbor_cell_meas.h` for **0xB192** subpkts 26/27 |

## Already in `knowlage_base/` (not duplicated here)

- `qxdm_filter_ref.pl` — extracted filter reference
- `SIM8200C Series_AT Command Manual_V1.00.pdf` — AT survey commands (CCED, COPS, CFUN, SPTESTMODE, …)
- `SIM826XX_…Hardware Design…pdf` — HW (limited DIAG value)

## Local (not cloned)

- `~/Desktop/dia_vldos` — Android/C++ DIAG parsers (fail-closed ML1 rewrite)

## Gaps vs our stack (from QCSuper docs)

We already cover proprietary cell identity (B0C2/B193/B0C4) + QMI + CellTracker better than QCSuper’s PCAP focus. Still missing relative to QCSuper:

1. DLF dump/read for QXDM interop
2. PCAP/gsmtap export of RRC OTA (helps debug missing SIB1 `pdu=3`)
3. Explicit single-DIAG-client / module lifecycle discipline
4. Optional geo-tagged JSON dump

Higher priority for air survey remains: PLMN→PCI merge, AT survey pipeline, `--towers-json` on `live_scanner`.

## Update clones

```bash
cd knowlage_base/refs
for d in */; do git -C "$d" pull --ff-only; done
```
