#include "xhci.h"

#include "io.h"
#include "memory.h"
#include "paging.h"
#include "pci.h"
#include "usb_storage.h"
#include "timer.h"

/*
 * xHCI 1.x host controller driver. Everything is polled: the event ring
 * is read directly instead of waiting for an interrupt. One command or
 * transfer is in flight at a time, which keeps the bookkeeping small.
 */

#define PAGE_SIZE 4096
#define XHCI_MMIO_BASE 0xffffc00000200000ULL
#define XHCI_MMIO_MIN_PAGES 16
#define XHCI_MMIO_MAX_PAGES 64

/* Capability registers. */
#define CAP_LENGTH 0x00
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF 0x14
#define CAP_RTSOFF 0x18
#define HCC_64BIT (1u << 0)
#define HCC_CONTEXT_64 (1u << 2)

/* Operational registers. */
#define OP_USBCMD 0x00
#define OP_USBSTS 0x04
#define OP_CRCR 0x18
#define OP_DCBAAP 0x30
#define OP_CONFIG 0x38
#define OP_PORTSC 0x400
#define USBCMD_RUN (1u << 0)
#define USBCMD_RESET (1u << 1)
#define USBSTS_HALTED (1u << 0)
#define USBSTS_NOT_READY (1u << 11)

/* Runtime registers of interrupter 0. */
#define RT_ERSTSZ 0x28
#define RT_ERSTBA 0x30
#define RT_ERDP 0x38
#define ERDP_BUSY (1u << 3)

/* PORTSC. Writing 1 to PORT_ENABLED disables the port and writing 1 to a
   change bit clears it, so writes keep only PORT_PRESERVE plus the bits
   meant to be set. */
#define PORT_CONNECTED (1u << 0)
#define PORT_ENABLED (1u << 1)
#define PORT_RESET (1u << 4)
#define PORT_SPEED(value) (((value) >> 10) & 0x0f)
#define PORT_CONNECT_CHANGE (1u << 17)
#define PORT_RESET_CHANGE (1u << 21)
#define PORT_CHANGE_BITS 0x00fe0000u
#define PORT_PRESERVE 0x0e00c200u

#define SPEED_FULL 1
#define SPEED_LOW 2
#define SPEED_HIGH 3
#define SPEED_SUPER 4

/* TRBs. */
#define TRB_NORMAL 1
#define TRB_SETUP 2
#define TRB_DATA 3
#define TRB_STATUS 4
#define TRB_LINK 6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEVICE 11
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVALUATE_CONTEXT 13
#define TRB_RESET_ENDPOINT 14
#define TRB_SET_DEQUEUE 16
#define TRB_TRANSFER_EVENT 32
#define TRB_COMMAND_COMPLETION 33
#define TRB_TYPE(type) ((uint32_t)(type) << 10)
#define TRB_CYCLE (1u << 0)
#define TRB_TOGGLE (1u << 1)
#define TRB_ISP (1u << 2)
#define TRB_CHAIN (1u << 4)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_DIRECTION_IN (1u << 16)
#define COMPLETION_SUCCESS 1
#define COMPLETION_STALL 6
#define COMPLETION_SHORT 13

#define RING_TRBS 256
/* Timeouts, in milliseconds of TSC time (calibrated against the PIT;
   counting port 0x80 waits is far too slow under KVM). */
/* The desktop polls USB from its main loop, so nothing here may block
   for long: a slow device must not freeze the screen. */
#define TIMEOUT_MS 3000          /* controller start-up only */
#define COMMAND_TIMEOUT_MS 500
#define CONTROL_TIMEOUT_MS 1000
/* Bulk: long enough for a USB hard disk to spin up. A device that
   misses it is dropped until it is plugged in again. */
#define BULK_TIMEOUT_MS 5000
/* Port bring-up runs across polls instead of waiting in place. */
#define PORT_RESET_MS 500
#define PORT_LINK_MS 1000
#define PORT_ATTEMPTS 5
/* At boot, how long to keep polling so sticks already plugged in are
   ready before the desktop starts. */
#define BOOT_SETTLE_MS 1500
/* Control transfers use the small buffer page from here on. */
#define CONTROL_OFFSET 1024
#define CONTROL_LIMIT (PAGE_SIZE - CONTROL_OFFSET)

/* USB requests and descriptors. */
#define REQUEST_GET_DESCRIPTOR 6
#define REQUEST_SET_CONFIGURATION 9
#define REQUEST_CLEAR_FEATURE 1
#define DESCRIPTOR_DEVICE 1
#define DESCRIPTOR_CONFIGURATION 2
#define DESCRIPTOR_INTERFACE 4
#define DESCRIPTOR_ENDPOINT 5
#define DESCRIPTOR_SS_COMPANION 0x30

struct trb {
    volatile uint32_t d[4];
};

struct ring {
    struct trb *trbs;
    uint64_t physical;
    uint32_t enqueue;
    uint32_t cycle;
};

struct usb_device {
    bool used;
    bool storage;
    uint8_t slot;
    uint8_t port;           /* 1-based root port */
    uint8_t speed;
    uint8_t *output;        /* device context */
    uint64_t output_physical;
    uint8_t *input;         /* input context */
    uint64_t input_physical;
    struct ring control;
    struct ring bulk_in;
    struct ring bulk_out;
    uint8_t in_dci;
    uint8_t out_dci;
    uint8_t in_address;
    uint8_t out_address;
    /* A transfer timed out: the device is dropped at the next poll and
       its port waits for a replug. */
    bool broken;
};

static struct usb_device devices[XHCI_MAX_DEVICES];
enum port_state {
    PORT_IDLE,
    PORT_RESETTING,  /* USB 2: reset issued, waiting for enable */
    PORT_WAIT_LINK,  /* USB 3: waiting for link training to enable it */
    PORT_BACKOFF,    /* an attempt failed; waiting before the next one */
    PORT_FAILED,     /* gave up until the device is plugged in again */
};
static uint8_t port_state[256];
static uint8_t port_attempts[256];
static uint64_t port_deadline[256];
/* From the Supported Protocol capabilities. */
static bool port_usb3[256];

static volatile uint8_t *mmio;
static volatile uint8_t *operational;
static volatile uint8_t *runtime;
static volatile uint32_t *doorbells;
static bool controller_ready;
static bool addressing_64;
static uint32_t context_size;
static uint32_t port_count;
static uint32_t slots_enabled;

static volatile uint64_t *dcbaa;
static struct ring command_ring;
static struct trb *events;
static uint64_t events_physical;
static uint32_t event_dequeue;
static uint32_t event_cycle;

static uint8_t *transfer_buffer;
static uint64_t transfer_physical;
static uint8_t *small_buffer;
static uint64_t small_physical;


/* ---------------------------------------------------------- timing */

static uint64_t tsc_per_ms;

static uint64_t read_tsc(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static void cpu_relax(void) {
    __asm__ volatile ("pause");
}

/* TSC rate. Never through PIT channel 2: that is the PC speaker's
   channel (gated by port 0x61), and touching it makes VMware stall input
   and screen updates for about 50 s. */
static void calibrate_tsc(void) {
    /* VMware and KVM report the TSC rate in kHz (hypervisor leaf
       0x40000010, EAX). */
    uint32_t eax = 0x40000000;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile ("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (eax >= 0x40000010) {
        eax = 0x40000010;
        ecx = 0;
        __asm__ volatile ("cpuid"
                          : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
        if (eax >= 100000) { /* at least 100 MHz */
            tsc_per_ms = eax;
            return;
        }
    }
    /* Otherwise time 50 ms of OPOS's 100 Hz timer (interrupts are on). */
    tsc_per_ms = 3000000; /* fallback guess: 3 GHz */
    uint64_t give_up = read_tsc() + 4000000000ull; /* > 1 s at <= 4 GHz */
    uint64_t tick = timer_ticks();
    while (timer_ticks() == tick) {
        if ((int64_t)(read_tsc() - give_up) >= 0) {
            return; /* the timer is not running */
        }
        cpu_relax();
    }
    uint64_t start = read_tsc();
    tick = timer_ticks();
    while (timer_ticks() - tick < 5) {
        if ((int64_t)(read_tsc() - give_up) >= 0) {
            return;
        }
        cpu_relax();
    }
    uint64_t measured = (read_tsc() - start) / 50;
    if (measured >= 100000) {
        tsc_per_ms = measured;
    }
}

static uint64_t deadline(uint64_t milliseconds) {
    return read_tsc() + milliseconds * tsc_per_ms;
}

static bool expired(uint64_t limit) {
    return (int64_t)(read_tsc() - limit) >= 0;
}
static uint32_t read32(volatile uint8_t *base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

static void write32(volatile uint8_t *base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}

static void write64(volatile uint8_t *base, uint32_t offset, uint64_t value) {
    write32(base, offset, (uint32_t)value);
    write32(base, offset + 4, (uint32_t)(value >> 32));
}

static void barrier(void) {
    __asm__ volatile ("mfence" : : : "memory");
}

static void zero(void *memory, size_t length) {
    uint8_t *bytes = memory;
    for (size_t i = 0; i < length; i++) {
        bytes[i] = 0;
    }
}

static void *dma_page(uint64_t *physical) {
    uint64_t page = physical_page_alloc();
    if (page == 0) {
        return NULL;
    }
    if (!addressing_64 && page + PAGE_SIZE - 1 > UINT32_MAX) {
        (void)physical_page_free(page);
        return NULL;
    }
    void *virtual = physical_page_to_hhdm(page);
    if (virtual == NULL) {
        (void)physical_page_free(page);
        return NULL;
    }
    zero(virtual, PAGE_SIZE);
    *physical = page;
    return virtual;
}

static void dma_free(uint64_t *physical) {
    if (*physical != 0) {
        (void)physical_page_free(*physical);
        *physical = 0;
    }
}

/* ------------------------------------------------------------- rings */

static bool ring_create(struct ring *ring) {
    ring->trbs = dma_page(&ring->physical);
    if (ring->trbs == NULL) {
        return false;
    }
    ring->enqueue = 0;
    ring->cycle = 1;
    struct trb *link = &ring->trbs[RING_TRBS - 1];
    link->d[0] = (uint32_t)ring->physical;
    link->d[1] = (uint32_t)(ring->physical >> 32);
    link->d[2] = 0;
    link->d[3] = TRB_TYPE(TRB_LINK) | TRB_TOGGLE;
    return true;
}

static void ring_free(struct ring *ring) {
    dma_free(&ring->physical);
    *ring = (struct ring){0};
}

/* Queues one TRB (its cycle bit is set here); returns its address. */
static uint64_t ring_push(struct ring *ring, uint32_t d0, uint32_t d1,
                          uint32_t d2, uint32_t d3) {
    struct trb *trb = &ring->trbs[ring->enqueue];
    uint64_t physical = ring->physical + (uint64_t)ring->enqueue * 16;
    trb->d[0] = d0;
    trb->d[1] = d1;
    trb->d[2] = d2;
    barrier();
    trb->d[3] = (d3 & ~TRB_CYCLE) | ring->cycle;
    if (++ring->enqueue == RING_TRBS - 1) {
        struct trb *link = &ring->trbs[RING_TRBS - 1];
        barrier();
        link->d[3] = TRB_TYPE(TRB_LINK) | TRB_TOGGLE | (d3 & TRB_CHAIN)
            | ring->cycle;
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
    return physical;
}

static bool event_pop(struct trb *event) {
    struct trb *trb = &events[event_dequeue];
    if ((trb->d[3] & TRB_CYCLE) != event_cycle) {
        return false;
    }
    barrier();
    for (size_t i = 0; i < 4; i++) {
        event->d[i] = trb->d[i];
    }
    if (++event_dequeue == RING_TRBS) {
        event_dequeue = 0;
        event_cycle ^= 1;
    }
    write64(runtime, RT_ERDP,
            (events_physical + (uint64_t)event_dequeue * 16) | ERDP_BUSY);
    return true;
}

/* Waits for a command completion (for the command TRB at `command`) or a
   transfer event (for slot/dci). Other events are dropped. */
static bool wait_event(uint32_t type, uint64_t command, uint8_t slot,
                       uint8_t dci, uint64_t timeout_ms, struct trb *event) {
    for (uint64_t limit = deadline(timeout_ms); !expired(limit);) {
        struct trb found;
        if (!event_pop(&found)) {
            cpu_relax();
            continue;
        }
        if (((found.d[3] >> 10) & 0x3f) != type) {
            continue;
        }
        if (type == TRB_COMMAND_COMPLETION) {
            uint64_t pointer = found.d[0] | ((uint64_t)found.d[1] << 32);
            if (pointer != command) {
                continue;
            }
        } else if ((found.d[3] >> 24) != slot
                   || ((found.d[3] >> 16) & 0x1f) != dci) {
            continue;
        }
        *event = found;
        return true;
    }
    return false;
}

static bool run_command(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
                        struct trb *completion) {
    uint64_t at = ring_push(&command_ring, d0, d1, d2, d3);
    barrier();
    doorbells[0] = 0;
    struct trb event;
    if (!wait_event(TRB_COMMAND_COMPLETION, at, 0, 0, COMMAND_TIMEOUT_MS,
                    &event)) {
        return false;
    }
    if (completion != NULL) {
        *completion = event;
    }
    return (event.d[2] >> 24) == COMPLETION_SUCCESS;
}

/* --------------------------------------------------------- contexts */

static volatile uint32_t *input_control(struct usb_device *device) {
    return (volatile uint32_t *)device->input;
}

static volatile uint32_t *input_slot(struct usb_device *device) {
    return (volatile uint32_t *)(device->input + context_size);
}

static volatile uint32_t *input_endpoint(struct usb_device *device,
                                         uint8_t dci) {
    return (volatile uint32_t *)(device->input
                                 + context_size * ((size_t)dci + 1));
}

static void set_endpoint(struct usb_device *device, uint8_t dci,
                         uint32_t type, uint32_t max_packet, uint32_t burst,
                         const struct ring *ring, uint32_t average) {
    volatile uint32_t *endpoint = input_endpoint(device, dci);
    endpoint[1] = (3u << 1) | (type << 3) | (burst << 8)
        | (max_packet << 16);
    endpoint[2] = (uint32_t)ring->physical | 1u; /* dequeue cycle state */
    endpoint[3] = (uint32_t)(ring->physical >> 32);
    endpoint[4] = average;
}

/* After a stall: reset the endpoint and move its dequeue pointer past the
   failed transfer. */
static void endpoint_reset(struct usb_device *device, uint8_t dci,
                           const struct ring *ring) {
    (void)run_command(0, 0, 0,
                      TRB_TYPE(TRB_RESET_ENDPOINT)
                          | ((uint32_t)device->slot << 24)
                          | ((uint32_t)dci << 16),
                      NULL);
    uint64_t dequeue = ring->physical + (uint64_t)ring->enqueue * 16;
    (void)run_command((uint32_t)dequeue | ring->cycle,
                      (uint32_t)(dequeue >> 32), 0,
                      TRB_TYPE(TRB_SET_DEQUEUE)
                          | ((uint32_t)device->slot << 24)
                          | ((uint32_t)dci << 16),
                      NULL);
}

/* ------------------------------------------------------- transfers */

static enum usb_result control(struct usb_device *device, uint8_t type,
                               uint8_t request, uint16_t value,
                               uint16_t index, void *data, uint16_t length) {
    if (device->broken || length > CONTROL_LIMIT
        || (length != 0 && data == NULL)) {
        return USB_ERROR;
    }
    bool in = (type & 0x80) != 0;
    uint8_t *buffer = small_buffer + CONTROL_OFFSET;
    uint64_t physical = small_physical + CONTROL_OFFSET;
    if (!in) {
        for (uint16_t i = 0; i < length; i++) {
            buffer[i] = ((const uint8_t *)data)[i];
        }
    }
    uint32_t transfer_type = length == 0 ? 0 : in ? 3 : 2;
    (void)ring_push(&device->control,
                    type | ((uint32_t)request << 8) | ((uint32_t)value << 16),
                    index | ((uint32_t)length << 16), 8,
                    TRB_TYPE(TRB_SETUP) | TRB_IDT | (transfer_type << 16));
    if (length != 0) {
        (void)ring_push(&device->control, (uint32_t)physical,
                        (uint32_t)(physical >> 32), length,
                        TRB_TYPE(TRB_DATA) | (in ? TRB_DIRECTION_IN : 0));
    }
    (void)ring_push(&device->control, 0, 0, 0,
                    TRB_TYPE(TRB_STATUS) | TRB_IOC
                        | (length != 0 && in ? 0 : TRB_DIRECTION_IN));
    barrier();
    doorbells[device->slot] = 1;
    struct trb event;
    if (!wait_event(TRB_TRANSFER_EVENT, 0, device->slot, 1,
                    CONTROL_TIMEOUT_MS, &event)) {
        device->broken = true;
        return USB_ERROR;
    }
    uint32_t code = event.d[2] >> 24;
    if (code == COMPLETION_STALL) {
        endpoint_reset(device, 1, &device->control);
        return USB_STALL;
    }
    if (code != COMPLETION_SUCCESS && code != COMPLETION_SHORT) {
        return USB_ERROR;
    }
    if (in) {
        barrier();
        for (uint16_t i = 0; i < length; i++) {
            ((uint8_t *)data)[i] = buffer[i];
        }
    }
    return USB_OK;
}

static struct usb_device *storage_device(size_t index) {
    return index < XHCI_MAX_DEVICES && devices[index].used
        && devices[index].storage && !devices[index].broken
        ? &devices[index] : NULL;
}

enum usb_result xhci_bulk(size_t index, bool in, bool small, uint32_t length,
                          uint32_t *done) {
    struct usb_device *device = storage_device(index);
    if (done != NULL) {
        *done = 0;
    }
    if (device == NULL || !controller_ready
        || length > (small ? CONTROL_OFFSET : XHCI_TRANSFER_BUFFER_SIZE)) {
        return USB_ERROR;
    }
    struct ring *ring = in ? &device->bulk_in : &device->bulk_out;
    uint8_t dci = in ? device->in_dci : device->out_dci;
    uint64_t physical = small ? small_physical : transfer_physical;
    (void)ring_push(ring, (uint32_t)physical, (uint32_t)(physical >> 32),
                    length, TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP);
    barrier();
    doorbells[device->slot] = dci;
    struct trb event;
    if (!wait_event(TRB_TRANSFER_EVENT, 0, device->slot, dci,
                    BULK_TIMEOUT_MS, &event)) {
        device->broken = true;
        return USB_ERROR;
    }
    uint32_t code = event.d[2] >> 24;
    uint32_t residual = event.d[2] & 0x00ffffffu;
    if (code == COMPLETION_STALL) {
        return USB_STALL;
    }
    if (code != COMPLETION_SUCCESS && code != COMPLETION_SHORT) {
        return USB_ERROR;
    }
    barrier();
    if (done != NULL) {
        *done = residual <= length ? length - residual : 0;
    }
    return USB_OK;
}

bool xhci_clear_halt(size_t index, bool in) {
    struct usb_device *device = storage_device(index);
    if (device == NULL) {
        return false;
    }
    uint8_t dci = in ? device->in_dci : device->out_dci;
    endpoint_reset(device, dci, in ? &device->bulk_in : &device->bulk_out);
    /* ENDPOINT_HALT feature, recipient endpoint. */
    return control(device, 0x02, REQUEST_CLEAR_FEATURE, 0,
                   in ? device->in_address : device->out_address, NULL, 0)
        == USB_OK;
}

bool xhci_control_no_data(size_t index, uint8_t request_type,
                          uint8_t request, uint16_t value, uint16_t index_) {
    struct usb_device *device = storage_device(index);
    return device != NULL
        && control(device, request_type, request, value, index_, NULL, 0)
            == USB_OK;
}

uint8_t xhci_device_port(size_t index) {
    return index < XHCI_MAX_DEVICES && devices[index].used
        ? devices[index].port : 0;
}

uint8_t *xhci_transfer_buffer(void) {
    return transfer_buffer;
}

uint8_t *xhci_small_buffer(void) {
    return small_buffer;
}

bool xhci_ready(void) {
    return controller_ready;
}

/* ------------------------------------------------------ enumeration */

static uint32_t port_read(uint32_t port) {
    return read32(operational, OP_PORTSC + 0x10 * port);
}

static void port_write(uint32_t port, uint32_t value) {
    write32(operational, OP_PORTSC + 0x10 * port, value);
}

static void device_free(struct usb_device *device) {
    if (device->slot != 0) {
        dcbaa[device->slot] = 0;
    }
    dma_free(&device->output_physical);
    dma_free(&device->input_physical);
    ring_free(&device->control);
    ring_free(&device->bulk_in);
    ring_free(&device->bulk_out);
    *device = (struct usb_device){0};
}

static void detach(size_t index) {
    struct usb_device *device = &devices[index];
    if (device->storage) {
        usb_storage_detach(index);
    }
    if (device->slot != 0) {
        (void)run_command(0, 0, 0,
                          TRB_TYPE(TRB_DISABLE_SLOT)
                              | ((uint32_t)device->slot << 24),
                          NULL);
    }
    device_free(device);
}

/* Finds a bulk-only SCSI mass-storage interface and its two bulk
   endpoints in a configuration descriptor. */
struct storage_interface {
    uint8_t interface;
    uint8_t in_address;
    uint8_t out_address;
    uint16_t in_packet;
    uint16_t out_packet;
    uint8_t in_burst;
    uint8_t out_burst;
};

static bool find_storage(const uint8_t *config, size_t length,
                         struct storage_interface *found) {
    bool inside = false;
    *found = (struct storage_interface){0};
    uint8_t *last_burst = NULL;
    for (size_t at = 0; at + 2 <= length;) {
        uint8_t size = config[at];
        uint8_t type = config[at + 1];
        if (size < 2 || at + size > length) {
            break;
        }
        if (type == DESCRIPTOR_INTERFACE && size >= 9) {
            if (inside && found->in_address != 0 && found->out_address != 0) {
                return true;
            }
            /* Class 8 (mass storage), subclass 6 (SCSI), protocol 0x50
               (bulk-only). */
            inside = config[at + 5] == 8 && config[at + 6] == 6
                && config[at + 7] == 0x50 && config[at + 3] == 0;
            if (inside) {
                *found = (struct storage_interface){
                    .interface = config[at + 2],
                };
            }
            last_burst = NULL;
        } else if (type == DESCRIPTOR_ENDPOINT && size >= 7 && inside
                   && (config[at + 3] & 0x03) == 2) {
            uint8_t address = config[at + 2];
            uint16_t packet = (uint16_t)(config[at + 4]
                                         | (config[at + 5] << 8)) & 0x7ff;
            if ((address & 0x80) != 0) {
                found->in_address = address;
                found->in_packet = packet;
                last_burst = &found->in_burst;
            } else {
                found->out_address = address;
                found->out_packet = packet;
                last_burst = &found->out_burst;
            }
        } else if (type == DESCRIPTOR_SS_COMPANION && size >= 3
                   && last_burst != NULL) {
            *last_burst = config[at + 2];
            last_burst = NULL;
        }
        at += size;
    }
    return inside && found->in_address != 0 && found->out_address != 0;
}

static bool configure_storage(struct usb_device *device,
                              const struct storage_interface *storage,
                              uint8_t configuration) {
    if (control(device, 0x00, REQUEST_SET_CONFIGURATION, configuration, 0,
                NULL, 0) != USB_OK
        || !ring_create(&device->bulk_in)
        || !ring_create(&device->bulk_out)) {
        return false;
    }
    device->in_address = storage->in_address;
    device->out_address = storage->out_address;
    device->in_dci = (uint8_t)((storage->in_address & 0x0f) * 2 + 1);
    device->out_dci = (uint8_t)((storage->out_address & 0x0f) * 2);
    uint8_t last = device->in_dci > device->out_dci ? device->in_dci
                                                    : device->out_dci;
    zero(device->input, PAGE_SIZE);
    input_control(device)[1] = 1u | (1u << device->in_dci)
        | (1u << device->out_dci);
    /* The slot context as the controller has it, with more entries. */
    volatile uint32_t *slot = input_slot(device);
    const volatile uint32_t *current = (const volatile uint32_t *)device->output;
    for (size_t i = 0; i < 8; i++) {
        slot[i] = current[i];
    }
    slot[0] = (slot[0] & ~(0x1fu << 27)) | ((uint32_t)last << 27);
    slot[3] = 0; /* device address and slot state are output only */
    set_endpoint(device, device->in_dci, 6, storage->in_packet,
                 storage->in_burst, &device->bulk_in, 3072);
    set_endpoint(device, device->out_dci, 2, storage->out_packet,
                 storage->out_burst, &device->bulk_out, 3072);
    return run_command((uint32_t)device->input_physical,
                       (uint32_t)(device->input_physical >> 32), 0,
                       TRB_TYPE(TRB_CONFIGURE_ENDPOINT)
                           | ((uint32_t)device->slot << 24),
                       NULL);
}

/* Addresses the device on an enabled port and, for mass storage,
   configures it. False when it failed (the port is tried again later). */
static bool attach(uint32_t port) {
    uint32_t status = port_read(port);
    /* Clear the reset/enable/link change bits, but never the connect
       change bit: xhci_poll must see a replug. */
    port_write(port, (status & PORT_PRESERVE)
                         | (status & PORT_CHANGE_BITS & ~PORT_CONNECT_CHANGE));
    if ((status & (PORT_CONNECTED | PORT_ENABLED))
        != (PORT_CONNECTED | PORT_ENABLED)) {
        return false;
    }
    size_t index = 0;
    while (index < XHCI_MAX_DEVICES && devices[index].used) {
        index++;
    }
    if (index == XHCI_MAX_DEVICES) {
        return false;
    }
    struct usb_device *device = &devices[index];
    *device = (struct usb_device){0};
    struct trb completion;
    if (!run_command(0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &completion)) {
        return false;
    }
    device->slot = (uint8_t)(completion.d[3] >> 24);
    device->port = (uint8_t)(port + 1);
    device->speed = (uint8_t)PORT_SPEED(status);
    device->used = true;
    if (device->slot == 0 || device->slot > slots_enabled) {
        device->slot = 0;
        device_free(device);
        return false;
    }
    device->output = dma_page(&device->output_physical);
    device->input = dma_page(&device->input_physical);
    if (device->output == NULL || device->input == NULL
        || !ring_create(&device->control)) {
        detach(index);
        return false;
    }
    dcbaa[device->slot] = device->output_physical;

    uint32_t packet = device->speed >= SPEED_SUPER ? 512
        : device->speed == SPEED_HIGH ? 64 : 8;
    input_control(device)[1] = 0x3; /* slot and endpoint 0 */
    input_slot(device)[0] = (1u << 27) | ((uint32_t)device->speed << 20);
    input_slot(device)[1] = (uint32_t)device->port << 16;
    set_endpoint(device, 1, 4, packet, 0, &device->control, 8);
    if (!run_command((uint32_t)device->input_physical,
                     (uint32_t)(device->input_physical >> 32), 0,
                     TRB_TYPE(TRB_ADDRESS_DEVICE)
                         | ((uint32_t)device->slot << 24),
                     NULL)) {
        detach(index);
        return false;
    }

    uint8_t descriptor[18];
    if (control(device, 0x80, REQUEST_GET_DESCRIPTOR,
                DESCRIPTOR_DEVICE << 8, 0, descriptor, 8) != USB_OK) {
        detach(index);
        return false;
    }
    uint32_t real_packet = device->speed >= SPEED_SUPER
        ? 1u << (descriptor[7] & 0x0f) : descriptor[7];
    if (real_packet != packet && real_packet >= 8) {
        /* Full-speed devices may use 8, 16, 32 or 64 bytes. */
        zero(device->input, PAGE_SIZE);
        input_control(device)[1] = 0x2;
        set_endpoint(device, 1, 4, real_packet, 0, &device->control, 8);
        (void)run_command((uint32_t)device->input_physical,
                          (uint32_t)(device->input_physical >> 32), 0,
                          TRB_TYPE(TRB_EVALUATE_CONTEXT)
                              | ((uint32_t)device->slot << 24),
                          NULL);
    }
    static uint8_t config[CONTROL_LIMIT];
    if (control(device, 0x80, REQUEST_GET_DESCRIPTOR,
                DESCRIPTOR_CONFIGURATION << 8, 0, config, 9) != USB_OK) {
        detach(index);
        return false;
    }
    uint16_t total = (uint16_t)(config[2] | (config[3] << 8));
    if (total > CONTROL_LIMIT) {
        total = CONTROL_LIMIT;
    }
    if (total < 9
        || control(device, 0x80, REQUEST_GET_DESCRIPTOR,
                   DESCRIPTOR_CONFIGURATION << 8, 0, config, total)
               != USB_OK) {
        detach(index);
        return false;
    }
    struct storage_interface storage;
    if (!find_storage(config, total, &storage)) {
        /* No driver for it (VMware's virtual mouse/keyboard, hubs...):
           release it and disable the port, as if nothing were there. */
        detach(index);
        uint32_t now = port_read(port);
        port_write(port, (now & PORT_PRESERVE) | PORT_ENABLED);
        return true;
    }
    if (!configure_storage(device, &storage, config[5])) {
        detach(index);
        return false;
    }
    device->storage = true;
    if (!usb_storage_attach(index, storage.interface)) {
        /* Often a stick that is not ready yet: drop it and let the port
           try again after a pause. */
        device->storage = false;
        detach(index);
        return false;
    }
    return true;
}

static size_t device_on_port(uint32_t port) {
    for (size_t i = 0; i < XHCI_MAX_DEVICES; i++) {
        if (devices[i].used && devices[i].port == port + 1) {
            return i;
        }
    }
    return XHCI_MAX_DEVICES;
}

static void port_failed_attempt(uint32_t port) {
    if (++port_attempts[port] >= PORT_ATTEMPTS) {
        port_state[port] = PORT_FAILED;
    } else {
        /* Back off before the next try (e.g. a stick not ready yet). */
        port_state[port] = PORT_BACKOFF;
        port_deadline[port] = deadline(PORT_LINK_MS);
    }
}

/* One step for one port; never waits for the port itself. */
static bool poll_port(uint32_t port) {
    bool changed = false;
    uint32_t status = port_read(port);
    bool connected = (status & PORT_CONNECTED) != 0;
    size_t index = device_on_port(port);
    if ((status & PORT_CONNECT_CHANGE) != 0) {
        port_write(port, (status & PORT_PRESERVE) | PORT_CONNECT_CHANGE);
        if (index != XHCI_MAX_DEVICES) {
            detach(index);
            index = XHCI_MAX_DEVICES;
            changed = true;
        }
        port_state[port] = PORT_IDLE;
        port_attempts[port] = 0;
    }
    if (index != XHCI_MAX_DEVICES && (!connected || devices[index].broken)) {
        bool broken = devices[index].broken;
        detach(index);
        index = XHCI_MAX_DEVICES;
        changed = true;
        if (connected && broken) {
            port_state[port] = PORT_FAILED;
        }
    }
    if (!connected) {
        port_state[port] = PORT_IDLE;
        port_attempts[port] = 0;
        return changed;
    }
    if (index != XHCI_MAX_DEVICES || port_state[port] == PORT_FAILED) {
        return changed;
    }
    if (port_state[port] == PORT_BACKOFF) {
        if (!expired(port_deadline[port])) {
            return changed;
        }
        port_state[port] = PORT_IDLE;
    }
    if ((status & PORT_ENABLED) != 0) {
        port_state[port] = PORT_IDLE;
        if (attach(port)) {
            if (device_on_port(port) == XHCI_MAX_DEVICES) {
                /* Unsupported device: leave it alone until replugged. */
                port_state[port] = PORT_FAILED;
            }
            changed = true;
        } else {
            port_failed_attempt(port);
        }
        return changed;
    }
    if (port_state[port] == PORT_WAIT_LINK
        || port_state[port] == PORT_RESETTING) {
        if (!expired(port_deadline[port])) {
            return changed;
        }
        if (port_state[port] == PORT_RESETTING) {
            port_failed_attempt(port);
            return changed;
        }
        /* The USB 3 link did not come up by itself: reset the port. */
    } else if (port_usb3[port]) {
        /* USB 3 ports enable themselves once link training finishes. */
        port_state[port] = PORT_WAIT_LINK;
        port_deadline[port] = deadline(PORT_LINK_MS);
        return changed;
    }
    /* USB 2 ports need a reset to become enabled. */
    port_write(port, (status & PORT_PRESERVE) | PORT_RESET);
    port_state[port] = PORT_RESETTING;
    port_deadline[port] = deadline(PORT_RESET_MS);
    return changed;
}

/* True while some port is part way through being brought up. */
static bool ports_settling(void) {
    for (uint32_t port = 0; port < port_count && port < 256; port++) {
        if (port_state[port] != PORT_IDLE
            && port_state[port] != PORT_FAILED) {
            return true;
        }
    }
    return false;
}

bool xhci_poll(void) {
    if (!controller_ready) {
        return false;
    }
    struct trb stale;
    while (event_pop(&stale)) {
        /* Port status events: the PORTSC registers are read instead. */
    }
    bool changed = false;
    for (uint32_t port = 0; port < port_count && port < 256; port++) {
        if (poll_port(port)) {
            changed = true;
        }
    }
    return changed;
}

/* ---------------------------------------------------------- startup */

static bool under_hypervisor(void) {
    uint32_t eax = 1;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile ("cpuid"
                      : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    return (ecx & (1u << 31)) != 0;
}

static bool map_registers(uint64_t physical, size_t pages) {
    for (size_t page = 0; page < pages; page++) {
        if (!paging_map_page(XHCI_MMIO_BASE + page * PAGE_SIZE,
                             (physical & ~(uint64_t)(PAGE_SIZE - 1))
                                 + page * PAGE_SIZE,
                             PAGING_WRITABLE | PAGING_WRITE_THROUGH
                                 | PAGING_CACHE_DISABLE | PAGING_NO_EXECUTE)) {
            return false;
        }
    }
    return true;
}

/* Walks the extended capabilities: takes the controller from the
   firmware (USB legacy support, which also turns off its SMIs) and notes
   which root ports are USB 3 (Supported Protocol). */
static void scan_capabilities(uint32_t capability_parameters, size_t mapped) {
    uint32_t offset = (capability_parameters >> 16) << 2;
    while (offset != 0 && offset + 12 <= mapped) {
        uint32_t capability = read32(mmio, offset);
        uint8_t id = (uint8_t)capability;
        if (id == 1) {
            write32(mmio, offset, capability | (1u << 24));
            for (uint64_t limit = deadline(TIMEOUT_MS); !expired(limit);) {
                if ((read32(mmio, offset) & (1u << 16)) == 0) {
                    break;
                }
                cpu_relax();
            }
            uint32_t smi = read32(mmio, offset + 4);
            write32(mmio, offset + 4, (smi & ~0x0000e011u) | 0xe0000000u);
        } else if (id == 2) {
            uint8_t major = (uint8_t)(capability >> 24);
            uint32_t ports = read32(mmio, offset + 8);
            uint32_t first = ports & 0xff;    /* 1-based */
            uint32_t count = (ports >> 8) & 0xff;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t port = first - 1 + i;
                if (first != 0 && port < 256) {
                    port_usb3[port] = major >= 3;
                }
            }
        }
        uint32_t next = (capability >> 8) & 0xff;
        if (next == 0) {
            return;
        }
        offset += next << 2;
    }
}

static bool wait_status(uint32_t mask, uint32_t wanted) {
    for (uint64_t limit = deadline(TIMEOUT_MS); !expired(limit);) {
        if ((read32(operational, OP_USBSTS) & mask) == wanted) {
            return true;
        }
        cpu_relax();
    }
    return false;
}

bool xhci_initialize(void) {
    if (controller_ready || !under_hypervisor()) {
        return controller_ready;
    }
    struct pci_device pci;
    if (!pci_find_class(0x0c, 0x03, 0x30, 0, &pci)) {
        return false;
    }
    calibrate_tsc();
    bool io_space = false;
    uint64_t bar = pci_bar_address(&pci, 0, &io_space);
    if (bar == 0 || io_space) {
        return false;
    }
    pci_enable_device(&pci);
    if (!map_registers(bar, 1)) {
        return false;
    }
    mmio = (volatile uint8_t *)(XHCI_MMIO_BASE + (bar & (PAGE_SIZE - 1)));
    uint8_t cap_length = mmio[CAP_LENGTH];
    uint32_t structural1 = read32(mmio, CAP_HCSPARAMS1);
    uint32_t structural2 = read32(mmio, CAP_HCSPARAMS2);
    uint32_t capability = read32(mmio, CAP_HCCPARAMS1);
    uint32_t doorbell_offset = read32(mmio, CAP_DBOFF) & ~0x3u;
    uint32_t runtime_offset = read32(mmio, CAP_RTSOFF) & ~0x1fu;
    port_count = structural1 >> 24;
    uint32_t max_slots = structural1 & 0xff;
    addressing_64 = (capability & HCC_64BIT) != 0;
    context_size = (capability & HCC_CONTEXT_64) != 0 ? 64 : 32;

    size_t needed = cap_length + OP_PORTSC + 0x10 * (size_t)port_count;
    if (runtime_offset + 0x40 > needed) {
        needed = runtime_offset + 0x40;
    }
    if (doorbell_offset + 4 * ((size_t)max_slots + 1) > needed) {
        needed = doorbell_offset + 4 * ((size_t)max_slots + 1);
    }
    size_t pages = (needed + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < XHCI_MMIO_MIN_PAGES) {
        pages = XHCI_MMIO_MIN_PAGES;
    }
    if (pages > XHCI_MMIO_MAX_PAGES || !map_registers(bar, pages)) {
        return false;
    }
    operational = mmio + cap_length;
    runtime = mmio + runtime_offset;
    doorbells = (volatile uint32_t *)(mmio + doorbell_offset);

    scan_capabilities(capability, pages * PAGE_SIZE);
    write32(operational, OP_USBCMD,
            read32(operational, OP_USBCMD) & ~USBCMD_RUN);
    if (!wait_status(USBSTS_HALTED, USBSTS_HALTED)) {
        return false;
    }
    write32(operational, OP_USBCMD, USBCMD_RESET);
    for (uint64_t limit = deadline(TIMEOUT_MS);
         !expired(limit)
         && (read32(operational, OP_USBCMD) & USBCMD_RESET) != 0;) {
        cpu_relax();
    }
    if (!wait_status(USBSTS_NOT_READY, 0)) {
        return false;
    }

    slots_enabled = max_slots < XHCI_MAX_DEVICES ? max_slots
                                                 : XHCI_MAX_DEVICES;
    write32(operational, OP_CONFIG, slots_enabled);

    uint64_t dcbaa_physical;
    dcbaa = dma_page(&dcbaa_physical);
    if (dcbaa == NULL) {
        return false;
    }
    uint32_t scratchpads = ((structural2 >> 27) & 0x1f)
        | (((structural2 >> 21) & 0x1f) << 5);
    if (scratchpads != 0) {
        uint64_t array_physical;
        volatile uint64_t *array = dma_page(&array_physical);
        if (array == NULL || scratchpads > PAGE_SIZE / 8) {
            return false;
        }
        for (uint32_t i = 0; i < scratchpads; i++) {
            uint64_t page;
            if (dma_page(&page) == NULL) {
                return false;
            }
            array[i] = page;
        }
        dcbaa[0] = array_physical;
    }
    write64(operational, OP_DCBAAP, dcbaa_physical);

    if (!ring_create(&command_ring)) {
        return false;
    }
    write64(operational, OP_CRCR, command_ring.physical | 1u);

    events = dma_page(&events_physical);
    uint64_t table_physical;
    volatile uint32_t *table = dma_page(&table_physical);
    if (events == NULL || table == NULL) {
        return false;
    }
    table[0] = (uint32_t)events_physical;
    table[1] = (uint32_t)(events_physical >> 32);
    table[2] = RING_TRBS;
    event_dequeue = 0;
    event_cycle = 1;
    write32(runtime, RT_ERSTSZ, 1);
    write64(runtime, RT_ERDP, events_physical);
    write64(runtime, RT_ERSTBA, table_physical);

    /* 64 KiB aligned inside 31 pages, so no transfer crosses 64 KiB. */
    uint64_t block = physical_pages_alloc_contiguous(31);
    if (block == 0) {
        return false;
    }
    transfer_physical = (block + 0xffff) & ~(uint64_t)0xffff;
    if (!addressing_64
        && transfer_physical + XHCI_TRANSFER_BUFFER_SIZE > UINT32_MAX) {
        return false;
    }
    transfer_buffer = physical_page_to_hhdm(transfer_physical);
    small_buffer = dma_page(&small_physical);
    if (transfer_buffer == NULL || small_buffer == NULL) {
        return false;
    }

    write32(operational, OP_USBCMD, USBCMD_RUN);
    if (!wait_status(USBSTS_HALTED, 0)) {
        return false;
    }
    controller_ready = true;
    /* Bring up what is already plugged in before the desktop starts:
       ports need a few polls (reset, link training). */
    uint64_t limit = deadline(BOOT_SETTLE_MS);
    uint64_t first_look = deadline(100);
    while (!expired(limit)) {
        (void)xhci_poll();
        if (expired(first_look) && !ports_settling()) {
            break;
        }
        for (uint64_t pause = deadline(10); !expired(pause);) {
            cpu_relax();
        }
    }
    return true;
}
