# BCM2711 SPI Slave Driver

Linux has an SPI framework with slave-controller support, but it does not
provide a standard driver for using the BCM2711 BSC peripheral as an SPI
slave. This makes using a Raspberry Pi 4 or Compute Module 4 as an SPI slave
far more difficult than it should be.

I built this experimental driver to solve that problem.

## Quick Start

After cloning this repo, from the repo folder, run:

```bash
sudo ./scripts/install.sh
sudo reboot
```

Make sure the headers matching your running kernel are installed. See the
[installation guide](docs/installation.md) for details.
After rebooting, the driver exposes this character device:

```text
/dev/bcm2711_spi_slave
```

Applications can communicate with it from C, Python, ROS 2, or any other
language that can use normal file operations:

- `read()` receives bytes sent by the SPI master.
- `write()` prepares bytes for the SPI master to read.
- `poll()` waits efficiently until reading or writing is ready.

See the [userspace ABI](docs/abi-v0.1.md) for the exact behavior of these
operations.

## Python Examples

The [`examples`](examples) directory contains simple Python scripts for
reading, writing, and testing `poll()` after installing the driver. They use
only the Python standard library.

See the [examples guide](docs/examples.md) for commands and expected behavior.

## SPI Pins

The driver uses GPIO8-11, the same GPIO group normally used by SPI0. Because
the Raspberry Pi is acting as the slave, the signal directions are different
from its usual SPI-master role. Follow the wiring table in the
[installation guide](docs/installation.md) when connecting MOSI and MISO.

The installer adds a Device Tree overlay that configures the pin controller
automatically after reboot. SPI0 and this SPI-slave driver cannot own these
pins at the same time.

If you later want to use GPIO8-11 for SPI0 or another purpose, uninstall this
driver and reboot:

```bash
sudo ./scripts/uninstall.sh
sudo reboot
```

## Tested

I have tested the driver at SPI clock speeds up to 1 MHz and completed a
continuous 10 MB file transfer using application-level chunking and CRC
checks. Version `0.1.0` remains experimental, so test it carefully in your own
system.

## More Information

- [Installation and hardware wiring](docs/installation.md)
- [Userspace ABI v0.1](docs/abi-v0.1.md)
- [Python examples](docs/examples.md)
- [Development and release checklist](docs/release-checklist.md)

I hope this is what you were looking for. Let me know how it works in your
setup. Thanks!

## License

GPL-2.0-only. See [LICENSE](LICENSE).
