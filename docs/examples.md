# Python Examples

The scripts in `examples/` use only the Python standard library and communicate
with the installed driver through `/dev/cm4_spi_slave`.

Read one chunk of received SPI payload data into a file:

```bash
python3 examples/read.py --count 4096 > received.bin
```

Try a read without waiting when no data is available:

```bash
python3 examples/read.py --nonblocking
```

Wait up to five seconds for received data:

```bash
python3 examples/read.py --timeout-ms 5000 > received.bin
```

Publish a text response:

```bash
python3 examples/write.py --text 'hello from the slave'
```

Publish a binary file as one response:

```bash
python3 examples/write.py --file response.bin
```

The write example keeps its descriptor open so the response remains armed.
Stop it with `Ctrl-C` after the master reads the response, or use a fixed hold
time such as `--hold-ms 5000`.

Wait for received data with `poll()`:

```bash
python3 examples/poll.py --events read --timeout-ms 5000
```

Check whether response setup is writable now:

```bash
python3 examples/poll.py --events write --timeout-ms 0
```

Use `--device PATH` with any example to select a numbered device instance.
