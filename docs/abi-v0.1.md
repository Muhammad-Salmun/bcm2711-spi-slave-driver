# Userspace ABI v0.1

This document defines the userspace ABI for experimental driver version
`0.1.x`. Programs should use the character device only. Debugfs files and
module parameters are diagnostic interfaces and are not part of this ABI.

## Device

The first bound device is `/dev/bcm2711_spi_slave`. The hardware-based name is
the same on Raspberry Pi 4 and Compute Module 4.

If more than one matching Device Tree node is present, later instances are
named `/dev/bcm2711_spi_slave1`, `/dev/bcm2711_spi_slave2`, and so on.
Numbered names depend on probe order and must not be treated as persistent
hardware IDs.

The device supports `O_RDONLY`, `O_WRONLY`, and `O_RDWR`. `O_NONBLOCK` changes
the behavior described below. There are no ioctls in ABI v0.1.

## SPI transactions

The SPI master starts every transaction with one direction byte:

- `0x00`: master writes payload bytes to the Raspberry Pi.
- `0x01`: master reads a response from the Raspberry Pi.

The BSC hardware consumes the direction byte. Userspace reads only payload
bytes following `0x00`; the direction byte is not returned by `read()`.
Transfers use SPI mode 0. The character device presents byte streams and does
not preserve chip-select or transaction boundaries.

## Reading

`read(fd, buffer, count)` has these rules:

- A zero-length read returns `0`.
- If at least one received byte is queued, the call returns immediately with
  between one and `count` bytes.
- A blocking read waits until at least one byte is available.
- A nonblocking read with no queued data fails with `EAGAIN`.
- A wait interrupted by a signal fails with `EINTR` in userspace.
- Removal causes blocked readers to wake and fail with `ENODEV`.
- An invalid userspace buffer fails with `EFAULT`; if some bytes were already
  copied, the call returns that partial byte count instead.

All readers consume one shared receive stream. Concurrent reads do not receive
copies of the same data.

The receive queue holds 65,536 bytes. When it is full, each new byte discards
the oldest queued byte. Overflow is not reported through `read()`; the
diagnostic `rx_overruns` counter records it.

## Writing

Each successful `write(fd, buffer, count)` publishes one complete response for
a later `0x01` transaction:

- A zero-length write returns `0` and does not change the response.
- A successful write returns exactly `count`.
- A new successful write replaces any previous response, including a response
  that the master has only partly clocked out.
- Concurrent writes are serialized. In nonblocking mode, a busy response
  setup path fails with `EAGAIN`.
- A blocking write interrupted while waiting for response setup fails with
  `EINTR`.
- Removal causes writes to fail with `ENODEV`.
- An invalid userspace buffer fails with `EFAULT`.
- DMA-buffer allocation failure returns `ENOMEM`.
- A size arithmetic overflow returns `EOVERFLOW`.
- DMA descriptor preparation or submission failure returns `EIO` or the
  negative error supplied by the DMA engine.

There is no fixed 64 KiB transmit limit. A response requires a coherent DMA
allocation of four bytes for every payload byte. Available coherent memory and
DMA-engine limits therefore determine the practical maximum. Applications
must handle `ENOMEM` and DMA setup errors for large writes.

A successful return means the response was copied and submitted to DMA. It
does not mean that the SPI master has clocked out any or all of the response.
ABI v0.1 provides no transmit-completion notification.

The response remains available while at least one writable file descriptor is
open. Closing the last `O_WRONLY` or `O_RDWR` descriptor cancels DMA, frees the
response, and clears the transmit path. Closing a read-only descriptor does
not affect it.

## Polling

`poll()`, `select()`, and equivalent APIs report:

- `POLLIN | POLLRDNORM` when at least one received byte can be read without
  blocking.
- `POLLOUT | POLLWRNORM` on a writable descriptor when response setup can
  begin immediately.
- `POLLERR | POLLHUP` when device removal starts.

Writable readiness is a snapshot. Another writer may claim the response setup
path before the caller writes, so nonblocking code must still handle `EAGAIN`.

## Compatibility

Patch releases in the `0.1.x` series will preserve the behavior documented
here. Any incompatible experimental ABI change requires a new ABI document and
a new minor version. Diagnostic debugfs layout, statistics, kernel log text,
and module parameters may change without an ABI version change.
