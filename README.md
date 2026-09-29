# BCM2711 SPI Slave Driver

An experimental Linux driver that lets a Raspberry Pi Compute Module 4 act as
an SPI slave.

The driver creates:

```text
/dev/cm4_spi_slave
```

- Read from the device to receive bytes from the SPI master.
- Write to the device to prepare a response for the SPI master.
- Use `poll()` or `select()` to wait for received data.

## Hardware

- Raspberry Pi Compute Module 4 with BCM2711
- 3.3 V SPI signals with a shared ground
- SPI mode 0 at 100 kHz recommended for initial testing

Pin multiplexing must be configured separately for the carrier board.

## Install

Install the matching kernel headers, then run:

```bash
sudo ./scripts/install.sh
sudo reboot
```

See the [installation guide](docs/installation.md) for requirements,
verification, kernel upgrades, removal, and guidance about keeping the source
folder.

## SPI protocol

The first byte from the master selects the direction:

- `0x00`: the master sends data to the CM4.
- `0x01`: the master reads a prepared response from the CM4.

Each application `write()` publishes one response. A later write replaces any
response that has not finished transmitting.

## Status

This driver is experimental and supports BCM2711 only. Transmission uses DMA;
reception uses a polling thread. There is no READY GPIO.

## License

GPL-2.0-only. See [LICENSE](LICENSE).
