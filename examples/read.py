#!/usr/bin/env python3
"""Continuously read bytes of received SPI payload data."""

import os

DEVICE = "/dev/bcm2711_spi_slave"
CHUNK_SIZE = 1024


def main():
    try:
        fd = os.open(DEVICE, os.O_RDONLY)
    except OSError as error:
        raise SystemExit(f"cannot open {DEVICE}: {error}") from error

    try:
        print(f"Reading from {DEVICE} in chunks of up to {CHUNK_SIZE} bytes")
        while True:
            data = os.read(fd, CHUNK_SIZE)
            if not data:
                break
            print(f"Received chunk: {len(data)} bytes: {data!r}")
    except KeyboardInterrupt:
        print("\nStopped")
    except OSError as error:
        raise SystemExit(f"read failed: {error}") from error
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
