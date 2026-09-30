# Test Utility

Build the userspace test utility with:

```bash
make -C tools
```

It uses `/dev/cm4_spi_slave` by default. Select another instance by placing
`-d DEVICE` before the command.

## Receive data

Perform one blocking read and save the received bytes:

```bash
./tools/spi_slave_test read -n 4096 > received.bin
```

Perform a nonblocking read:

```bash
./tools/spi_slave_test read -N -n 4096 > received.bin
```

Wait up to five seconds for readable data before reading:

```bash
./tools/spi_slave_test read -n 4096 -t 5000 > received.bin
```

## Publish a response

Publish text for the SPI master to read:

```bash
./tools/spi_slave_test write-text 'hello from the slave'
```

Publish one binary file as a single response:

```bash
./tools/spi_slave_test write-file response.bin
```

Use `-` to read the response from standard input. The utility buffers the
complete input and makes exactly one device `write()` call because a later
write replaces the previous response. By default the utility keeps the writer
open, and therefore keeps the response armed, until it is stopped with
`Ctrl-C`. Place `-t MS` before the text or filename to close after a fixed
time, for example `write-file -t 5000 response.bin`.

## Poll

Wait for received data:

```bash
./tools/spi_slave_test poll -e read -t 5000
```

Check whether response setup is currently writable:

```bash
./tools/spi_slave_test poll -e write -t 0
```

The utility prints returned poll events and reports system-call errors with
their kernel-provided error text.
