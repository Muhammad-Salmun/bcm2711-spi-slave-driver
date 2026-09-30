CC ?= cc
CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -Werror -std=c11

all: spi_slave_test

spi_slave_test: spi_slave_test.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f spi_slave_test

.PHONY: all clean
