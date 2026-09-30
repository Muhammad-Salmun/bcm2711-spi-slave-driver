#!/usr/bin/env bash

set -euo pipefail

MODULE_NAME="bcm2711_spi_slave"
OVERLAY_NAME="bcm2711-bsc-spi-slave"
DEVICE_PATH="/dev/bcm2711_spi_slave"
KERNEL_RELEASE="$(uname -r)"
MODULE_LOAD_FILE="/etc/modules-load.d/${MODULE_NAME}.conf"
STATE_DIR="/var/lib/bcm2711-spi-slave"
STATE_FILE="${STATE_DIR}/install.conf"

if [[ ${EUID} -ne 0 ]]; then
	echo "Run this uninstaller as root: sudo $0" >&2
	exit 1
fi

INSTALLED_KERNEL="${KERNEL_RELEASE}"
if [[ -r ${STATE_FILE} ]]; then
	RECORDED_KERNEL="$(sed -n 's/^KERNEL_RELEASE=//p' "${STATE_FILE}")"
	if [[ ${RECORDED_KERNEL} =~ ^[A-Za-z0-9._+-]+$ ]]; then
		INSTALLED_KERNEL="${RECORDED_KERNEL}"
	fi
fi
MODULE_FILE="/lib/modules/${INSTALLED_KERNEL}/extra/${MODULE_NAME}.ko"

if [[ -e ${DEVICE_PATH} ]] && command -v fuser >/dev/null 2>&1; then
	OPEN_PIDS="$(fuser "${DEVICE_PATH}" 2>/dev/null || true)"
	if [[ -n ${OPEN_PIDS//[[:space:]]/} ]]; then
		echo "Cannot uninstall: ${DEVICE_PATH} is in use." >&2
		echo "Processes:${OPEN_PIDS}" >&2
		echo "Stop those applications and run the uninstaller again." >&2
		exit 1
	fi
fi

if [[ -f /boot/firmware/config.txt ]]; then
	BOOT_CONFIG=/boot/firmware/config.txt
	OVERLAY_DIR=/boot/firmware/overlays
	if grep -q '^[[:space:]]*os_prefix=current/' "${BOOT_CONFIG}"; then
		OVERLAY_DIR=/boot/firmware/current/overlays
	fi
elif [[ -f /boot/config.txt ]]; then
	BOOT_CONFIG=/boot/config.txt
	OVERLAY_DIR=/boot/overlays
else
	echo "Could not find /boot/firmware/config.txt or /boot/config.txt." >&2
	exit 1
fi

if grep -q "^${MODULE_NAME}[[:space:]]" /proc/modules; then
	echo "Unloading kernel module..."
	modprobe -r "${MODULE_NAME}"
fi

echo "Removing installed files..."
rm -f -- "${MODULE_FILE}"
rm -f -- "${MODULE_LOAD_FILE}"
rm -f -- "${OVERLAY_DIR}/${OVERLAY_NAME}.dtbo"
rm -f -- "${STATE_FILE}"
rmdir --ignore-fail-on-non-empty "${STATE_DIR}" 2>/dev/null || true

sed -i \
	"/^[[:space:]]*dtoverlay=${OVERLAY_NAME}[[:space:]]*\(#.*\)\?$/d" \
	"${BOOT_CONFIG}"

if [[ -d /lib/modules/${INSTALLED_KERNEL} ]]; then
	depmod -a "${INSTALLED_KERNEL}"
fi

echo "Uninstallation complete. Reboot to deactivate the overlay."
