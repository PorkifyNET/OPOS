# Builds OPOS applications with any x86-64 GCC or Clang on Linux (or WSL).
# No OPOS source tree is needed: copy this sdk/ folder anywhere.
#
#   make                 builds build/libopos.a and the examples
#   make APP=myapp       builds build/myapp from myapp.c in this folder
#
# Copy the result to an OPOS disk (for example with mtools: mcopy) and run
# it from the Terminal with "run /disk/hello", or double-click it in Files.
#
# A program links build/crt0.o, its own objects and build/libopos.a (the
# small C library, text drawing and the system font).

CC ?= cc
AR ?= ar
BUILD := build

CFLAGS := -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-builtin \
	-fno-stack-protector -fno-pie -fno-pic -fno-asynchronous-unwind-tables \
	-mno-red-zone -Iinclude -Ibearssl/inc
LDFLAGS := -nostdlib -static -no-pie -Wl,--build-id=none \
	-Wl,-z,noexecstack -Wl,-z,max-page-size=0x1000 -Wl,-T,opos.ld

EXAMPLES := hello permtest sketch notepad fetch update
LIB_SOURCES := $(wildcard lib/*.c)
LIB_OBJECTS := $(patsubst lib/%.c,$(BUILD)/lib/%.o,$(LIB_SOURCES))
HEADERS := $(wildcard include/*.h)
LIBRARY := $(BUILD)/libopos.a

# BearSSL (bearssl/, MIT licence) for opos_tls.h. Linked into a program
# only when it uses TLS.
BEARSSL := $(BUILD)/libbearssl.a
BEARSSL_SOURCES := $(shell find bearssl/src -name '*.c')
BEARSSL_OBJECTS := $(patsubst bearssl/src/%.c,$(BUILD)/bearssl/%.o,$(BEARSSL_SOURCES))
BEARSSL_CFLAGS := $(filter-out -Wall -Wextra,$(CFLAGS)) -Ibearssl/src 	-DBR_USE_UNIX_TIME=0 -DBR_USE_URANDOM=0 -DBR_USE_GETENTROPY=0 	-DBR_USE_WIN32_RAND=0 -DBR_USE_WIN32_TIME=0 -DBR_RDRAND=0 	-DBR_AES_X86NI=0 -DBR_SSE2=0 -DBR_POWER8=0
LIBRARIES := -Wl,--start-group $(LIBRARY) $(BEARSSL) -Wl,--end-group

.PHONY: all clean
all: $(LIBRARY) $(BEARSSL) $(addprefix $(BUILD)/,$(EXAMPLES)) $(if $(APP),$(BUILD)/$(APP))

$(BUILD)/crt0.o: src/crt0.S
	mkdir -p $(BUILD)
	$(CC) -c $< -o $@

$(BUILD)/lib/%.o: lib/%.c $(HEADERS)
	mkdir -p $(BUILD)/lib
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBRARY): $(LIB_OBJECTS)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/bearssl/%.o: bearssl/src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@

$(BEARSSL): $(BEARSSL_OBJECTS)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/%: examples/%.c $(BUILD)/crt0.o $(LIBRARY) $(BEARSSL) $(HEADERS) opos.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BUILD)/crt0.o $< $(LIBRARIES)

$(BUILD)/%: %.c $(BUILD)/crt0.o $(LIBRARY) $(BEARSSL) $(HEADERS) opos.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BUILD)/crt0.o $< $(LIBRARIES)

clean:
	rm -rf $(BUILD)
