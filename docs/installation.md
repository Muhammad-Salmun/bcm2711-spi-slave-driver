# Installation Guide

This guide installs the BCM2711 SPI slave driver on a Raspberry Pi Compute
Module 4.

## 1. Check the hardware

Use this driver only on a Compute Module 4 with a BCM2711. SPI signals must use
3.3 V levels, and the CM4 and SPI master must share a ground.

The driver does not configure pin multiplexing. Configure the BSC pins required
by your carrier board before testing SPI communication.

## 2. Reboot after system updates

Before installing the driver, check whether the machine needs a reboot:

```bash
test -e /var/run/reboot-required && cat /var/run/reboot-required
```

If a reboot is required, reboot before continuing:

```bash
sudo reboot
```

The installer also compares the running kernel with the kernel selected for the
next boot. It stops without installing anything if they are different.

## 3. Install build requirements

On Ubuntu, install the compiler, Device Tree compiler, and headers for the
running kernel:

```bash
sudo apt update
sudo apt install build-essential device-tree-compiler linux-headers-$(uname -r)
```

The kernel headers must match the output of `uname -r` exactly.

## 4. Install the driver

Open a terminal in the downloaded repository and run:

```bash
sudo ./scripts/install.sh
```

The script builds and installs:

- the `bcm2711_spi_slave` kernel module;
- the `bcm2711-bsc-spi-slave` Device Tree overlay;
- the boot configuration for the overlay; and
- the configuration that loads the module during boot.

Reboot to activate the overlay:

```bash
sudo reboot
```

## 5. Verify the installation

After rebooting, check the module and device file:

```bash
lsmod | grep bcm2711_spi_slave
ls -l /dev/cm4_spi_slave
```

Check the Device Tree overlay:

```bash
test -d /sys/firmware/devicetree/base/soc/spi-slave@7e214000 \
  && echo "SPI slave overlay is active"
```

If `/dev/cm4_spi_slave` is missing, inspect the boot log:

```bash
sudo dmesg | grep -i -E 'bcm2711|spi slave|cm4_spi'
```

## Keeping the downloaded folder

The downloaded repository is not needed for normal operation after a successful
installation. The module and overlay are copied into system directories, so
deleting the downloaded folder does not disable the installed driver.

Keeping the folder is still useful because it contains:

- the uninstall script;
- the source needed to rebuild the module;
- the Git history; and
- any local changes made during testing.

If the folder is deleted, download the same release again before uninstalling
or rebuilding the driver.

This driver does not currently use DKMS. After a kernel upgrade, download or
open the source folder, install the matching kernel headers, and run
`install.sh` again.

## Uninstall

From the repository folder, run:

```bash
sudo ./scripts/uninstall.sh
sudo reboot
```

The uninstall script removes the module, overlay, automatic module-loading
configuration, and the overlay entry from the boot configuration.

