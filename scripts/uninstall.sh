#!/usr/bin/env bash

set -euo pipefail

MODULE_NAME="bcm2711_spi_slave"
OVERLAY_NAME="bcm2711-bsc-spi-slave"
KERNEL_RELEASE="$(uname -r)"
MODULE_FILE="/lib/modules/${KERNEL_RELEASE}/extra/${MODULE_NAME}.ko"
MODULE_LOAD_FILE="/etc/modules-load.d/${MODULE_NAME}.conf"

if [[ ${EUID} -ne 0 ]]; then
	echo "Run this uninstaller as root: sudo $0" >&2
	exit 1
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

sed -i \
	"/^[[:space:]]*dtoverlay=${OVERLAY_NAME}[[:space:]]*\(#.*\)\?$/d" \
	"${BOOT_CONFIG}"

depmod -a "${KERNEL_RELEASE}"

echo "Uninstallation complete. Reboot to deactivate the overlay."

