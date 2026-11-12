#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "es1371.h"
#include "io.h"
#include "memory.h"
#include "pci.h"

/*
 * Ensoniq AudioPCI 97: ES1371 and its successors CT5880 and EV1938
 * (docs/sound.md). VMware emulates it as its default sound card for many
 * guest types, so a VM plays sound without its .vmx asking for HD Audio.
 *
 * The card has I/O registers only. Its DAC2 plays a ring in memory
 * (32-bit DMA) over and over; a sample rate converter (SRC) takes it to
 * the AC'97 codec's 48 kHz. Like the HDA driver, no interrupts: the 100 Hz
 * timer reads the playing position and writes new samples behind it.
 * Register meanings follow the Linux driver (sound/pci/ens1370.c).
 */

/* I/O registers. */
#define REG_CONTROL 0x00
#define REG_STATUS 0x04
#define REG_MEM_PAGE 0x0c
#define REG_SRC 0x10
#define REG_CODEC 0x14
#define REG_LEGACY 0x18
#define REG_SERIAL 0x20
#define REG_DAC2_COUNT 0x28
/* In memory page PAGE_DAC. */
#define REG_DAC2_FRAME 0x38
#define REG_DAC2_SIZE 0x3c
#define PAGE_DAC 0x0cu

#define CONTROL_DAC2_ENABLE (1u << 5)
#define STATUS_SYNC_RESET (1u << 14)
#define STATUS_DAC2 (1u << 1)

/* SERIAL: DAC2 16-bit stereo (mode 3), end increment 2. */
#define SERIAL_P2_MODE(m) (((m) & 3u) << 2)
#define SERIAL_P2_END_INC(n) (((n) & 7u) << 19)

/* SRC interface register. */
#define SRC_RAM_ADDRESS(a) (((uint32_t)(a) & 0x7fu) << 25)
#define SRC_RAM_WRITE (1u << 24)
#define SRC_RAM_BUSY (1u << 23)
#define SRC_DISABLE (1u << 22)
#define SRC_DISABLE_P1 (1u << 21)
#define SRC_DISABLE_P2 (1u << 20)
#define SRC_DISABLE_RECORD (1u << 19)
#define SRC_KEEP (SRC_DISABLE | SRC_DISABLE_P1 | SRC_DISABLE_P2 \
                  | SRC_DISABLE_RECORD)
#define SRC_STATE_MASK 0x00870000u
#define SRC_STATE_READY 0x00010000u

/* SRC memory. */
#define SRC_DAC1 0x70u
#define SRC_DAC2 0x74u
#define SRC_VOLUME_ADC 0x6cu
#define SRC_VOLUME_DAC1 0x7cu
#define SRC_VOLUME_DAC2 0x7eu
#define SRC_TRUNC_N 0x00u
#define SRC_INT_REGS 0x01u
#define SRC_VFREQ_FRAC 0x03u

/* AC'97 codec interface. */
#define CODEC_WRITE_IN_PROGRESS (1u << 30)
#define CODEC_WRITE(r, v) ((((uint32_t)(r) & 0x7fu) << 16) | ((v) & 0xffffu))
#define AC97_RESET 0x00u
#define AC97_MASTER 0x02u
#define AC97_HEADPHONE 0x04u
#define AC97_PCM_OUT 0x18u
#define AC97_VENDOR1 0x7cu
#define AC97_VENDOR2 0x7eu

#define RATE 48000u
#define RING_BYTES 32768u
#define RING_PAGES (RING_BYTES / 4096u)
#define FRAME_BYTES 4u
#define LEAD_BYTES (RING_BYTES - 4096u)
#define POLL_COUNT 100000u

static bool ready;
static uint16_t port;
static struct pci_device card;
static uint8_t *ring;
static uint64_t played_bytes;
static uint64_t written_bytes;
static uint32_t last_position;
static void (*source)(int16_t *out, size_t frames);
static uint32_t codec_vendor;

static uint32_t in32(uint32_t reg) {
    return io_in32((uint16_t)(port + reg));
}

static void out32(uint32_t reg, uint32_t value) {
    io_out32((uint16_t)(port + reg), value);
}

static void delay_us(uint32_t microseconds) {
    /* Port 0x80 writes take about a microsecond each. */
    for (uint32_t i = 0; i < microseconds; i++) {
        io_wait();
    }
}

static void silence(int16_t *out, size_t frames) {
    for (size_t i = 0; i < frames * 2; i++) {
        out[i] = 0;
    }
}

/* Waits until the SRC is not busy; returns the register then. */
static uint32_t src_wait(void) {
    for (uint32_t i = 0; i < POLL_COUNT; i++) {
        uint32_t value = in32(REG_SRC);
        if ((value & SRC_RAM_BUSY) == 0) {
            return value;
        }
    }
    return 0;
}

static void src_write(uint32_t address, uint32_t data) {
    uint32_t value = src_wait() & SRC_KEEP;
    out32(REG_SRC, value | SRC_RAM_ADDRESS(address) | (data & 0xffffu)
                       | SRC_RAM_WRITE);
}

/* Writes an AC'97 register. The SRC must be in a safe state while the
   codec interface is used, so it is held there and then restored. */
static void codec_write(uint32_t reg, uint32_t value) {
    for (uint32_t i = 0; i < POLL_COUNT; i++) {
        if ((in32(REG_CODEC) & CODEC_WRITE_IN_PROGRESS) == 0) {
            break;
        }
    }
    uint32_t saved = src_wait();
    out32(REG_SRC, (saved & SRC_KEEP) | SRC_STATE_READY);
    for (uint32_t i = 0; i < POLL_COUNT; i++) {
        if ((in32(REG_SRC) & SRC_STATE_MASK) == 0) {
            break;
        }
    }
    for (uint32_t i = 0; i < POLL_COUNT; i++) {
        if ((in32(REG_SRC) & SRC_STATE_MASK) == SRC_STATE_READY) {
            break;
        }
    }
    out32(REG_CODEC, CODEC_WRITE(reg, value));
    (void)src_wait();
    out32(REG_SRC, saved);
}

/* Reads an AC'97 register (bit 23 asks for a read; bit 31 says the data
   is valid). Returns 0xffffffff when the codec does not answer. */
static uint32_t codec_read(uint32_t reg) {
    for (uint32_t i = 0; i < POLL_COUNT; i++) {
        if ((in32(REG_CODEC) & CODEC_WRITE_IN_PROGRESS) == 0) {
            break;
        }
    }
    uint32_t saved = src_wait();
    out32(REG_SRC, (saved & SRC_KEEP) | SRC_STATE_READY);
    for (uint32_t i = 0; i < POLL_COUNT; i++) {
        if ((in32(REG_SRC) & SRC_STATE_MASK) == 0) {
            break;
        }
    }
    for (uint32_t i = 0; i < POLL_COUNT; i++) {
        if ((in32(REG_SRC) & SRC_STATE_MASK) == SRC_STATE_READY) {
            break;
        }
    }
    out32(REG_CODEC, ((reg & 0x7fu) << 16) | (1u << 23));
    (void)src_wait();
    out32(REG_SRC, saved);
    for (uint32_t i = 0; i < POLL_COUNT; i++) {
        uint32_t value = in32(REG_CODEC);
        if ((value & (1u << 31)) != 0) {
            return value & 0xffffu;
        }
    }
    return 0xffffffffu;
}

static bool is_audiopci97(const struct pci_device *device) {
    return (device->vendor_id == 0x1274
            && (device->device_id == 0x1371 || device->device_id == 0x5880))
        || (device->vendor_id == 0x1102 && device->device_id == 0x8938);
}

static bool find_card(void) {
    /* Multimedia audio (class 04, subclass 01). */
    for (size_t index = 0; index < 8; index++) {
        if (!pci_find_class(0x04, 0x01, 0x00, index, &card)) {
            return false;
        }
        if (is_audiopci97(&card)) {
            return true;
        }
    }
    return false;
}

bool es1371_initialize(void) {
    ready = false;
    if (!find_card()) {
        return false;
    }
    bool io_space = false;
    uint64_t bar = pci_bar_address(&card, 0, &io_space);
    if (bar == 0 || !io_space || bar > 0xffffu) {
        console_write_string("OPOS: AudioPCI: no I/O ports\n");
        return false;
    }
    port = (uint16_t)bar;
    (void)pci_set_power_d0(&card);
    pci_enable_device(&card);

    uint64_t physical = physical_pages_alloc_contiguous(RING_PAGES);
    if (physical == 0) {
        return false;
    }
    if (physical + RING_BYTES > 0x100000000ULL) {
        console_write_string("OPOS: AudioPCI: no memory below 4 GiB\n");
        for (uint32_t i = 0; i < RING_PAGES; i++) {
            (void)physical_page_free(physical + (uint64_t)i * 4096);
        }
        return false;
    }
    ring = physical_page_to_hhdm(physical);
    for (uint32_t i = 0; i < RING_BYTES; i++) {
        ring[i] = 0;
    }

    /* Everything off, then a warm reset of the AC'97 link. */
    out32(REG_CONTROL, 0);
    out32(REG_SERIAL, 0);
    out32(REG_LEGACY, 0);
    out32(REG_STATUS, STATUS_SYNC_RESET);
    (void)in32(REG_STATUS);
    delay_us(20);
    out32(REG_STATUS, 0);

    /* The sample rate converter: cleared, unity volumes, DAC2 at 48 kHz
       (freq = rate * 2^15 / 3000, split over INT_REGS and VFREQ_FRAC). */
    (void)src_wait();
    out32(REG_SRC, SRC_DISABLE);
    for (uint32_t i = 0; i < 0x80; i++) {
        src_write(i, 0);
    }
    src_write(SRC_DAC1 + SRC_TRUNC_N, 16u << 4);
    src_write(SRC_DAC1 + SRC_INT_REGS, 16u << 10);
    src_write(SRC_DAC2 + SRC_TRUNC_N, 16u << 4);
    src_write(SRC_DAC2 + SRC_INT_REGS, 16u << 10);
    src_write(SRC_VOLUME_ADC, 1u << 12);
    src_write(SRC_VOLUME_ADC + 1, 1u << 12);
    src_write(SRC_VOLUME_DAC1, 1u << 12);
    src_write(SRC_VOLUME_DAC1 + 1, 1u << 12);
    src_write(SRC_VOLUME_DAC2, 1u << 12);
    src_write(SRC_VOLUME_DAC2 + 1, 1u << 12);
    uint32_t freq = (RATE << 15) / 3000u;
    src_write(SRC_DAC2 + SRC_INT_REGS, (freq >> 5) & 0xfc00u);
    src_write(SRC_DAC2 + SRC_VFREQ_FRAC, freq & 0x7fffu);
    (void)src_wait();
    out32(REG_SRC, 0); /* SRC on */

    /* The codec: reset, then master, headphones and PCM at 0 dB. */
    codec_write(AC97_RESET, 0);
    delay_us(1000);
    codec_vendor = codec_read(AC97_VENDOR1) << 16 | codec_read(AC97_VENDOR2);
    codec_write(AC97_MASTER, 0x0000);
    codec_write(AC97_HEADPHONE, 0x0000);
    codec_write(AC97_PCM_OUT, 0x0808);

    /* DAC2 plays the ring: 16-bit stereo, looping, no interrupts. */
    out32(REG_MEM_PAGE, PAGE_DAC);
    out32(REG_DAC2_FRAME, (uint32_t)physical);
    out32(REG_DAC2_SIZE, RING_BYTES / 4 - 1);
    /* The mode, then the count (frames per period - 1), in the order
       Linux uses. */
    out32(REG_SERIAL, SERIAL_P2_MODE(3) | SERIAL_P2_END_INC(2));
    out32(REG_DAC2_COUNT, 1024 - 1);

    source = silence;
    played_bytes = 0;
    written_bytes = RING_BYTES;
    last_position = 0;
    out32(REG_CONTROL, CONTROL_DAC2_ENABLE);
    ready = true;
    console_write_string("OPOS: AudioPCI sound ready\n");
    return true;
}

bool es1371_ready(void) {
    return ready;
}

void es1371_set_source(void (*new_source)(int16_t *out, size_t frames)) {
    source = new_source != NULL ? new_source : silence;
}

void es1371_poll(void) {
    if (!ready) {
        return;
    }
    /* VMware's card (measured) plays one period (1024 frames, 21 ms) and
       then waits until its DAC2 interrupt flag is cleared. Rewriting
       SERIAL clears it, as Linux's interrupt handler does; no interrupt is
       enabled, so none reaches the CPU. At 100 Hz polls that keeps it
       playing. */
    if ((in32(REG_STATUS) & STATUS_DAC2) != 0) {
        out32(REG_SERIAL, SERIAL_P2_MODE(3) | SERIAL_P2_END_INC(2));
    }
    /* DAC2_SIZE's upper half is the position in 32-bit words. */
    out32(REG_MEM_PAGE, PAGE_DAC);
    uint32_t position = ((in32(REG_DAC2_SIZE) >> 16) * 4u) % RING_BYTES;
    uint32_t advanced = (position + RING_BYTES - last_position) % RING_BYTES;
    last_position = position;
    played_bytes += advanced;
    if (written_bytes < played_bytes) {
        written_bytes = played_bytes;
    }
    uint64_t target = played_bytes + LEAD_BYTES;
    while (written_bytes < target) {
        uint32_t at = (uint32_t)(written_bytes % RING_BYTES);
        uint32_t chunk = RING_BYTES - at;
        if (chunk > target - written_bytes) {
            chunk = (uint32_t)(target - written_bytes);
        }
        chunk -= chunk % FRAME_BYTES;
        if (chunk == 0) {
            break;
        }
        source((int16_t *)(ring + at), chunk / FRAME_BYTES);
        written_bytes += chunk;
    }
}

static void append(char *line, size_t *used, size_t capacity,
                   const char *text) {
    while (*text != '\0' && *used + 1 < capacity) {
        line[(*used)++] = *text++;
    }
    line[*used] = '\0';
}

static void append_hex(char *line, size_t *used, size_t capacity,
                       uint32_t value, int digits) {
    static const char hex[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0 && *used + 1 < capacity; i--) {
        line[(*used)++] = hex[(value >> (i * 4)) & 0xfu];
    }
    line[*used] = '\0';
}

static void append_decimal(char *line, size_t *used, size_t capacity,
                           uint64_t value) {
    char digits[24];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0 && *used + 1 < capacity) {
        line[(*used)++] = digits[--count];
    }
    line[*used] = '\0';
}

void es1371_describe(void (*print)(const char *line)) {
    if (!ready) {
        return;
    }
    char line[96];
    size_t used = 0;
    append(line, &used, sizeof(line), "Sound: AudioPCI 97 ");
    append_hex(line, &used, sizeof(line), card.vendor_id, 4);
    append(line, &used, sizeof(line), ":");
    append_hex(line, &used, sizeof(line), card.device_id, 4);
    append(line, &used, sizeof(line), " at ");
    append_hex(line, &used, sizeof(line), card.bus, 2);
    append(line, &used, sizeof(line), ":");
    append_hex(line, &used, sizeof(line), card.device, 2);
    append(line, &used, sizeof(line), ".");
    append_hex(line, &used, sizeof(line), card.function, 1);
    append(line, &used, sizeof(line), ", AC'97 codec ");
    append_hex(line, &used, sizeof(line), codec_vendor, 8);
    print(line);
    used = 0;
    append(line, &used, sizeof(line), "  48000 Hz, 16-bit stereo; played ");
    append_decimal(line, &used, sizeof(line), played_bytes / FRAME_BYTES);
    append(line, &used, sizeof(line), " frames");
    print(line);
}
