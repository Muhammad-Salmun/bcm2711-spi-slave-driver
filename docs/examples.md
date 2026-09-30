# Python Examples

The scripts in `examples/` use only the Python standard library and communicate
with the installed driver through `/dev/cm4_spi_slave`.

Continuously read received SPI payload data:

```bash
python3 examples/read.py
```

The script requests up to 1024 bytes per blocking `read()`, prints the actual
number of bytes returned, and waits for the next chunk. A returned chunk can be
as small as one byte. Stop it with `Ctrl-C`.

Publish a text response:

```bash
python3 examples/write.py --text 'hello from the slave'
```

Publish a binary file as one response:

```bash
python3 examples/write.py --file response.bin
```

The write example keeps its descriptor open so the response remains armed.
Stop it with `Ctrl-C` after the master reads the response.

Wait for received data with `poll()`:

```bash
python3 examples/poll.py --events read --timeout-ms 5000
```

Check whether response setup is writable now:

```bash
python3 examples/poll.py --events write --timeout-ms 0
```

Use `--device PATH` with any example to select a numbered device instance.
