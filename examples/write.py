#!/usr/bin/env python3
"""Publish one text or binary response for the SPI master."""

import argparse
import os
import signal


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="/dev/bcm2711_spi_slave")
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--text", help="UTF-8 text response")
    source.add_argument("--file", help="binary response file")

    return parser.parse_args()


def main():
    args = parse_args()
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
        print("response armed; press Ctrl-C after the master reads it")
        signal.pause()
    except KeyboardInterrupt:
        pass
    finally:
        os.close(fd)


if __name__ == "__main__":
    main()
