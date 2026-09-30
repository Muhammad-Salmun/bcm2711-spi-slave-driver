#!/usr/bin/env python3
"""Read one chunk of received SPI payload data."""

import argparse
import os
import select
import sys


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="/dev/cm4_spi_slave")
    parser.add_argument("--count", type=int, default=4096)
    parser.add_argument("--nonblocking", action="store_true")
    parser.add_argument(
        "--timeout-ms",
        type=int,
        help="wait this long for data; omit to wait indefinitely",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    if args.count <= 0:
        raise SystemExit("--count must be greater than zero")
    if args.timeout_ms is not None and args.timeout_ms < 0:
        raise SystemExit("--timeout-ms must be zero or greater")

    flags = os.O_RDONLY
    if args.nonblocking or args.timeout_ms is not None:
        flags |= os.O_NONBLOCK

    try:
        fd = os.open(args.device, flags)
    except OSError as error:
        raise SystemExit(f"cannot open {args.device}: {error}") from error

    try:
        if args.timeout_ms is not None:
            poller = select.poll()
            poller.register(fd, select.POLLIN | select.POLLERR | select.POLLHUP)
            events = poller.poll(args.timeout_ms)
            if not events:
                raise SystemExit("timed out waiting for SPI data")
            event = events[0][1]
            if event & (select.POLLERR | select.POLLHUP):
                raise SystemExit("SPI slave device became unavailable")

        try:
            data = os.read(fd, args.count)
        except BlockingIOError as error:
            raise SystemExit("no SPI data is currently available") from error
        except OSError as error:
            raise SystemExit(f"read failed: {error}") from error

        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()
        print(f"read {len(data)} bytes", file=sys.stderr)
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
