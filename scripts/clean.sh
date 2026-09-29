#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
DRIVER_DIR="${PROJECT_DIR}/driver"
OVERLAY_DIR="${PROJECT_DIR}/overlay"
KERNEL_BUILD="/lib/modules/$(uname -r)/build"

if [[ -d ${KERNEL_BUILD} ]]; then
	make -C "${DRIVER_DIR}" KDIR="${KERNEL_BUILD}" clean
else
	echo "Current kernel headers are unavailable; removing known build files directly."
	find "${DRIVER_DIR}" -maxdepth 1 -type f \
		\( -name '*.o' -o -name '*.ko' -o -name '*.mod' \
		-o -name '*.mod.c' -o -name '*.cmd' -o -name 'Module.symvers' \
		-o -name 'modules.order' \) -delete
	rm -rf -- "${DRIVER_DIR}/.tmp_versions"
fi

make -C "${OVERLAY_DIR}" clean

echo "Build artifacts removed. Installed driver files were not changed."

