DTC ?= dtc

all: bcm2711-bsc-spi-slave.dtbo

bcm2711-bsc-spi-slave.dtbo: bcm2711-bsc-spi-slave-overlay.dts
	$(DTC) -@ -I dts -O dtb -o $@ $<

clean:
	rm -f bcm2711-bsc-spi-slave.dtbo

.PHONY: all clean
