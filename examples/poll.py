#!/usr/bin/env python3
"""Wait for readable or writable events from the SPI slave driver."""

import argparse
import os
import select


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="/dev/bcm2711_spi_slave")
    parser.add_argument(
        "--events",
        choices=("read", "write", "both"),
        default="both",
    )
    parser.add_argument("--timeout-ms", type=int, default=-1)
    return parser.parse_args()


def main():
    args = parse_args()
    if args.timeout_ms < -1:
        raise SystemExit("--timeout-ms must be -1 or greater")

    flags = os.O_RDWR
    requested = select.POLLIN | select.POLLOUT
    if args.events == "read":
        flags = os.O_RDONLY | os.O_NONBLOCK
        requested = select.POLLIN
    elif args.events == "write":
        flags = os.O_WRONLY | os.O_NONBLOCK
        requested = select.POLLOUT

    try:
        fd = os.open(args.device, flags)
    except OSError as error:
        raise SystemExit(f"cannot open {args.device}: {error}") from error

    try:
        poller = select.poll()
        poller.register(fd, requested | select.POLLERR | select.POLLHUP)
        events = poller.poll(args.timeout_ms)
        if not events:
            print("timeout")
            return

        event = events[0][1]
        names = []
        for flag, name in (
            (select.POLLIN, "POLLIN"),
            (select.POLLOUT, "POLLOUT"),
            (select.POLLERR, "POLLERR"),
            (select.POLLHUP, "POLLHUP"),
        ):
            if event & flag:
                names.append(name)
        print(" ".join(names) if names else f"unknown event 0x{event:x}")
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
