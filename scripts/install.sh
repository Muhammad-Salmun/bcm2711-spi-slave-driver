#!/usr/bin/env bash

set -euo pipefail

MODULE_NAME="bcm2711_spi_slave"
OVERLAY_NAME="bcm2711-bsc-spi-slave"
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
KERNEL_RELEASE="$(uname -r)"
MODULE_INSTALL_DIR="/lib/modules/${KERNEL_RELEASE}/extra"
MODULE_FILE="${MODULE_INSTALL_DIR}/${MODULE_NAME}.ko"
MODULE_LOAD_FILE="/etc/modules-load.d/${MODULE_NAME}.conf"
STATE_DIR="/var/lib/bcm2711-spi-slave"
STATE_FILE="${STATE_DIR}/install.conf"

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

OVERLAY_FILE="${OVERLAY_DIR}/${OVERLAY_NAME}.dtbo"

echo "Building driver for ${KERNEL_RELEASE}..."
make -C "${PROJECT_DIR}/driver"
make -C "${PROJECT_DIR}/overlay"

BUILT_MODULE="${PROJECT_DIR}/driver/${MODULE_NAME}.ko"
BUILT_OVERLAY="${PROJECT_DIR}/overlay/${OVERLAY_NAME}.dtbo"
BUILT_VERSION="$(modinfo -F version "${BUILT_MODULE}")"
BUILT_VERMAGIC="$(modinfo -F vermagic "${BUILT_MODULE}")"

if [[ -z ${BUILT_VERSION} ]]; then
	echo "The built module does not declare a version." >&2
	exit 1
fi

if [[ ${BUILT_VERMAGIC%% *} != "${KERNEL_RELEASE}" ]]; then
	echo "The built module does not match the running kernel." >&2
	echo "Module vermagic: ${BUILT_VERMAGIC}" >&2
	exit 1
fi

install -d -m 0755 "${MODULE_INSTALL_DIR}" "${OVERLAY_DIR}" "${STATE_DIR}"

WORK_DIR="$(mktemp -d /tmp/bcm2711-spi-slave-install.XXXXXX)"
MODULE_STAGE="${MODULE_INSTALL_DIR}/.${MODULE_NAME}.ko.new.$$"
OVERLAY_STAGE="${OVERLAY_DIR}/.${OVERLAY_NAME}.dtbo.new.$$"
MODULE_EXISTED=0
OVERLAY_EXISTED=0
LOAD_FILE_EXISTED=0
STATE_FILE_EXISTED=0
COMMITTED=0

cleanup()
{
	rm -f -- "${MODULE_STAGE}" "${OVERLAY_STAGE}"
	rm -rf -- "${WORK_DIR}"
}

rollback()
{
	local status=$?
	trap - ERR INT TERM

	if [[ ${COMMITTED} -eq 0 ]]; then
		echo "Installation failed; restoring the previous installation." >&2

		if [[ ${MODULE_EXISTED} -eq 1 ]]; then
			install -m 0644 "${WORK_DIR}/module.ko" "${MODULE_FILE}" || true
		else
			rm -f -- "${MODULE_FILE}" || true
		fi

		if [[ ${OVERLAY_EXISTED} -eq 1 ]]; then
			install -m 0644 "${WORK_DIR}/overlay.dtbo" "${OVERLAY_FILE}" || true
		else
			rm -f -- "${OVERLAY_FILE}" || true
		fi

		install -m 0644 "${WORK_DIR}/boot-config" "${BOOT_CONFIG}" || true

		if [[ ${LOAD_FILE_EXISTED} -eq 1 ]]; then
			install -m 0644 "${WORK_DIR}/modules-load.conf" "${MODULE_LOAD_FILE}" || true
		else
			rm -f -- "${MODULE_LOAD_FILE}" || true
		fi

		if [[ ${STATE_FILE_EXISTED} -eq 1 ]]; then
			install -m 0644 "${WORK_DIR}/install.conf" "${STATE_FILE}" || true
		else
			rm -f -- "${STATE_FILE}" || true
		fi

		depmod -a "${KERNEL_RELEASE}" || true
	fi

	cleanup
	exit "${status}"
}

trap cleanup EXIT

install -m 0644 "${BUILT_MODULE}" "${MODULE_STAGE}"
install -m 0644 "${BUILT_OVERLAY}" "${OVERLAY_STAGE}"

if [[ $(modinfo -F version "${MODULE_STAGE}") != "${BUILT_VERSION}" ]]; then
	echo "Staged module validation failed." >&2
	false
fi

if [[ -f ${MODULE_FILE} ]]; then
	MODULE_EXISTED=1
	cp -a -- "${MODULE_FILE}" "${WORK_DIR}/module.ko"
fi
if [[ -f ${OVERLAY_FILE} ]]; then
	OVERLAY_EXISTED=1
	cp -a -- "${OVERLAY_FILE}" "${WORK_DIR}/overlay.dtbo"
fi
if [[ -f ${MODULE_LOAD_FILE} ]]; then
	LOAD_FILE_EXISTED=1
	cp -a -- "${MODULE_LOAD_FILE}" "${WORK_DIR}/modules-load.conf"
fi
if [[ -f ${STATE_FILE} ]]; then
	STATE_FILE_EXISTED=1
	cp -a -- "${STATE_FILE}" "${WORK_DIR}/install.conf"
fi
cp -a -- "${BOOT_CONFIG}" "${WORK_DIR}/boot-config"

trap rollback ERR INT TERM

echo "Installing BCM2711 SPI slave driver ${BUILT_VERSION}..."
mv -f -- "${MODULE_STAGE}" "${MODULE_FILE}"
mv -f -- "${OVERLAY_STAGE}" "${OVERLAY_FILE}"

if ! grep -Eq "^[[:space:]]*dtoverlay=${OVERLAY_NAME}([[:space:]]*(#.*)?)?$" "${BOOT_CONFIG}"; then
	printf '\n%s\n' "dtoverlay=${OVERLAY_NAME}" >> "${BOOT_CONFIG}"
fi

printf '%s\n' "${MODULE_NAME}" > "${MODULE_LOAD_FILE}"
depmod -a "${KERNEL_RELEASE}"

MODULE_SHA256="$(sha256sum "${MODULE_FILE}" | cut -d ' ' -f 1)"
OVERLAY_SHA256="$(sha256sum "${OVERLAY_FILE}" | cut -d ' ' -f 1)"
cat > "${STATE_FILE}" <<EOF
VERSION=${BUILT_VERSION}
KERNEL_RELEASE=${KERNEL_RELEASE}
MODULE_SHA256=${MODULE_SHA256}
OVERLAY_SHA256=${OVERLAY_SHA256}
EOF
chmod 0644 "${STATE_FILE}"

COMMITTED=1
trap - ERR INT TERM

LOADED_VERSION=""
if [[ -r /sys/module/${MODULE_NAME}/version ]]; then
	LOADED_VERSION="$(cat "/sys/module/${MODULE_NAME}/version")"
fi

echo "Installed version: ${BUILT_VERSION}"
if [[ -n ${LOADED_VERSION} ]]; then
	echo "Loaded version:    ${LOADED_VERSION}"
fi
echo "Reboot to activate the installed module and overlay together."
