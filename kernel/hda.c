#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "hda.h"
#include "io.h"
#include "memory.h"
#include "paging.h"
#include "pci.h"

/*
 * Intel High Definition Audio, output only (docs/sound.md).
 *
 * The controller talks to codecs through two rings in memory: commands go
 * into the CORB, answers come back in the RIRB. Both are polled. Sound is
 * one output stream: a ring of RING_ENTRIES buffers described by a buffer
 * descriptor list (BDL), which the controller plays over and over. The
 * 100 Hz timer calls hda_poll, which reads how far the controller has
 * played (LPIB) and writes new samples behind it.
 *
 * Codecs differ between machines, so the driver walks each codec's widget
 * graph: from every connected output pin (line out, speaker, headphone)
 * back through mixers and selectors to a converter (DAC), and sets every
 * DAC it reaches to the one stream.
 *
 * A machine may have several HDA controllers (the chipset's, and one per
 * graphics card for HDMI); the first with an analog output is used. What
 * the probe found is kept as a few lines for the shell's "sound" command,
 * so a report from a real machine shows why it has no sound.
 *
 * Some chipsets do not snoop the CPU caches for HDA DMA, so the driver
 * flushes what it writes for the controller (CORB, sample ring) and drops
 * the cached copy of what the controller writes (RIRB) before reading it.
 */

#define HDA_MMIO_BASE 0xffffc00000500000ULL
#define HDA_MMIO_PAGES 4
/* Each controller tried gets its own 64 KiB of address space. */
#define HDA_MMIO_STRIDE 0x10000ULL
#define MAX_CONTROLLERS 4u

/* Controller registers. */
#define REG_GCAP 0x00
#define GCAP_64BIT 0x0001u
#define REG_VMIN 0x02
#define REG_VMAJ 0x03
#define REG_GCTL 0x08
#define REG_STATESTS 0x0e
#define REG_INTCTL 0x20
#define REG_CORBLBASE 0x40
#define REG_CORBUBASE 0x44
#define REG_CORBWP 0x48
#define REG_CORBRP 0x4a
#define REG_CORBCTL 0x4c
#define REG_CORBSIZE 0x4e
#define REG_RIRBLBASE 0x50
#define REG_RIRBUBASE 0x54
#define REG_RIRBWP 0x58
#define REG_RINTCNT 0x5a
#define REG_RIRBCTL 0x5c
#define REG_RIRBSTS 0x5d
#define REG_RIRBSIZE 0x5e
#define REG_DPLBASE 0x70
#define REG_DPUBASE 0x74
#define REG_STREAMS 0x80
#define STREAM_SIZE 0x20

/* Stream descriptor registers (offsets in a descriptor). */
#define SD_CTL 0x00
#define SD_STS 0x03
#define SD_LPIB 0x04
#define SD_CBL 0x08
#define SD_LVI 0x0c
#define SD_FMT 0x12
#define SD_BDPL 0x18
#define SD_BDPU 0x1c
#define SD_CTL_RESET 0x01u
#define SD_CTL_RUN 0x02u

#define GCTL_RESET 0x01u
#define RING_DMA_RUN 0x02u
#define RIRB_RESPONSE_FLAG_ENABLE 0x01u
#define CORBRP_RESET 0x8000u
#define RIRBWP_RESET 0x8000u

/* Codec verbs. */
#define VERB_GET_PARAMETER 0xf00u
#define VERB_GET_CONNECTION_LIST 0xf02u
#define VERB_SET_CONNECTION_SELECT 0x701u
#define VERB_SET_POWER_STATE 0x705u
#define VERB_SET_STREAM_CHANNEL 0x706u
#define VERB_SET_PIN_CONTROL 0x707u
#define VERB_SET_EAPD 0x70cu
#define VERB_GET_CONFIG_DEFAULT 0xf1cu
/* Four-bit verbs with a 16-bit payload. */
#define VERB_SET_FORMAT 0x2u
#define VERB_SET_AMP 0x3u

/* GET_PARAMETER parameters. */
#define PARAM_VENDOR_ID 0x00u
#define PARAM_NODE_COUNT 0x04u
#define PARAM_FUNCTION_TYPE 0x05u
#define PARAM_WIDGET_CAPS 0x09u
#define PARAM_PIN_CAPS 0x0cu
#define PARAM_INPUT_AMP_CAPS 0x0du
#define PARAM_CONNECTION_LENGTH 0x0eu
#define PARAM_OUTPUT_AMP_CAPS 0x12u

#define FUNCTION_AUDIO 0x01u
#define WIDGET_OUTPUT 0x0u
#define WIDGET_MIXER 0x2u
#define WIDGET_SELECTOR 0x3u
#define WIDGET_PIN 0x4u
#define CAPS_INPUT_AMP (1u << 1)
#define CAPS_OUTPUT_AMP (1u << 2)
#define CAPS_AMP_OVERRIDE (1u << 3)
#define CAPS_CONNECTION_LIST (1u << 8)
#define CAPS_DIGITAL (1u << 9)
#define CAPS_POWER_CONTROL (1u << 10)
#define PIN_CAPS_OUTPUT (1u << 4)
#define PIN_CAPS_HEADPHONE (1u << 3)
#define PIN_CAPS_EAPD (1u << 16)
#define PIN_CONTROL_OUT 0x40u
#define PIN_CONTROL_HEADPHONE 0x80u

/* 48 kHz, 16 bits, 2 channels. */
#define STREAM_FORMAT 0x0011u
#define STREAM_TAG 1u

#define CORB_ENTRIES 256u
#define RIRB_ENTRIES 256u
#define RING_ENTRIES 8u
#define RING_ENTRY_BYTES 4096u
#define RING_BYTES (RING_ENTRIES * RING_ENTRY_BYTES)
#define FRAME_BYTES 4u
/* Samples written ahead of the playing position. */
#define LEAD_BYTES (RING_BYTES - RING_ENTRY_BYTES)

#define MAX_NODES 128u
#define MAX_CONNECTIONS 16u
#define MAX_PATH 6u
#define MAX_OUTPUTS 4u

struct widget {
    uint32_t caps;
    uint32_t pin_caps;
    uint32_t config;
    uint8_t connection_count;
    uint8_t connections[MAX_CONNECTIONS];
};

struct output_path {
    uint8_t codec;
    uint8_t length;
    uint8_t nodes[MAX_PATH];  /* pin first, DAC last */
    uint8_t device;           /* default device of the pin */
};

static volatile uint8_t *registers;
static bool ready;
static struct pci_device controller;
static uint32_t *corb;
static uint64_t *rirb;
static uint8_t *ring;
static uint16_t corb_write;
static uint16_t rirb_read;
static uint32_t stream_base;
static uint8_t codec_mask;
static uint32_t codec_vendor;
static uint8_t codec_address;

/* What the probe found, for hda_describe. */
#define PROBE_LINES 12u
#define PROBE_LINE_BYTES 80u
static char probe_log[PROBE_LINES][PROBE_LINE_BYTES];
static size_t probe_count;

static struct widget widgets[MAX_NODES];
static uint8_t afg_node;
static struct output_path outputs[MAX_OUTPUTS];
static size_t output_count;

/* Ring bookkeeping (bytes, counted from start). */
static uint64_t played_bytes;
static uint64_t written_bytes;
static uint32_t last_lpib;
static hda_source source;

static uint32_t beep_frames_left;
static uint32_t beep_period;
static uint32_t beep_phase;

static uint8_t read8(uint32_t offset) {
    return *(volatile uint8_t *)(registers + offset);
}

static uint16_t read16(uint32_t offset) {
    return *(volatile uint16_t *)(registers + offset);
}

static uint32_t read32(uint32_t offset) {
    return *(volatile uint32_t *)(registers + offset);
}

static void write8(uint32_t offset, uint8_t value) {
    *(volatile uint8_t *)(registers + offset) = value;
}

static void write16(uint32_t offset, uint16_t value) {
    *(volatile uint16_t *)(registers + offset) = value;
}

static void write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(registers + offset) = value;
}

/* About a microsecond per step (port 0x80 reads). */
static void delay_us(uint32_t microseconds) {
    for (uint32_t i = 0; i < microseconds; i++) {
        io_wait();
    }
}

/* Waits up to about timeout_us for (read & mask) == value. */
/* Writes back (and drops) the cache lines of [address, address + size). */
static void cache_flush(const void *address, size_t size) {
    uintptr_t line = (uintptr_t)address & ~(uintptr_t)63;
    uintptr_t end = (uintptr_t)address + size;
    for (; line < end; line += 64) {
        __asm__ volatile ("clflush (%0)" : : "r"(line) : "memory");
    }
    __asm__ volatile ("mfence" : : : "memory");
}

static bool wait8(uint32_t offset, uint8_t mask, uint8_t value,
                  uint32_t timeout_us) {
    for (uint32_t i = 0; i < timeout_us; i++) {
        if ((read8(offset) & mask) == value) {
            return true;
        }
        delay_us(1);
    }
    return false;
}

static bool wait16(uint32_t offset, uint16_t mask, uint16_t value,
                   uint32_t timeout_us) {
    for (uint32_t i = 0; i < timeout_us; i++) {
        if ((read16(offset) & mask) == value) {
            return true;
        }
        delay_us(1);
    }
    return false;
}

static bool wait32(uint32_t offset, uint32_t mask, uint32_t value,
                   uint32_t timeout_us) {
    for (uint32_t i = 0; i < timeout_us; i++) {
        if ((read32(offset) & mask) == value) {
            return true;
        }
        delay_us(1);
    }
    return false;
}

static bool map_registers(uint64_t physical, size_t slot) {
    uint64_t base = HDA_MMIO_BASE + slot * HDA_MMIO_STRIDE;
    for (size_t page = 0; page < HDA_MMIO_PAGES; page++) {
        if (!paging_map_page(base + page * 0x1000,
                             (physical & ~0xfffULL) + page * 0x1000,
                             PAGING_WRITABLE | PAGING_WRITE_THROUGH
                                 | PAGING_CACHE_DISABLE
                                 | PAGING_NO_EXECUTE)) {
            return false;
        }
    }
    registers = (volatile uint8_t *)(base + (physical & 0xfffULL));
    return true;
}

/* ------------------------------------------------------------ commands */

/* Sends one verb and waits for its answer (unsolicited answers are
   skipped). */
static bool command(uint8_t codec, uint8_t node, uint32_t verb,
                    uint32_t *response) {
    uint32_t word = (uint32_t)codec << 28 | (uint32_t)node << 20 | verb;
    corb_write = (uint16_t)((corb_write + 1) % CORB_ENTRIES);
    corb[corb_write] = word;
    cache_flush(&corb[corb_write], sizeof(corb[0]));
    write16(REG_CORBWP, corb_write);
    for (uint32_t i = 0; i < 100000; i++) {
        uint16_t write_pointer = read16(REG_RIRBWP) & 0xff;
        while (rirb_read != write_pointer) {
            rirb_read = (uint16_t)((rirb_read + 1) % RIRB_ENTRIES);
            cache_flush(&rirb[rirb_read], sizeof(rirb[0]));
            uint64_t entry = rirb[rirb_read];
            uint32_t extended = (uint32_t)(entry >> 32);
            if ((extended & 0x10u) != 0) {
                continue; /* unsolicited */
            }
            if (response != NULL) {
                *response = (uint32_t)entry;
            }
            write8(REG_RIRBSTS, 0x05);
            return true;
        }
        delay_us(1);
    }
    return false;
}

/* 12-bit verb with an 8-bit payload. */
static uint32_t verb12(uint32_t verb, uint32_t payload) {
    return (verb & 0xfffu) << 8 | (payload & 0xffu);
}

/* 4-bit verb with a 16-bit payload. */
static uint32_t verb4(uint32_t verb, uint32_t payload) {
    return (verb & 0xfu) << 16 | (payload & 0xffffu);
}

static uint32_t parameter(uint8_t codec, uint8_t node, uint32_t which) {
    uint32_t value = 0;
    if (!command(codec, node, verb12(VERB_GET_PARAMETER, which), &value)) {
        return 0;
    }
    return value;
}

static void set(uint8_t codec, uint8_t node, uint32_t verb) {
    (void)command(codec, node, verb, NULL);
}

/* ------------------------------------------------------- codec graph */

static uint32_t widget_type(const struct widget *widget) {
    return (widget->caps >> 20) & 0xfu;
}

static void read_connections(uint8_t codec, uint8_t node,
                             struct widget *widget) {
    widget->connection_count = 0;
    if ((widget->caps & CAPS_CONNECTION_LIST) == 0) {
        return;
    }
    uint32_t length_word = parameter(codec, node, PARAM_CONNECTION_LENGTH);
    bool long_form = (length_word & 0x80u) != 0;
    uint32_t length = length_word & 0x7fu;
    uint32_t per_word = long_form ? 2 : 4;
    uint32_t bits = long_form ? 16 : 8;
    uint32_t previous = 0;
    for (uint32_t offset = 0; offset < length; offset += per_word) {
        uint32_t word = 0;
        if (!command(codec, node,
                     verb12(VERB_GET_CONNECTION_LIST, offset), &word)) {
            return;
        }
        for (uint32_t i = 0; i < per_word && offset + i < length; i++) {
            uint32_t entry = (word >> (i * bits))
                & ((1u << bits) - 1u);
            uint32_t range_flag = 1u << (bits - 1);
            uint32_t id = entry & (range_flag - 1u);
            if ((entry & range_flag) != 0 && previous != 0) {
                /* A range: previous+1 .. id. */
                for (uint32_t n = previous + 1; n <= id; n++) {
                    if (widget->connection_count < MAX_CONNECTIONS) {
                        widget->connections[widget->connection_count++] =
                            (uint8_t)n;
                    }
                }
            } else if (widget->connection_count < MAX_CONNECTIONS) {
                widget->connections[widget->connection_count++] =
                    (uint8_t)id;
            }
            previous = id;
        }
    }
}

/* Depth-first from node towards a DAC. path[0..depth) is filled. */
static bool find_dac(uint8_t node, struct output_path *path, uint8_t depth) {
    if (node == 0 || node >= MAX_NODES || depth >= MAX_PATH) {
        return false;
    }
    for (uint8_t i = 0; i < depth; i++) {
        if (path->nodes[i] == node) {
            return false; /* loop */
        }
    }
    const struct widget *widget = &widgets[node];
    path->nodes[depth] = node;
    uint32_t type = widget_type(widget);
    if (type == WIDGET_OUTPUT && (widget->caps & CAPS_DIGITAL) == 0) {
        path->length = (uint8_t)(depth + 1);
        return true;
    }
    if (depth != 0 && type != WIDGET_MIXER && type != WIDGET_SELECTOR) {
        return false;
    }
    for (uint8_t i = 0; i < widget->connection_count; i++) {
        if (find_dac(widget->connections[i], path, (uint8_t)(depth + 1))) {
            return true;
        }
    }
    return false;
}

/* Amplifier offset (0 dB step) from caps of node or, by default, the
   function group. */
static uint32_t amp_offset(uint8_t codec, uint8_t node, bool output) {
    uint32_t which = output ? PARAM_OUTPUT_AMP_CAPS : PARAM_INPUT_AMP_CAPS;
    uint32_t caps = (widgets[node].caps & CAPS_AMP_OVERRIDE) != 0
        ? parameter(codec, node, which) : parameter(codec, afg_node, which);
    uint32_t offset = caps & 0x7fu;
    uint32_t steps = (caps >> 8) & 0x7fu;
    return offset <= steps ? offset : steps;
}

static void unmute_output(uint8_t codec, uint8_t node) {
    if ((widgets[node].caps & CAPS_OUTPUT_AMP) == 0) {
        return;
    }
    uint32_t gain = amp_offset(codec, node, true);
    set(codec, node, verb4(VERB_SET_AMP, 0xb000u | gain));
}

static void unmute_input(uint8_t codec, uint8_t node, uint8_t index) {
    if ((widgets[node].caps & CAPS_INPUT_AMP) == 0) {
        return;
    }
    uint32_t gain = amp_offset(codec, node, false);
    set(codec, node,
        verb4(VERB_SET_AMP, 0x7000u | (uint32_t)index << 8 | gain));
}

/* Powers, routes and unmutes one pin-to-DAC path. */
static void set_up_path(uint8_t codec, const struct output_path *path) {
    for (uint8_t i = 0; i < path->length; i++) {
        uint8_t node = path->nodes[i];
        const struct widget *widget = &widgets[node];
        if ((widget->caps & CAPS_POWER_CONTROL) != 0) {
            set(codec, node, verb12(VERB_SET_POWER_STATE, 0));
        }
        if (i + 1 < path->length) {
            uint8_t next = path->nodes[i + 1];
            uint8_t index = 0;
            for (uint8_t k = 0; k < widget->connection_count; k++) {
                if (widget->connections[k] == next) {
                    index = k;
                    break;
                }
            }
            if (widget_type(widget) == WIDGET_MIXER) {
                unmute_input(codec, node, index);
            } else if (widget->connection_count > 1) {
                set(codec, node,
                    verb12(VERB_SET_CONNECTION_SELECT, index));
            }
            if (widget_type(widget) == WIDGET_SELECTOR) {
                unmute_input(codec, node, index);
            }
        }
        unmute_output(codec, node);
    }
    uint8_t pin = path->nodes[0];
    uint32_t control = PIN_CONTROL_OUT;
    if ((widgets[pin].pin_caps & PIN_CAPS_HEADPHONE) != 0
        && path->device == 2) {
        control |= PIN_CONTROL_HEADPHONE;
    }
    set(codec, pin, verb12(VERB_SET_PIN_CONTROL, control));
    if ((widgets[pin].pin_caps & PIN_CAPS_EAPD) != 0) {
        set(codec, pin, verb12(VERB_SET_EAPD, 0x02));
    }
    uint8_t dac = path->nodes[path->length - 1];
    set(codec, dac, verb12(VERB_SET_STREAM_CHANNEL, STREAM_TAG << 4));
    set(codec, dac, verb4(VERB_SET_FORMAT, STREAM_FORMAT));
}

/* Finds the audio function group of codec and the output paths in it. */
static bool set_up_codec(uint8_t codec) {
    uint32_t root = parameter(codec, 0, PARAM_NODE_COUNT);
    uint32_t first = (root >> 16) & 0xffu;
    uint32_t count = root & 0xffu;
    afg_node = 0;
    for (uint32_t n = first; n < first + count && n < MAX_NODES; n++) {
        if ((parameter(codec, (uint8_t)n, PARAM_FUNCTION_TYPE) & 0xffu)
            == FUNCTION_AUDIO) {
            afg_node = (uint8_t)n;
            break;
        }
    }
    if (afg_node == 0) {
        return false;
    }
    codec_vendor = parameter(codec, 0, PARAM_VENDOR_ID);
    set(codec, afg_node, verb12(VERB_SET_POWER_STATE, 0));
    delay_us(10000);

    uint32_t nodes = parameter(codec, afg_node, PARAM_NODE_COUNT);
    uint32_t start = (nodes >> 16) & 0xffu;
    uint32_t total = nodes & 0xffu;
    for (size_t i = 0; i < MAX_NODES; i++) {
        widgets[i] = (struct widget){0};
    }
    for (uint32_t n = start; n < start + total && n < MAX_NODES; n++) {
        struct widget *widget = &widgets[n];
        widget->caps = parameter(codec, (uint8_t)n, PARAM_WIDGET_CAPS);
        if (widget_type(widget) == WIDGET_PIN) {
            widget->pin_caps = parameter(codec, (uint8_t)n, PARAM_PIN_CAPS);
            (void)command(codec, (uint8_t)n,
                          verb12(VERB_GET_CONFIG_DEFAULT, 0),
                          &widget->config);
        }
        read_connections(codec, (uint8_t)n, widget);
    }

    /* Connected output pins: line out (0), speaker (1), headphone (2).
       Pins whose configuration says "no connection" (31:30 = 1) are
       skipped, except that a codec with no configured pins (QEMU's
       and some virtual ones report 0) still gets its outputs. */
    size_t before = output_count;
    for (int pass = 0; pass < 2 && output_count == before; pass++) {
        for (uint32_t n = start;
             n < start + total && n < MAX_NODES
                 && output_count < MAX_OUTPUTS;
             n++) {
            const struct widget *widget = &widgets[n];
            if (widget_type(widget) != WIDGET_PIN
                || (widget->pin_caps & PIN_CAPS_OUTPUT) == 0) {
                continue;
            }
            uint32_t connectivity = widget->config >> 30;
            uint32_t device = (widget->config >> 20) & 0xfu;
            if (pass == 0 && (connectivity == 1 || device > 2)) {
                continue;
            }
            struct output_path path = {.codec = codec,
                                       .device = (uint8_t)device};
            if (find_dac((uint8_t)n, &path, 0)) {
                outputs[output_count++] = path;
                set_up_path(codec, &path);
            }
        }
    }
    return output_count != before;
}

/* ---------------------------------------------------------- controller */

static bool reset_controller(void) {
    write32(REG_GCTL, read32(REG_GCTL) & ~GCTL_RESET);
    if (!wait32(REG_GCTL, GCTL_RESET, 0, 100000)) {
        return false;
    }
    delay_us(100);
    write32(REG_GCTL, read32(REG_GCTL) | GCTL_RESET);
    if (!wait32(REG_GCTL, GCTL_RESET, GCTL_RESET, 100000)) {
        return false;
    }
    /* Codecs announce themselves within 521 us of the reset ending. */
    delay_us(2000);
    return true;
}

static bool start_rings(uint64_t corb_physical, uint64_t rirb_physical) {
    write8(REG_CORBCTL, 0);
    write8(REG_RIRBCTL, 0);
    if (!wait8(REG_CORBCTL, RING_DMA_RUN, 0, 10000)
        || !wait8(REG_RIRBCTL, RING_DMA_RUN, 0, 10000)) {
        return false;
    }
    /* 256 entries (size code 2); every controller supports it. */
    write8(REG_CORBSIZE, (uint8_t)((read8(REG_CORBSIZE) & ~0x3u) | 0x2u));
    write32(REG_CORBLBASE, (uint32_t)corb_physical);
    write32(REG_CORBUBASE, (uint32_t)(corb_physical >> 32));
    write16(REG_CORBRP, CORBRP_RESET);
    /* Some controllers need the reset bit to be seen set, then cleared. */
    (void)wait16(REG_CORBRP, CORBRP_RESET, CORBRP_RESET, 1000);
    write16(REG_CORBRP, 0);
    (void)wait16(REG_CORBRP, CORBRP_RESET, 0, 1000);
    write16(REG_CORBWP, 0);
    corb_write = 0;

    write8(REG_RIRBSIZE, (uint8_t)((read8(REG_RIRBSIZE) & ~0x3u) | 0x2u));
    write32(REG_RIRBLBASE, (uint32_t)rirb_physical);
    write32(REG_RIRBUBASE, (uint32_t)(rirb_physical >> 32));
    write16(REG_RIRBWP, RIRBWP_RESET);
    write16(REG_RINTCNT, 1);
    rirb_read = 0;

    write8(REG_CORBCTL, RING_DMA_RUN);
    /* The response-flag enable too: QEMU stops taking commands after
       RINTCNT answers until software clears that flag, which is only
       set with it on. INTCTL stays 0, so no interrupt reaches the CPU. */
    write8(REG_RIRBCTL, RING_DMA_RUN | RIRB_RESPONSE_FLAG_ENABLE);
    return wait8(REG_CORBCTL, RING_DMA_RUN, RING_DMA_RUN, 10000)
        && wait8(REG_RIRBCTL, RING_DMA_RUN, RING_DMA_RUN, 10000);
}

static uint32_t stream_ctl(void) {
    return read32(stream_base + SD_CTL);
}

static bool start_stream(uint64_t bdl_physical) {
    /* Reset the stream descriptor. */
    write32(stream_base + SD_CTL, stream_ctl() & ~SD_CTL_RUN);
    write32(stream_base + SD_CTL, SD_CTL_RESET);
    if (!wait32(stream_base + SD_CTL, SD_CTL_RESET, SD_CTL_RESET, 10000)) {
        return false;
    }
    write32(stream_base + SD_CTL, 0);
    if (!wait32(stream_base + SD_CTL, SD_CTL_RESET, 0, 10000)) {
        return false;
    }
    write32(stream_base + SD_CBL, RING_BYTES);
    write16(stream_base + SD_LVI, RING_ENTRIES - 1);
    write16(stream_base + SD_FMT, STREAM_FORMAT);
    write32(stream_base + SD_BDPL, (uint32_t)bdl_physical);
    write32(stream_base + SD_BDPU, (uint32_t)(bdl_physical >> 32));
    write8(stream_base + SD_STS, 0x1c);
    /* Stream number in bits 23:20, then run. No interrupts. */
    write32(stream_base + SD_CTL, STREAM_TAG << 20);
    write32(stream_base + SD_CTL, STREAM_TAG << 20 | SD_CTL_RUN);
    return wait32(stream_base + SD_CTL, SD_CTL_RUN, SD_CTL_RUN, 10000);
}

static void silence(int16_t *out, size_t frames) {
    for (size_t i = 0; i < frames * HDA_CHANNELS; i++) {
        out[i] = 0;
    }
}

/* A square wave, mixed over whatever the source wrote. */
static void add_beep(int16_t *out, size_t frames) {
    for (size_t i = 0; i < frames && beep_frames_left != 0; i++) {
        int16_t value = beep_phase < beep_period / 2 ? 6000 : -6000;
        int32_t left = out[2 * i] + value;
        int32_t right = out[2 * i + 1] + value;
        out[2 * i] = (int16_t)(left > 32767 ? 32767
                               : left < -32768 ? -32768 : left);
        out[2 * i + 1] = (int16_t)(right > 32767 ? 32767
                                   : right < -32768 ? -32768 : right);
        beep_phase = (beep_phase + 1) % beep_period;
        beep_frames_left--;
    }
}

static void probe_line(const char *a, uint32_t hex, int digits,
                       const char *b) {
    if (probe_count == PROBE_LINES) {
        return;
    }
    char *line = probe_log[probe_count++];
    size_t used = 0;
    while (*a != '\0' && used + 1 < PROBE_LINE_BYTES) {
        line[used++] = *a++;
    }
    static const char digit[] = "0123456789abcdef";
    for (int i = digits - 1; i >= 0 && used + 1 < PROBE_LINE_BYTES; i--) {
        line[used++] = digit[(hex >> (i * 4)) & 0xfu];
    }
    while (b != NULL && *b != '\0' && used + 1 < PROBE_LINE_BYTES) {
        line[used++] = *b++;
    }
    line[used] = '\0';
}

/* Stops a controller that was not chosen, so it stops reading memory the
   next one will use. */
static void stop_controller(void) {
    write8(REG_CORBCTL, 0);
    write8(REG_RIRBCTL, 0);
    write32(REG_GCTL, read32(REG_GCTL) & ~GCTL_RESET);
}

/* Sets up one controller. memory/physical: the CORB, RIRB, BDL page and
   then the sample ring. */
static bool try_controller(size_t slot, uint8_t *memory, uint64_t physical) {
    probe_line("controller ", (uint32_t)controller.vendor_id << 16
                   | controller.device_id, 8, NULL);
    bool io_space = false;
    uint64_t bar = pci_bar_address(&controller, 0, &io_space);
    if (bar == 0 || io_space) {
        probe_line("  no memory BAR", 0, 0, NULL);
        return false;
    }
    (void)pci_set_power_d0(&controller);
    pci_enable_device(&controller);
    if (controller.vendor_id == 0x8086) {
        /* Traffic class 0 (TCSEL, config 0x44 bits 2:0), as Linux sets
           it on Intel controllers. */
        uint32_t value = pci_config_read32(&controller, 0x44);
        pci_config_write32(&controller, 0x44, value & ~0x7u);
    }
    if (!map_registers(bar, slot)) {
        probe_line("  cannot map registers", 0, 0, NULL);
        return false;
    }
    uint16_t caps = read16(REG_GCAP);
    if ((caps & GCAP_64BIT) == 0
        && physical + 4096 + RING_BYTES > 0x100000000ULL) {
        probe_line("  32-bit DMA only, buffers above 4 GiB", 0, 0, NULL);
        return false;
    }
    uint32_t input_streams = (caps >> 8) & 0xfu;
    uint32_t output_streams = (caps >> 12) & 0xfu;
    if (output_streams == 0) {
        probe_line("  no output streams", 0, 0, NULL);
        return false;
    }

    for (size_t i = 0; i < 4096 + RING_BYTES; i++) {
        memory[i] = 0;
    }
    corb = (uint32_t *)memory;
    rirb = (uint64_t *)(memory + 1024);
    uint64_t *bdl = (uint64_t *)(memory + 3072);
    ring = memory + 4096;
    uint64_t ring_physical = physical + 4096;
    for (uint32_t i = 0; i < RING_ENTRIES; i++) {
        bdl[2 * i] = ring_physical + (uint64_t)i * RING_ENTRY_BYTES;
        bdl[2 * i + 1] = RING_ENTRY_BYTES; /* length; no interrupt */
    }
    cache_flush(memory, 4096 + RING_BYTES);

    write32(REG_INTCTL, 0);
    if (!reset_controller()) {
        probe_line("  reset failed", 0, 0, NULL);
        return false;
    }
    codec_mask = (uint8_t)(read16(REG_STATESTS) & 0x7fffu);
    write16(REG_STATESTS, 0x7fff);
    if (codec_mask == 0) {
        probe_line("  no codec", 0, 0, NULL);
        stop_controller();
        return false;
    }
    if (!start_rings(physical, physical + 1024)) {
        probe_line("  command rings did not start", 0, 0, NULL);
        stop_controller();
        return false;
    }
    output_count = 0;
    for (uint8_t codec = 0; codec < 15; codec++) {
        if ((codec_mask & (1u << codec)) == 0) {
            continue;
        }
        bool found = set_up_codec(codec);
        uint32_t vendor = parameter(codec, 0, PARAM_VENDOR_ID);
        probe_line("  codec ", (uint32_t)codec, 1, NULL);
        probe_line("    vendor ", vendor, 8,
                   found ? ", analog outputs" : ", no analog output");
        if (found) {
            codec_address = codec;
            break; /* first codec with outputs (the onboard one) */
        }
    }
    if (output_count == 0) {
        stop_controller();
        return false;
    }
    stream_base = REG_STREAMS + input_streams * STREAM_SIZE;
    source = silence;
    played_bytes = 0;
    written_bytes = RING_BYTES; /* the ring starts full of silence */
    last_lpib = 0;
    if (!start_stream(physical + 3072)) {
        probe_line("  output stream did not start", 0, 0, NULL);
        stop_controller();
        return false;
    }
    return true;
}

bool hda_initialize(void) {
    ready = false;
    probe_count = 0;
    /* One page for the CORB (1 KiB), RIRB (2 KiB) and BDL, then the
       sample ring; shared by the controllers tried, one at a time. */
    uint64_t physical =
        physical_pages_alloc_contiguous(1 + RING_BYTES / 4096);
    if (physical == 0) {
        return false;
    }
    uint8_t *memory = physical_page_to_hhdm(physical);
    for (size_t index = 0; index < MAX_CONTROLLERS; index++) {
        if (!pci_find_class(0x04, 0x03, 0x00, index, &controller)) {
            break;
        }
        if (try_controller(index, memory, physical)) {
            ready = true;
            console_write_string("OPOS: HDA sound ready\n");
            return true;
        }
    }
    for (uint32_t i = 0; i < 1 + RING_BYTES / 4096; i++) {
        (void)physical_page_free(physical + (uint64_t)i * 4096);
    }
    if (probe_count == 0) {
        probe_line("no HDA controller", 0, 0, NULL);
    }
    return false;
}

bool hda_ready(void) {
    return ready;
}

void hda_set_source(hda_source new_source) {
    source = new_source != NULL ? new_source : silence;
}

void hda_poll(void) {
    if (!ready) {
        return;
    }
    uint32_t lpib = read32(stream_base + SD_LPIB) % RING_BYTES;
    uint32_t advanced = (lpib + RING_BYTES - last_lpib) % RING_BYTES;
    last_lpib = lpib;
    played_bytes += advanced;
    /* Never fall behind: if the writer was late, skip ahead. */
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
        int16_t *out = (int16_t *)(ring + at);
        size_t frames = chunk / FRAME_BYTES;
        source(out, frames);
        add_beep(out, frames);
        cache_flush(out, chunk);
        written_bytes += chunk;
    }
}

bool hda_beep(uint32_t frequency, uint32_t milliseconds) {
    if (!ready || frequency < 20 || frequency > 20000) {
        return false;
    }
    beep_period = HDA_SAMPLE_RATE / frequency;
    beep_phase = 0;
    beep_frames_left = HDA_SAMPLE_RATE / 1000 * milliseconds;
    return true;
}

uint64_t hda_frames_played(void) {
    return played_bytes / FRAME_BYTES;
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

void hda_describe(void (*print)(const char *line)) {
    char line[96];
    size_t used = 0;
    if (!ready) {
        print("Sound: no HDA controller with an output.");
        for (size_t i = 0; i < probe_count; i++) {
            print(probe_log[i]);
        }
        return;
    }
    append(line, &used, sizeof(line), "Sound: HDA controller ");
    append_hex(line, &used, sizeof(line), controller.vendor_id, 4);
    append(line, &used, sizeof(line), ":");
    append_hex(line, &used, sizeof(line), controller.device_id, 4);
    append(line, &used, sizeof(line), " at ");
    append_hex(line, &used, sizeof(line), controller.bus, 2);
    append(line, &used, sizeof(line), ":");
    append_hex(line, &used, sizeof(line), controller.device, 2);
    append(line, &used, sizeof(line), ".");
    append_hex(line, &used, sizeof(line), controller.function, 1);
    append(line, &used, sizeof(line), ", codec ");
    append_hex(line, &used, sizeof(line), codec_address, 1);
    append(line, &used, sizeof(line), " ");
    append_hex(line, &used, sizeof(line), codec_vendor, 8);
    print(line);
    static const char *const devices[] = {"line out", "speaker",
                                          "headphones"};
    for (size_t i = 0; i < output_count; i++) {
        used = 0;
        const struct output_path *path = &outputs[i];
        append(line, &used, sizeof(line), "  ");
        append(line, &used, sizeof(line),
               path->device <= 2 ? devices[path->device] : "output");
        append(line, &used, sizeof(line), ": pin ");
        for (uint8_t k = 0; k < path->length; k++) {
            if (k != 0) {
                append(line, &used, sizeof(line), " <- ");
            }
            append_hex(line, &used, sizeof(line), path->nodes[k], 2);
        }
        print(line);
    }
    used = 0;
    append(line, &used, sizeof(line), "  48000 Hz, 16-bit stereo; played ");
    append_decimal(line, &used, sizeof(line), hda_frames_played());
    append(line, &used, sizeof(line), " frames");
    print(line);
}
