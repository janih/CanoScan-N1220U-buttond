CC ?= cc
PKG_CONFIG ?= pkg-config

CFLAGS += -Wall -Wextra -std=c11 -O2 $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LDLIBS += $(shell $(PKG_CONFIG) --libs libusb-1.0)

BIN = canoscan-n1220u-buttond

all: $(BIN)

$(BIN): canoscan_n1220u_buttond.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(BIN)

.PHONY: all clean
