# BCM2711 SPI Slave

Experimental Linux character driver for operating the BCM2711 BSC peripheral
as an SPI slave on Raspberry Pi Compute Module 4 hardware.

> [!WARNING]
> This is pre-release hardware-specific software. It currently uses a fixed
> BCM2711 peripheral address and cooperative polling. Do not load it while
> another driver owns the BSC peripheral.

## Current interface

The module creates `/dev/cm4_spi_slave` with mode `0660`:

- `read()` receives bytes clocked into the BSC RX FIFO.
- `write()` queues bytes for transmission from the BSC TX FIFO.
- blocking and nonblocking I/O are supported.
- `poll()`/`select()` report both readable and writable state.

Diagnostic information is available under
`/sys/kernel/debug/cm4_spi_slave/`. Reading `regs` intentionally omits the data
register because reading that register consumes RX data. `reset_stats` resets
counters only; it does not flush buffered traffic.

## Build

Install the headers for the running kernel, then run:

```bash
make -C driver
make -C overlay
```

Install the Device Tree overlay and enable it for the next boot:

```bash
overlay_dir=/boot/firmware/overlays
if grep -q '^os_prefix=current/' /boot/firmware/config.txt; then
  overlay_dir=/boot/firmware/current/overlays
fi
sudo install -m 0644 overlay/bcm2711-bsc-spi-slave.dtbo "$overlay_dir/"
printf '%s\n' 'dtoverlay=bcm2711-bsc-spi-slave' | \
  sudo tee -a /boot/firmware/config.txt
sudo reboot
```

The overlay creates a platform-device node for the BSC/SPI-slave block and
assigns BCM2711 DMA request 8 to `tx` and request 9 to `rx`. The driver binds
to that node, maps its register resource, and acquires the TX channel with
`dma_request_chan(dev, "tx")`.

Writes use DREQ-paced DMA. Each DMA byte is expanded into the low byte of a
32-bit word because the BCM2835 Linux DMA engine requires a four-byte
peripheral access width. Each userspace write publishes one complete frame.
Before loading it, the driver terminates the previous descriptor and clears
the BSC TX path; this is what makes a retry begin again at the first byte.
The write returns after issuing the DMA descriptor and does not wait for DMA
completion or for MISO to finish shifting.

The driver does not reset the TX path on a chip-select edge. The BSC FIFO and
the active DREQ-paced DMA descriptor therefore preserve their position while
the master clocks one published frame over multiple transmit-mode
transactions. In each transaction the BSC consumes direction `0x01`; the byte
returned concurrently is discarded by the master, and subsequent dummy bytes
clock FIFO data. In receive mode it consumes direction `0x00`, and only the
following status and sequence bytes reach the driver's RX ring. Transmit-mode
dummy `0xff` bytes do not enter that ring. RX FIFO servicing remains
polling-based. The `tx_pio_limit` module parameter can prime a CPU-written
prefix for diagnostics; its default is zero.

No READY GPIO is used by the driver. The master must allow enough time after
the sender's `write()` for DMA to prime the BSC FIFO before it begins response
clocks. The driver cannot enforce SPI mode or clock rate; those are master
settings. This stack is intended for mode 0 at 100000 Hz.

After reboot, verify the live tree before changing the driver:

```bash
node=/sys/firmware/devicetree/base/soc/spi-slave@7e214000
test -d "$node"
tr -d '\0' < "$node/compatible"
od -An -tx1 "$node/dmas"
```

Load and unload the resulting module with:

```bash
sudo insmod driver/bcm2711_spi_slave.ko
sudo rmmod bcm2711_spi_slave
```

The polling interval defaults to 20 microseconds and can be selected when the
module is loaded:

```bash
sudo insmod driver/bcm2711_spi_slave.ko poll_interval_us=20
```

## Hardware assumptions

The overlay describes the BSC register block using its `0x7E214000`
peripheral-bus address. The BCM2711 `/soc` ranges translate that to the
`0xFE214000` CPU physical address. Both CPU MMIO and BSC-slave DMA use the
translated address; a DMA-only FIFO probe verified that `0xFE214000` clears
`FR_TXFE`, whereas DMA writes to the untranslated alias do not enter the FIFO.
The driver programs the control register to `0x303`. Pin multiplexing must
currently be configured separately.

## Development status

The driver is Device Tree-backed. TX is DMA-driven and RX is serviced by a
cooperative polling thread. The DMA path must be validated first with a short,
known byte pattern at a low master clock rate before image transfers resume.
