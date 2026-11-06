#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "memory.h"
#include "paging.h"
#include "pci.h"
#include "timer.h"
#include "vmxnet3.h"

/*
 * VMware vmxnet3, revision 1 interface. The driver describes its rings in
 * a "driver shared" block and a queue descriptor table, both in guest
 * memory, and hands their address to the device. Descriptors belong to
 * whichever side's generation bit they carry; the device flips its bit
 * each time a ring wraps, and so do we. Interrupts stay masked.
 *
 * Structure offsets follow Linux's vmxnet3_defs.h.
 */

/* BAR1 ("VD") registers. */
#define VD_VRRS 0x00
#define VD_UVRS 0x08
#define VD_DSAL 0x10
#define VD_DSAH 0x18
#define VD_CMD 0x20
#define VD_MACL 0x28
#define VD_MACH 0x30
#define VD_ECR 0x40
/* BAR0 ("PT") registers. */
#define PT_IMR 0x000
#define PT_TXPROD 0x600
#define PT_RXPROD 0x800
#define PT_RXPROD2 0xa00

#define CMD_ACTIVATE_DEV 0xcafe0000u
#define CMD_RESET_DEV 0xcafe0002u
#define CMD_UPDATE_RX_MODE 0xcafe0003u
#define CMD_GET_LINK 0xf00d0002u

#define SHARED_MAGIC 0xbabefee1u
#define RXM_UCAST 0x01
#define RXM_BCAST 0x04
#define IC_DISABLE_ALL 0x01

/* Driver shared block offsets. */
#define DS_MAGIC 0
#define DS_VERSION 8
#define DS_GOS 12
#define DS_REV_SUPPORT 16
#define DS_UPT_SUPPORT 20
#define DS_QUEUE_DESC_PA 40
#define DS_QUEUE_DESC_LEN 52
#define DS_MTU 56
#define DS_MAX_RX_SG 60
#define DS_TX_QUEUES 62
#define DS_RX_QUEUES 63
#define DS_AUTO_MASK 80
#define DS_NUM_INTRS 81
#define DS_EVENT_INTR 82
#define DS_INTR_CTRL 108
#define DS_RX_MODE 120
#define DS_SIZE 720

/* Queue descriptor offsets (256 bytes per queue, tx first). */
#define QD_TX 0x000
#define QD_RX 0x100
#define QD_LEN 0x200
#define TXQ_THRESHOLD 4
#define TXQ_RING_PA 16
#define TXQ_DATA_RING_PA 24
#define TXQ_COMP_RING_PA 32
#define TXQ_RING_SIZE 56
#define TXQ_DATA_RING_SIZE 60
#define TXQ_COMP_RING_SIZE 64
#define RXQ_RING0_PA 16
#define RXQ_RING1_PA 24
#define RXQ_COMP_RING_PA 32
#define RXQ_RING0_SIZE 56
#define RXQ_RING1_SIZE 60
#define RXQ_COMP_RING_SIZE 64

#define RING_SIZE 32 /* rings must be multiples of 32 */
#define BUFFER_SIZE 2048
#define MAX_FRAME 1514
#define DESCRIPTOR_SIZE 16
#define TX_DATA_DESC_SIZE 128

/* Memory layout (contiguous pages):
   page 0: shared block at 0, queue descriptors at 0x400
   page 1: tx ring 0x000, tx completions 0x200, rx ring 0 0x400,
           rx ring 1 0x600, rx completions 0x800 (64 entries)
   page 2: tx data ring (unused but required)
   then rx buffers (16 pages) and tx buffers (16 pages). */
#define PAGE 0x1000
#define OFFSET_QUEUES 0x400
#define OFFSET_TX_RING (PAGE + 0x000)
#define OFFSET_TX_COMP (PAGE + 0x200)
#define OFFSET_RX_RING0 (PAGE + 0x400)
#define OFFSET_RX_RING1 (PAGE + 0x600)
#define OFFSET_RX_COMP (PAGE + 0x800)
#define OFFSET_TX_DATA (2 * PAGE)
#define OFFSET_RX_BUFFERS (3 * PAGE)
#define OFFSET_TX_BUFFERS (OFFSET_RX_BUFFERS + RING_SIZE * BUFFER_SIZE)
#define TOTAL_PAGES (3 + 2 * (RING_SIZE * BUFFER_SIZE / PAGE))

#define VMXNET3_PT_BASE 0xffffc00000440000ULL
#define VMXNET3_VD_BASE 0xffffc00000448000ULL

static volatile uint8_t *pt;
static volatile uint8_t *vd;
static uint8_t *memory;
static uint64_t memory_physical;
static bool ready;
static uint32_t pci_id;
static uint8_t mac[6];

static size_t tx_fill;       /* next tx descriptor to fill */
static uint32_t tx_gen;      /* generation we write into tx descriptors */
static size_t tx_in_flight;
static size_t tx_comp_next;
static uint32_t tx_comp_gen;
static size_t rx_fill;       /* next rx ring 0 slot to hand to the device */
static uint32_t rx_fill_gen;
static size_t rx_comp_next;
static uint32_t rx_comp_gen;
static uint64_t sent;
static uint64_t received;

static uint32_t vd_read(uint32_t offset) {
    return *(volatile uint32_t *)(vd + offset);
}

static void vd_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(vd + offset) = value;
}

static void pt_write(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(pt + offset) = value;
}

static void put8(size_t offset, uint8_t value) {
    memory[offset] = value;
}

static void put16(size_t offset, uint16_t value) {
    *(volatile uint16_t *)(memory + offset) = value;
}

static void put32(size_t offset, uint32_t value) {
    *(volatile uint32_t *)(memory + offset) = value;
}

static void put64(size_t offset, uint64_t value) {
    *(volatile uint64_t *)(memory + offset) = value;
}

static uint32_t get32(size_t offset) {
    return *(volatile uint32_t *)(memory + offset);
}

static bool map_window(uintptr_t window, const struct pci_device *device,
                       uint8_t bar) {
    bool io_space = false;
    uint64_t address = pci_bar_address(device, bar, &io_space);
    if (address == 0 || io_space) {
        return false;
    }
    return paging_map_page(window, address & ~0xfffULL,
                           PAGING_WRITABLE | PAGING_WRITE_THROUGH
                               | PAGING_CACHE_DISABLE | PAGING_NO_EXECUTE);
}

/* Gives rx ring 0 slot `rx_fill` (with its own buffer) to the device. */
static void rx_give(void) {
    size_t slot = OFFSET_RX_RING0 + rx_fill * DESCRIPTOR_SIZE;
    put64(slot, memory_physical + OFFSET_RX_BUFFERS + rx_fill * BUFFER_SIZE);
    put32(slot + 12, 0);
    __asm__ volatile ("sfence" : : : "memory");
    /* len, btype 0 (head), dtype 0, gen in bit 31. */
    put32(slot + 8, BUFFER_SIZE | rx_fill_gen << 31);
    rx_fill++;
    if (rx_fill == RING_SIZE) {
        rx_fill = 0;
        rx_fill_gen ^= 1;
    }
}

bool vmxnet3_initialize(void) {
    struct pci_device device;
    if (ready) {
        return true;
    }
    if (!pci_find_device(0x15ad, 0x07b0, &device)) {
        return false;
    }
    pci_enable_device(&device);
    if (!map_window(VMXNET3_PT_BASE, &device, 0)
        || !map_window(VMXNET3_VD_BASE, &device, 1)) {
        console_write_string("OPOS: vmxnet3: cannot map registers\n");
        return false;
    }
    bool io_space = false;
    pt = (volatile uint8_t *)(VMXNET3_PT_BASE
                              + (pci_bar_address(&device, 0, &io_space)
                                 & 0xfff));
    vd = (volatile uint8_t *)(VMXNET3_VD_BASE
                              + (pci_bar_address(&device, 1, &io_space)
                                 & 0xfff));
    pci_id = (uint32_t)device.vendor_id << 16 | device.device_id;

    /* Revision 1 of both the device and the UPT interface. */
    if ((vd_read(VD_VRRS) & 1) == 0 || (vd_read(VD_UVRS) & 1) == 0) {
        console_write_string("OPOS: vmxnet3: revision 1 not offered\n");
        return false;
    }
    vd_write(VD_VRRS, 1);
    vd_write(VD_UVRS, 1);
    vd_write(VD_CMD, CMD_RESET_DEV);

    uint32_t low = vd_read(VD_MACL);
    uint32_t high = vd_read(VD_MACH);
    for (int i = 0; i < 4; i++) {
        mac[i] = (uint8_t)(low >> (8 * i));
    }
    mac[4] = (uint8_t)high;
    mac[5] = (uint8_t)(high >> 8);

    memory_physical = physical_pages_alloc_contiguous(TOTAL_PAGES);
    if (memory_physical == 0) {
        console_write_string("OPOS: vmxnet3: out of memory\n");
        return false;
    }
    memory = physical_page_to_hhdm(memory_physical);
    for (size_t i = 0; i < TOTAL_PAGES * PAGE; i++) {
        memory[i] = 0;
    }

    /* Driver shared block. */
    put32(DS_MAGIC, SHARED_MAGIC);
    put32(DS_VERSION, 0x01000000);
    put32(DS_GOS, 2 | 1u << 2); /* 64-bit, "Linux" type */
    put32(DS_REV_SUPPORT, 1);
    put32(DS_UPT_SUPPORT, 1);
    put64(DS_QUEUE_DESC_PA, memory_physical + OFFSET_QUEUES);
    put32(DS_QUEUE_DESC_LEN, QD_LEN);
    put32(DS_MTU, 1500);
    put16(DS_MAX_RX_SG, 1);
    put8(DS_TX_QUEUES, 1);
    put8(DS_RX_QUEUES, 1);
    put8(DS_AUTO_MASK, 1);
    put8(DS_NUM_INTRS, 1);
    put8(DS_EVENT_INTR, 0);
    put32(DS_INTR_CTRL, IC_DISABLE_ALL);
    put32(DS_RX_MODE, RXM_UCAST | RXM_BCAST);

    /* Transmit queue. */
    size_t tx = OFFSET_QUEUES + QD_TX;
    put32(tx + TXQ_THRESHOLD, 1);
    put64(tx + TXQ_RING_PA, memory_physical + OFFSET_TX_RING);
    put64(tx + TXQ_DATA_RING_PA, memory_physical + OFFSET_TX_DATA);
    put64(tx + TXQ_COMP_RING_PA, memory_physical + OFFSET_TX_COMP);
    put32(tx + TXQ_RING_SIZE, RING_SIZE);
    put32(tx + TXQ_DATA_RING_SIZE, RING_SIZE);
    put32(tx + TXQ_COMP_RING_SIZE, RING_SIZE);

    /* Receive queue: ring 0 holds our buffers; ring 1 stays empty. */
    size_t rx = OFFSET_QUEUES + QD_RX;
    put64(rx + RXQ_RING0_PA, memory_physical + OFFSET_RX_RING0);
    put64(rx + RXQ_RING1_PA, memory_physical + OFFSET_RX_RING1);
    put64(rx + RXQ_COMP_RING_PA, memory_physical + OFFSET_RX_COMP);
    put32(rx + RXQ_RING0_SIZE, RING_SIZE);
    put32(rx + RXQ_RING1_SIZE, RING_SIZE);
    put32(rx + RXQ_COMP_RING_SIZE, 2 * RING_SIZE);

    tx_fill = 0;
    tx_gen = 1;
    tx_in_flight = 0;
    tx_comp_next = 0;
    tx_comp_gen = 1;
    rx_fill = 0;
    rx_fill_gen = 1;
    rx_comp_next = 0;
    rx_comp_gen = 1;
    /* All but one slot: a full ring would look empty to the producer
       index. */
    for (size_t i = 0; i + 1 < RING_SIZE; i++) {
        rx_give();
    }
    __asm__ volatile ("mfence" : : : "memory");

    vd_write(VD_DSAL, (uint32_t)memory_physical);
    vd_write(VD_DSAH, (uint32_t)(memory_physical >> 32));
    vd_write(VD_CMD, CMD_ACTIVATE_DEV);
    uint32_t result = vd_read(VD_CMD);
    if (result != 0) {
        console_write_string("OPOS: vmxnet3: activation failed ");
        console_write_hex(result);
        console_write_string("\n");
        return false;
    }
    pt_write(PT_IMR, 1); /* mask interrupt 0 */
    pt_write(PT_RXPROD, (uint32_t)rx_fill);
    pt_write(PT_RXPROD2, 0);
    vd_write(VD_CMD, CMD_UPDATE_RX_MODE);
    ready = true;
    return true;
}

uint32_t vmxnet3_pci_id(void) {
    return ready ? pci_id : 0;
}

void vmxnet3_mac_address(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) {
        out[i] = ready ? mac[i] : 0;
    }
}

bool vmxnet3_link_up(void) {
    if (!ready) {
        return false;
    }
    vd_write(VD_CMD, CMD_GET_LINK);
    return (vd_read(VD_CMD) & 1) != 0;
}

/* Frees transmit slots the device has finished with. */
static void tx_reap(void) {
    for (;;) {
        size_t entry = OFFSET_TX_COMP + tx_comp_next * DESCRIPTOR_SIZE;
        uint32_t last = get32(entry + 12);
        if ((last >> 31) != tx_comp_gen) {
            return;
        }
        __asm__ volatile ("lfence" : : : "memory");
        /* One descriptor per frame, so each completion frees one slot. */
        if (tx_in_flight != 0) {
            tx_in_flight--;
        }
        tx_comp_next++;
        if (tx_comp_next == RING_SIZE) {
            tx_comp_next = 0;
            tx_comp_gen ^= 1;
        }
    }
}

bool vmxnet3_send(const void *frame, size_t length) {
    if (!ready || length == 0 || length > MAX_FRAME) {
        return false;
    }
    tx_reap();
    uint64_t deadline = timer_ticks() + 10;
    while (tx_in_flight + 1 >= RING_SIZE) {
        if (timer_ticks() > deadline) {
            return false;
        }
        tx_reap();
    }
    uint8_t *buffer = memory + OFFSET_TX_BUFFERS + tx_fill * BUFFER_SIZE;
    const uint8_t *bytes = frame;
    for (size_t i = 0; i < length; i++) {
        buffer[i] = bytes[i];
    }
    size_t slot = OFFSET_TX_RING + tx_fill * DESCRIPTOR_SIZE;
    put64(slot, memory_physical + OFFSET_TX_BUFFERS + tx_fill * BUFFER_SIZE);
    /* Write the descriptor with the device's generation, then hand it
       over by flipping the generation bit last. */
    put32(slot + 8, (uint32_t)length | (tx_gen ^ 1) << 14);
    put32(slot + 12, 1u << 12 | 1u << 13); /* end of packet, completion */
    __asm__ volatile ("sfence" : : : "memory");
    put32(slot + 8, (uint32_t)length | tx_gen << 14);
    tx_fill++;
    if (tx_fill == RING_SIZE) {
        tx_fill = 0;
        tx_gen ^= 1;
    }
    tx_in_flight++;
    __asm__ volatile ("mfence" : : : "memory");
    pt_write(PT_TXPROD, (uint32_t)tx_fill);
    sent++;
    return true;
}

size_t vmxnet3_receive(void *out, size_t capacity) {
    if (!ready) {
        return 0;
    }
    for (;;) {
        size_t entry = OFFSET_RX_COMP + rx_comp_next * DESCRIPTOR_SIZE;
        uint32_t last = get32(entry + 12);
        if ((last >> 31) != rx_comp_gen) {
            return 0;
        }
        __asm__ volatile ("lfence" : : : "memory");
        uint32_t first = get32(entry);
        uint32_t third = get32(entry + 8);
        size_t index = first & 0xfff;
        uint32_t queue = first >> 16 & 0x3ff;
        bool whole = (first >> 14 & 1) != 0 && (first >> 15 & 1) != 0
                  && (third >> 14 & 1) == 0;
        size_t length = third & 0x3fff;
        rx_comp_next++;
        if (rx_comp_next == 2 * RING_SIZE) {
            rx_comp_next = 0;
            rx_comp_gen ^= 1;
        }
        if (queue != 0 || index >= RING_SIZE) {
            continue; /* ring 1 is never filled */
        }
        if (whole && length != 0) {
            if (length > capacity) {
                length = capacity;
            }
            const uint8_t *buffer = memory + OFFSET_RX_BUFFERS
                                  + index * BUFFER_SIZE;
            uint8_t *bytes = out;
            for (size_t i = 0; i < length; i++) {
                bytes[i] = buffer[i];
            }
        }
        /* The slot after the last given one is free again (the device
           fills in order), so give it back. */
        rx_give();
        pt_write(PT_RXPROD, (uint32_t)rx_fill);
        if (whole && length != 0) {
            received++;
            return length;
        }
    }
}

uint64_t vmxnet3_sent_count(void) {
    return sent;
}

uint64_t vmxnet3_received_count(void) {
    return received;
}
