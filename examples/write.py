#!/usr/bin/env python3
"""Publish one text or binary response for the SPI master."""

import argparse
import os
import signal
import time


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="/dev/cm4_spi_slave")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--text", help="UTF-8 text response")
    source.add_argument("--file", help="binary response file")
    parser.add_argument(
        "--hold-ms",
        type=int,
        default=-1,
        help="keep response armed for this many ms; default waits for Ctrl-C",
    )
    return parser.parse_args()


def main():
    args = parse_args()
    if args.hold_ms < -1:
        raise SystemExit("--hold-ms must be -1 or greater")

    if args.text is not None:
        data = args.text.encode("utf-8")
    else:
        try:
            with open(args.file, "rb") as response_file:
                data = response_file.read()
        except OSError as error:
            raise SystemExit(f"cannot read {args.file}: {error}") from error

    try:
        fd = os.open(args.device, os.O_WRONLY)
    except OSError as error:
        raise SystemExit(f"cannot open {args.device}: {error}") from error

    try:
        try:
            written = os.write(fd, data)
        except OSError as error:
            raise SystemExit(f"write failed: {error}") from error
        if written != len(data):
            raise SystemExit(f"short write: {written} of {len(data)} bytes")

        print(f"published {written} bytes to {args.device}")
        if args.hold_ms < 0:
            print("response armed; press Ctrl-C after the master reads it")
            signal.pause()
        elif args.hold_ms > 0:
            print(f"keeping response armed for {args.hold_ms} ms")
            time.sleep(args.hold_ms / 1000)
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
