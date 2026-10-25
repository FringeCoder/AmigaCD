# latprobe: MEASUREMENT ONLY, see latprobe.c. Same toolchain as Main
# (setup_default_toolchain.sh); static so it runs on any MiSTer image.
BASE   ?= arm-none-linux-gnueabihf
CC      = $(BASE)-gcc
CFLAGS  = -O2 -Wall -Wextra -std=gnu99

latprobe: latprobe.c
	$(CC) $(CFLAGS) -static -o $@ $<

clean:
	rm -f latprobe

.PHONY: clean
