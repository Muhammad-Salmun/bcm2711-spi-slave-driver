#!/usr/bin/env bash

set -euo pipefail

MODULE_NAME="bcm2711_spi_slave"
OVERLAY_NAME="bcm2711-bsc-spi-slave"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
KERNEL_RELEASE="$(uname -r)"
MODULE_INSTALL_DIR="/lib/modules/${KERNEL_RELEASE}/extra"
MODULE_LOAD_FILE="/etc/modules-load.d/${MODULE_NAME}.conf"

if [[ ${EUID} -ne 0 ]]; then
	echo "Run this installer as root: sudo $0" >&2
	exit 1
fi

if [[ -e /boot/vmlinuz ]]; then
	NEXT_KERNEL_IMAGE="$(readlink -f /boot/vmlinuz)"
	NEXT_KERNEL_NAME="${NEXT_KERNEL_IMAGE##*/}"
	if [[ ${NEXT_KERNEL_NAME} == vmlinuz-* ]]; then
		NEXT_KERNEL_RELEASE="${NEXT_KERNEL_NAME#vmlinuz-}"
		if [[ ${NEXT_KERNEL_RELEASE} != "${KERNEL_RELEASE}" ]]; then
			echo "A different kernel is selected for the next boot." >&2
			echo "Running kernel:   ${KERNEL_RELEASE}" >&2
			echo "Next-boot kernel: ${NEXT_KERNEL_RELEASE}" >&2
			echo "Reboot first, then run this installer again." >&2
			exit 1
		fi
	fi
fi

if [[ ! -d "/lib/modules/${KERNEL_RELEASE}/build" ]]; then
	echo "Kernel headers are missing for ${KERNEL_RELEASE}." >&2
	echo "Install the matching headers and run this script again." >&2
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

echo "Building driver for ${KERNEL_RELEASE}..."
make -C "${PROJECT_DIR}/driver"
make -C "${PROJECT_DIR}/overlay"

echo "Installing kernel module..."
install -d -m 0755 "${MODULE_INSTALL_DIR}"
install -m 0644 \
	"${PROJECT_DIR}/driver/${MODULE_NAME}.ko" \
	"${MODULE_INSTALL_DIR}/${MODULE_NAME}.ko"
depmod -a "${KERNEL_RELEASE}"

echo "Installing Device Tree overlay..."
install -d -m 0755 "${OVERLAY_DIR}"
install -m 0644 \
	"${PROJECT_DIR}/overlay/${OVERLAY_NAME}.dtbo" \
	"${OVERLAY_DIR}/${OVERLAY_NAME}.dtbo"

if ! grep -Eq "^[[:space:]]*dtoverlay=${OVERLAY_NAME}([[:space:]]*(#.*)?)?$" "${BOOT_CONFIG}"; then
	printf '\n%s\n' "dtoverlay=${OVERLAY_NAME}" >> "${BOOT_CONFIG}"
fi

printf '%s\n' "${MODULE_NAME}" > "${MODULE_LOAD_FILE}"

echo "Installation complete. Reboot to activate the overlay and load the driver."
