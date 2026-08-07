#!/usr/bin/env bash
# Recover SIM8300/QMI after DMS offline/reset left RF/DIAG dead or ports wedged.
#
# Typical symptoms:
#   - live_scanner: raw_bytes=0, Diag masks init OK but silence
#   - qmicli: CID allocation / Transaction timed out
#   - open(/dev/ttyUSB*) hangs
#
# Usage:
#   sudo ./tools/qmi_recover.sh
#   sudo QMI_DEVICE=/dev/cdc-wdm0 ./tools/qmi_recover.sh
set -euo pipefail

DEV="${QMI_DEVICE:-/dev/cdc-wdm0}"
VIDPID="${USB_VIDPID:-1e0e:9001}"
QMI_TIMEOUT_SEC="${QMI_TIMEOUT_SEC:-12}"

qmi() {
  # Never hang forever on a wedged QMI endpoint.
  timeout --signal=KILL "${QMI_TIMEOUT_SEC}" qmicli -d "$DEV" "$@"
}

echo "== kill stale qmi clients / MM =="
systemctl stop ModemManager 2>/dev/null || true
killall -q qmicli qmi-proxy 2>/dev/null || true
sleep 1

echo "== USB reset ${VIDPID} (if possible) =="
if command -v usbreset >/dev/null 2>&1; then
  usbreset "$VIDPID" 2>/dev/null || true
else
  # Fallback: USBDEVFS_RESET via python
  python3 - <<'PY' || true
import glob, os, fcntl, subprocess, re
vidpid = os.environ.get("USB_VIDPID", "1e0e:9001").lower()
vid, pid = vidpid.split(":")
out = subprocess.check_output(["lsusb", "-d", f"{vid}:{pid}"], text=True)
# Bus 001 Device 007: ...
m = re.search(r"Bus\s+(\d+)\s+Device\s+(\d+)", out)
if not m:
    raise SystemExit("lsusb parse failed")
bus, dev = int(m.group(1)), int(m.group(2))
path = f"/dev/bus/usb/{bus:03d}/{dev:03d}"
print("reset", path)
USBDEVFS_RESET = (ord("U") << 8) | 20
fd = os.open(path, os.O_WRONLY)
try:
    fcntl.ioctl(fd, USBDEVFS_RESET, 0)
    print("USBDEVFS_RESET ok")
finally:
    os.close(fd)
PY
fi

echo "== wait for $DEV =="
for i in $(seq 1 60); do
  if [[ -e "$DEV" ]]; then
    echo "present (try $i)"
    ls -l "$DEV"
    break
  fi
  echo "try $i: missing"
  sleep 2
done
if [[ ! -e "$DEV" ]]; then
  echo "FATAL: $DEV never came back" >&2
  exit 1
fi

# Allow dialout users to open QMI without fighting root-only 0600.
if [[ -e "$DEV" ]]; then
  chmod 666 "$DEV" 2>/dev/null || chmod 660 "$DEV" 2>/dev/null || true
  chgrp dialout "$DEV" 2>/dev/null || true
  ls -l "$DEV"
fi

# Brief settle after USB re-enum
sleep 3

echo "== operating mode (timeout ${QMI_TIMEOUT_SEC}s) =="
if ! qmi --dms-get-operating-mode; then
  echo "WARN: DMS still wedged after USB reset — re-plug cable and re-run" >&2
  exit 2
fi

mode="$(qmi --dms-get-operating-mode 2>/dev/null | awk -F"'" '/Mode:/{print $2; exit}' || true)"
echo "parsed mode: ${mode:-unknown}"

if [[ "${mode:-}" != "online" ]]; then
  echo "== try online =="
  if ! qmi --dms-set-operating-mode=online; then
    echo "== online failed; try low-power -> online =="
    qmi --dms-set-operating-mode=low-power || true
    sleep 2
    qmi --dms-set-operating-mode=online || true
  fi
  sleep 3
  qmi --dms-get-operating-mode || true
fi

echo "== SSP =="
qmi --nas-get-system-selection-preference || true

echo "== force network search =="
qmi --nas-force-network-search || true

echo "== wait for serving / cell info (up to ~60s) =="
ok=0
for i in $(seq 1 12); do
  echo "--- poll $i ---"
  qmi --nas-get-serving-system || true
  if qmi --nas-get-cell-location-info; then
    ok=1
    break
  fi
  sleep 5
done

if [[ "$ok" -ne 1 ]]; then
  echo "== still no cell; signal / RF band =="
  qmi --nas-get-signal-info || true
  qmi --nas-get-rf-band-info || true
  echo "HINT: radio 'none' + NoNetworkFound with online often = antenna / still scanning"
fi

echo "== tty sanity =="
ls -l /dev/ttyUSB0 /dev/ttyUSB2 2>/dev/null || true
echo "Next: ./build/live_scanner --duration 20"

echo "== done =="
