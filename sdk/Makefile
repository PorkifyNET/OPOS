# Builds OPOS applications with any x86-64 GCC or Clang on Linux (or WSL).
# No OPOS source tree is needed: copy this sdk/ folder anywhere.
#
#   make                 builds build/hello and build/permtest
#   make APP=myapp       builds build/myapp from myapp.c in this folder
#
# Copy the result to an OPOS disk (for example with mtools: mcopy) and run
# it from the Terminal with "run /disk/hello", or double-click it in Files.

CC ?= cc
BUILD := build

CFLAGS := -std=c11 -O2 -Wall -Wextra -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-pie -fno-pic -fno-asynchronous-unwind-tables \
	-mno-red-zone -Iinclude
LDFLAGS := -nostdlib -static -no-pie -Wl,--build-id=none \
	-Wl,-z,noexecstack -Wl,-z,max-page-size=0x1000 -Wl,-T,opos.ld

EXAMPLES := hello permtest

.PHONY: all clean
all: $(addprefix $(BUILD)/,$(EXAMPLES)) $(if $(APP),$(BUILD)/$(APP))

$(BUILD)/crt0.o: src/crt0.S
	mkdir -p $(BUILD)
	$(CC) -c $< -o $@

$(BUILD)/%: examples/%.c $(BUILD)/crt0.o include/opos.h opos.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BUILD)/crt0.o $<

$(BUILD)/%: %.c $(BUILD)/crt0.o include/opos.h opos.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BUILD)/crt0.o $<

clean:
	rm -rf $(BUILD)
