# Makefile — libaoa_hid
#
# This file is only for contributors running syntax checks.
# End users do not need it — see the build instructions in README.md.
#
# Usage:
#   make check          # syntax-check libaoa_hid.c (normal + silent mode)
#   make check-example  # syntax-check example/touch_example.c as well
#   make all            # both of the above

CC      ?= gcc
CFLAGS  := -Wall -Wextra -std=c11 -fsyntax-only \
           -I./libusb/linux_x64/include/libusb-1.0

.PHONY: all check check-example

all: check check-example

check:
	$(CC) $(CFLAGS) libaoa_hid.c
	$(CC) $(CFLAGS) -DAOA_HID_SILENT libaoa_hid.c
	@echo "check passed"

check-example:
	$(CC) $(CFLAGS) example/touch_example.c
	@echo "check-example passed"
