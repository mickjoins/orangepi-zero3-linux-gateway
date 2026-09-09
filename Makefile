# Orange Pi Zero 3 Linux Gateway
#
# Host build (simulation / development):
#   make
#
# Run host simulation:
#   make run-sim
#
# Cross-compile for Orange Pi Zero 3 (aarch64):
#   make clean && make CROSS_COMPILE=aarch64-linux-gnu-
#
# Install on a target:
#   make install DESTDIR=<rootfs-or-sysroot> PREFIX=/usr

APP        := opiz3-gateway
BUILD_DIR  := build
SRC_DIR    := src
SRCS       := $(wildcard $(SRC_DIR)/*.c)
OBJS       := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

CROSS_COMPILE ?=
CC      := $(CROSS_COMPILE)gcc
STRIP   := $(CROSS_COMPILE)strip

PREFIX  ?= /usr
DESTDIR ?=

CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra -std=gnu11 -pthread -D_GNU_SOURCE -I$(SRC_DIR)
LDFLAGS += -pthread -lm

.PHONY: all clean run-sim run help install uninstall

all: $(BUILD_DIR)/$(APP)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(APP): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

run-sim: all
	./$(BUILD_DIR)/$(APP) --simulate

run: all
	./$(BUILD_DIR)/$(APP)

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BUILD_DIR)/$(APP) $(DESTDIR)$(PREFIX)/bin/$(APP)
	install -d $(DESTDIR)/etc/orangepi
	install -m 0644 config/opiz3-gateway.conf $(DESTDIR)/etc/orangepi/opiz3-gateway.conf
	install -d $(DESTDIR)/etc/systemd/system
	install -m 0644 deploy/opiz3-gateway.service $(DESTDIR)/etc/systemd/system/opiz3-gateway.service

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(APP)
	rm -f $(DESTDIR)/etc/orangepi/opiz3-gateway.conf
	rm -f $(DESTDIR)/etc/systemd/system/opiz3-gateway.service

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "make                      : host build"
	@echo "make run-sim              : build and run host simulation"
	@echo "make CROSS_COMPILE=aarch64-linux-gnu- : cross-compile for Orange Pi Zero 3"
	@echo "make install DESTDIR=\$$PWD/out PREFIX=/usr : install to staging dir"
