# BCM2711 SPI Slave Driver

An experimental Linux driver that lets a BCM2711-based Raspberry Pi 4 or
Compute Module 4 act as an SPI slave.

The driver creates:

```text
/dev/cm4_spi_slave
```

- Read from the device to receive bytes from the SPI master.
- Write to the device to prepare a response for the SPI master.
- Use `poll()` or `select()` to wait for received data.

## Hardware

- Raspberry Pi 4 or Compute Module 4 with BCM2711
- 3.3 V SPI signals with a shared ground
- BSC slave signals on GPIO8-11
- SPI mode 0 at 100 kHz recommended for initial testing

The Device Tree overlay configures GPIO8-11 for the BSC slave peripheral.
These pins are also used by SPI0, so SPI0 must not be enabled at the same time.

## Install

Install the matching kernel headers, then run:

```bash
sudo ./scripts/install.sh
sudo reboot
```

Running the installer again safely replaces the installed files. Installing a
newer release replaces the previous release; reboot afterward so the module and
overlay from the same version become active together. A failed replacement is
rolled back automatically.

See the [installation guide](docs/installation.md) for requirements,
verification, kernel upgrades, removal, and guidance about keeping the source
folder.

## SPI protocol

The first byte from the master selects the direction:

- `0x00`: the master sends data to the Raspberry Pi.
- `0x01`: the master reads a prepared response from the Raspberry Pi.

Each application `write()` publishes one response. A later write replaces any
response that has not finished transmitting.

## Status

Version `0.1.0` is experimental and supports BCM2711 only, including Raspberry
Pi 4 and Compute Module 4. Existing hardware testing has used CM4 systems.
Transmission uses DMA; reception uses a polling thread.

Each matching Device Tree node has independent driver state, DMA resources,
buffers, locks, counters, and debugfs entries. The first instance uses
`/dev/cm4_spi_slave`; additional instances use numbered device names such as
`/dev/cm4_spi_slave1`.

Development and release requirements are tracked in the
[release checklist](docs/release-checklist.md).

## License

GPL-2.0-only. See [LICENSE](LICENSE).
