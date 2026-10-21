#!/usr/bin/env bash
# ==============================================================================
# AIOS Raspberry Pi 3 Network Boot (TFTP / ProxyDHCP) Server
#
# Lets the Pi pick up a new kernel on every power-cycle with NO SD card swap,
# once the one-time OTP netboot-enable has been done (flash-rpi3.sh writes
# program_usb_boot_mode=1 to config.txt for that - it burns on the boot that
# uses that SD card, regardless of which kernel it loads).
#
# The original Pi 3B's boot ROM has real bugs around fetching bootcode.bin
# itself over the network, so this only replaces what comes AFTER
# bootcode.bin: leave a minimal SD card in permanently (bootcode.bin only,
# no kernel8.img/start.elf/config.txt) so the ROM's local bootcode.bin then
# falls through to fetching start.elf, fixup.dat, config.txt and kernel8.img
# from THIS server over TFTP - that is the actual code-update path from here
# on. See the aios-metal-arm64-netboot memory entry for the full story.
# ==============================================================================
set -euo pipefail

#
#   metal/serve-netboot.sh             build, stage the TFTP root, serve
#   metal/serve-netboot.sh --dry-run   print what it would stage and run; nothing changes
#   metal/serve-netboot.sh --unit      render metal/aios-netboot.service.in for this site
#
# The checkout can be called anything and live anywhere: every path is taken
# from where this script is (it used to take the checkout's PARENT and append
# "aios/", so a checkout with any other name staged into a directory that did
# not exist).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/metal-arm64"
FW_DIR="${SCRIPT_DIR}/rpi-firmware"
TFTP_DIR="${AIOS_NETBOOT_TFTP:-${REPO_ROOT}/build/tftpboot}"
IFACE="${AIOS_NETBOOT_IFACE:-br0}"
MODE="${1:-serve}"
# Anything but the three modes is a mistake, not a request to serve: a typo
# (or `--help`) used to stage and restart the live unit.
case "$MODE" in
  serve|--dry-run|--unit) ;;
  *) echo "usage: metal/serve-netboot.sh [--dry-run|--unit]   (no argument: build, stage the TFTP root and serve)" >&2; exit 2;;
esac

if [ "$MODE" = "--unit" ]; then
  # Site values for the persistent unit. None has a default that could be
  # right somewhere else, so a missing one is an error, not a guess.
  missing=()
  for v in AIOS_NETBOOT_PI_MAC AIOS_NETBOOT_PI_IP AIOS_NETBOOT_GATEWAY; do [ -n "${!v:-}" ] || missing+=("$v"); done
  if [ ${#missing[@]} -gt 0 ]; then
    echo "[!] set ${missing[*]} (the Pi's MAC, the address to give it, the LAN gateway)." >&2
    echo "    optional: AIOS_NETBOOT_IFACE (br0) AIOS_NETBOOT_TFTP (${REPO_ROOT}/build/tftpboot)" >&2
    echo "              AIOS_NETBOOT_NETMASK (255.255.255.0) AIOS_NETBOOT_DNS (1.1.1.1,8.8.8.8) AIOS_NETBOOT_USER ($(id -un))" >&2
    exit 1
  fi
  pi_ip="${AIOS_NETBOOT_PI_IP}"
  sed -e "s|@IFACE@|${IFACE}|g" -e "s|@TFTP_ROOT@|${TFTP_DIR}|g" -e "s|@REPO_ROOT@|${REPO_ROOT}|g" \
      -e "s|@SUBNET@|${AIOS_NETBOOT_SUBNET:-${pi_ip%.*}.0}|g" -e "s|@NETMASK@|${AIOS_NETBOOT_NETMASK:-255.255.255.0}|g" \
      -e "s|@PI_MAC@|${AIOS_NETBOOT_PI_MAC}|g" -e "s|@PI_IP@|${pi_ip}|g" -e "s|@GATEWAY@|${AIOS_NETBOOT_GATEWAY}|g" \
      -e "s|@DNS@|${AIOS_NETBOOT_DNS:-1.1.1.1,8.8.8.8}|g" -e "s|@USER@|${AIOS_NETBOOT_USER:-$(id -un)}|g" \
      "${SCRIPT_DIR}/aios-netboot.service.in"
  exit 0
fi
if [ "$MODE" = "--dry-run" ]; then
  echo "repo:       ${REPO_ROOT}"
  echo "kernel:     ${BUILD_DIR}/aios-metal-arm64.bin $([ -f "${BUILD_DIR}/aios-metal-arm64.bin" ] && echo '(built)' || echo '(not built: a real run builds it)')"
  echo "tftp root:  ${TFTP_DIR}"
  echo "stages:     kernel8.img bootcode.bin start.elf fixup.dat config.txt$([ -f "${SCRIPT_DIR}/arm64/wifi.cfg" ] && echo ' wifi.cfg')"
  for f in bootcode.bin start.elf fixup.dat; do [ -f "${FW_DIR}/${f}" ] || echo "would fetch: ${FW_DIR}/${f} from raspberrypi/firmware"; done
  echo "interface:  ${IFACE} ($(ip -4 -o addr show "${IFACE}" 2>/dev/null | awk '{print $4}' | head -1 || true))"
  if systemctl cat aios-netboot.service >/dev/null 2>&1; then echo "would run:  sudo systemctl restart aios-netboot.service"
  else echo "would run:  sudo dnsmasq --interface=${IFACE} --enable-tftp --tftp-root=${TFTP_DIR} ... (ProxyDHCP)"; fi
  exit 0
fi

mkdir -p "${TFTP_DIR}"

echo "[*] Ensuring kernel is built..."
bash "${SCRIPT_DIR}/build-arm64.sh"

echo "[*] Staging TFTP boot directory at ${TFTP_DIR}..."
cp "${BUILD_DIR}/aios-metal-arm64.bin" "${TFTP_DIR}/kernel8.img"

# The GPU firmware blobs. bootcode.bin does not actually get fetched over
# TFTP on this model (ROM bug - it is read from the local SD card instead),
# but it costs nothing to have it here too in case a future board revision's
# ROM does ask for it. start.elf and fixup.dat ARE fetched over TFTP and are
# required - their absence here is exactly why the previous version of this
# script never actually worked.
for f in bootcode.bin start.elf fixup.dat; do
  if [ ! -f "${FW_DIR}/${f}" ]; then
    echo "[!] Missing ${FW_DIR}/${f} - fetching from raspberrypi/firmware..."
    curl -sL --max-time 30 -o "${FW_DIR}/${f}" \
      "https://raw.githubusercontent.com/raspberrypi/firmware/master/boot/${f}"
  fi
  cp "${FW_DIR}/${f}" "${TFTP_DIR}/${f}"
done

cat > "${TFTP_DIR}/config.txt" << 'EOF'
# Raspberry Pi 3 Bare-Metal AIOS Configuration (served over TFTP)
arm_64bit=1
kernel=kernel8.img
enable_uart=1
uart_2ndstage=1
core_freq=250
hdmi_force_hotplug=1
disable_overscan=1
# No framebuffer_width/height: the kernel asks the firmware for the
# monitor's native mode and renders at it (a fixed 1280x720 was upscaled
# by the GPU on 1080p screens and made all text soft).
framebuffer_depth=32
framebuffer_ignore_alpha=1
disable_commandline_tags=1
EOF

if [ -f "${SCRIPT_DIR}/arm64/wifi.cfg" ]; then
    cp "${SCRIPT_DIR}/arm64/wifi.cfg" "${TFTP_DIR}/wifi.cfg"
fi

HOST_IP=$(ip -4 -o addr show "${IFACE}" 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1)
if [ -z "${HOST_IP}" ]; then
  echo "[!] Could not determine IPv4 address on interface ${IFACE} - is it up?"
  exit 1
fi

echo "[+] TFTP boot directory ready on host ${HOST_IP} (interface ${IFACE}):"
ls -lh "${TFTP_DIR}"/kernel8.img "${TFTP_DIR}"/start.elf "${TFTP_DIR}"/fixup.dat "${TFTP_DIR}"/config.txt

echo "[*] Launching dnsmasq with TFTP + ProxyDHCP on interface ${IFACE}..."
echo "[+] TFTP root: ${TFTP_DIR}"
echo "[!] The Pi only netboots if its SD card is bootcode.bin-only"
echo "    (metal/strip-sd-to-netboot.sh --yes). With a full card it boots from"
echo "    the SD and never asks this server for anything. No OTP fuse needed."
echo "-------------------------------------------------------------------------------"

# The server itself is the aios-netboot systemd unit (enabled, survives
# reboots). Running a second dnsmasq here would fight it for ports 67/69.
if systemctl cat aios-netboot.service >/dev/null 2>&1; then
  sudo systemctl restart aios-netboot.service
  systemctl --no-pager --lines=0 status aios-netboot.service | head -3
  echo "[+] aios-netboot restarted; logs: journalctl -u aios-netboot -f"
  exit 0
fi

if ! command -v dnsmasq >/dev/null 2>&1; then
  echo "[!] dnsmasq not found - required for ProxyDHCP + TFTP. Install it (e.g. pacman -S dnsmasq)."
  exit 1
fi

exec sudo dnsmasq \
    --no-daemon \
    --interface="${IFACE}" \
    --bind-dynamic \
    --port=0 \
    --enable-tftp \
    --tftp-root="${TFTP_DIR}" \
    --dhcp-range="${HOST_IP%.*}.0,proxy" \
    --dhcp-boot=bootcode.bin \
    --pxe-service=0,"Raspberry Pi Boot" \
    --log-dhcp \
    --log-queries \
    --user="$(id -un)"
