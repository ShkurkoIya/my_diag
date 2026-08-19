#!/usr/bin/env bash
# Fix Oracle VirtualBox on Fedora: matching kernel-devel, build vboxdrv, Secure Boot hint.
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
  echo "Run: sudo bash $0"
  exit 1
fi

echo "==> Current kernel: $(uname -r)"
echo "==> Secure Boot: $(mokutil --sb-state 2>/dev/null || echo unknown)"

# Quiet noisy broken interactive GPG prompts if Cursor key missing
rpm --import https://downloads.cursor.com/keys/anysphere.asc 2>/dev/null || true

echo "==> Installing build deps + newest kernel stack with matching devel"
dnf -y install \
  gcc make perl elfutils-libelf-devel \
  kernel kernel-core kernel-modules kernel-modules-extra \
  kernel-devel kernel-devel-matched kernel-headers

echo "==> Installed kernel packages:"
rpm -q kernel-core kernel-devel kernel-headers || true
ls /usr/src/kernels/ || true

RUNNING="$(uname -r)"
if [[ ! -d "/usr/src/kernels/${RUNNING}" ]] && [[ ! -d "/lib/modules/${RUNNING}/build" ]]; then
  echo
  echo ">>> No kernel-devel for RUNNING kernel ${RUNNING}."
  echo ">>> A newer kernel was installed. Reboot into it, then re-run this script:"
  echo "    sudo bash $0"
  echo
  # Show newest kernel-core
  rpm -q kernel-core | sort -V | tail -3
  exit 2
fi

MOK_DIR=/var/lib/shim-signed/mok
if mokutil --sb-state 2>/dev/null | grep -qi enabled; then
  mkdir -m 0700 -p "${MOK_DIR}"
  if [[ ! -f "${MOK_DIR}/MOK.der" ]]; then
    echo "==> Generating MOK key for module signing"
    openssl req -nodes -new -x509 -newkey rsa:2048 -outform DER \
      -addext "extendedKeyUsage=codeSigning" \
      -keyout "${MOK_DIR}/MOK.priv" \
      -out "${MOK_DIR}/MOK.der" \
      -days 36500 \
      -subj "/CN=VirtualBox Module Signing MOK/"
    chmod 600 "${MOK_DIR}/MOK.priv"
  fi

  TEST_OUT="$(mokutil --test-key "${MOK_DIR}/MOK.der" 2>&1 || true)"
  echo "==> mokutil --test-key: ${TEST_OUT}"

  if echo "${TEST_OUT}" | grep -qi 'is already enrolled'; then
    echo "==> MOK enrolled in firmware"
  elif echo "${TEST_OUT}" | grep -qi 'enrollment request' || mokutil --list-new 2>/dev/null | grep -q .; then
    echo
    echo "MOK is QUEUED but not enrolled yet (this is why the script stops)."
    echo "Do NOT re-run import. Just reboot and finish the blue screen ONCE:"
    echo "  1) sudo reboot"
    echo "  2) Blue 'Perform MOK management' → Enroll MOK → Continue → Yes"
    echo "  3) Password = the one you typed at 'input password' (last run)"
    echo "  4) After Fedora boots: sudo bash $0"
    echo
    echo "If no blue screen appears: enter UEFI and Disable Secure Boot, then:"
    echo "  sudo /sbin/vboxconfig"
    exit 3
  else
    echo "==> Queue MOK import (one-time). Remember the password for the blue screen."
    mokutil --import "${MOK_DIR}/MOK.der"
    echo
    echo ">>> sudo reboot → Enroll MOK → Continue → Yes → password → then re-run this script"
    exit 3
  fi
fi

echo "==> Building/loading VirtualBox kernel modules"
/sbin/vboxconfig || true
systemctl restart vboxdrv.service || true

if lsmod | grep -q '^vboxdrv'; then
  echo
  echo "OK: vboxdrv loaded. Start VirtualBox and your Win11 VM."
  lsmod | grep vbox
  exit 0
fi

echo
echo "FAILED: vboxdrv still not loaded."
if mokutil --sb-state 2>/dev/null | grep -qi enabled; then
  echo
  echo "Secure Boot is still blocking modules."
  echo "  A) Disable Secure Boot in UEFI, then: sudo /sbin/vboxconfig"
  echo "  B) Ensure MOK enrolled (blue screen), then re-run this script"
fi

journalctl -u vboxdrv.service -n 30 --no-pager || true
dmesg | tail -30 | grep -iE 'vbox|secure|lockdown' || true
exit 1
