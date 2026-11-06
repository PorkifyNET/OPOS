#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "e1000.h"
#include "memory.h"
#include "paging.h"
#include "pci.h"
#include "timer.h"

/*
 * Intel 8254x/82574 programming, the subset every member shares: legacy
 * receive and transmit descriptors, one receive address, no interrupts
 * (the interrupt mask stays all clear and net_poll reads the rings).
 */
#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_EERD 0x0014
#define REG_ICR 0x00c0
#define REG_IMC 0x00d8
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_MTA 0x5200
#define REG_RAL 0x5400
#define REG_RAH 0x5404

#define CTRL_LRST (1u << 3)
#define CTRL_ASDE (1u << 5)
#define CTRL_SLU (1u << 6)
#define CTRL_ILOS (1u << 7)
#define CTRL_RST (1u << 26)
#define CTRL_VME (1u << 30)
#define CTRL_PHY_RST (1u << 31)
#define STATUS_LU (1u << 1)
#define RCTL_EN (1u << 1)
#define RCTL_BAM (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN (1u << 1)
#define TCTL_PSP (1u << 3)
#define TCTL_CT (0x0fu << 4)
#define TCTL_COLD (0x40u << 12)
#define RAH_AV (1u << 31)

#define TX_CMD_EOP 0x01
#define TX_CMD_IFCS 0x02
#define TX_CMD_RS 0x08
#define DESC_DD 0x01
#define RX_STATUS_EOP 0x02

#define RING_SIZE 32
#define BUFFER_SIZE 2048
#define MAX_FRAME 1514

/* 128 KiB register window. */
#define E1000_MMIO_BASE 0xffffc00000400000ULL
#define E1000_MMIO_PAGES 32

struct rx_descriptor {
    uint64_t address;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct tx_descriptor {
    uint64_t address;
    uint16_t length;
    uint8_t cso;
    uint8_t command;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

/* Cards that work with this driver. The 82574L (10d3) takes the EEPROM
   read register with a different layout. */
static const uint16_t supported_ids[] = {
    0x1000, 0x1001, 0x1004, 0x1008, 0x1009, 0x100c, 0x100d, 0x100e,
    0x100f, 0x1010, 0x1011, 0x1012, 0x1013, 0x1015, 0x1016, 0x1017,
    0x1018, 0x1019, 0x101a, 0x101d, 0x101e, 0x1026, 0x1027, 0x1028,
    0x1075, 0x1076, 0x1077, 0x1078, 0x1079, 0x107a, 0x107b, 0x107c,
    0x108a, 0x1099, 0x10b5, 0x10d3,
};

static volatile uint8_t *registers;
static bool ready;
static uint32_t pci_id;
static uint8_t mac[6];
static volatile struct rx_descriptor *rx_ring;
static volatile struct tx_descriptor *tx_ring;
static uint8_t *rx_buffers;
static uint8_t *tx_buffers;
static uint64_t rx_buffers_physical;
static uint64_t tx_buffers_physical;
static size_t rx_next;
static size_t tx_next;
static uint64_t sent;
static uint64_t received;

static uint32_t read32(uint32_t offset) {
    return *(volatile uint32_t *)(registers + offset);
}

static void write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(registers + offset) = value;
}

/* Waits roughly microseconds by reading the status register (each read
   is an uncached bus access of about a microsecond or more). */
static void settle(uint32_t microseconds) {
    for (uint32_t i = 0; i < microseconds; i++) {
        (void)read32(REG_STATUS);
    }
}

static bool eeprom_read(uint8_t word, uint16_t *value, bool e1000e) {
    uint32_t shift = e1000e ? 2 : 8;
    uint32_t done = e1000e ? (1u << 1) : (1u << 4);
    write32(REG_EERD, 1u | ((uint32_t)word << shift));
    for (uint32_t i = 0; i < 100000; i++) {
        uint32_t eerd = read32(REG_EERD);
        if ((eerd & done) != 0) {
            *value = (uint16_t)(eerd >> 16);
            return true;
        }
    }
    return false;
}

static bool find_card(struct pci_device *device) {
    for (size_t i = 0; i < sizeof(supported_ids) / sizeof(supported_ids[0]);
         i++) {
        if (pci_find_device(0x8086, supported_ids[i], device)) {
            return true;
        }
    }
    return false;
}

static bool map_registers(uint64_t physical) {
    for (size_t page = 0; page < E1000_MMIO_PAGES; page++) {
        if (!paging_map_page(E1000_MMIO_BASE + page * 0x1000,
                             (physical & ~0xfffULL) + page * 0x1000,
                             PAGING_WRITABLE | PAGING_WRITE_THROUGH
                                 | PAGING_CACHE_DISABLE
                                 | PAGING_NO_EXECUTE)) {
            return false;
        }
    }
    registers = (volatile uint8_t *)E1000_MMIO_BASE;
    return true;
}

static void read_mac(bool e1000e) {
    uint32_t low = read32(REG_RAL);
    uint32_t high = read32(REG_RAH);
    if ((high & RAH_AV) != 0 && (low != 0 || (high & 0xffff) != 0)) {
        for (int i = 0; i < 4; i++) {
            mac[i] = (uint8_t)(low >> (8 * i));
        }
        mac[4] = (uint8_t)high;
        mac[5] = (uint8_t)(high >> 8);
        return;
    }
    for (uint8_t word = 0; word < 3; word++) {
        uint16_t value = 0;
        (void)eeprom_read(word, &value, e1000e);
        mac[2 * word] = (uint8_t)value;
        mac[2 * word + 1] = (uint8_t)(value >> 8);
    }
}

bool e1000_initialize(void) {
    struct pci_device device;
    if (ready || !find_card(&device)) {
        return ready;
    }
    bool io_space = false;
    uint64_t bar = pci_bar_address(&device, 0, &io_space);
    if (bar == 0 || io_space) {
        console_write_string("OPOS: e1000: no memory BAR\n");
        return false;
    }
    pci_enable_device(&device);
    if (!map_registers(bar)) {
        console_write_string("OPOS: e1000: cannot map registers\n");
        return false;
    }
    bool e1000e = device.device_id == 0x10d3;
    pci_id = (uint32_t)device.vendor_id << 16 | device.device_id;

    /* Ring and buffer memory: 1 page per ring, 16 pages per buffer set. */
    uint64_t pages = 2 + 2 * (RING_SIZE * BUFFER_SIZE / 0x1000);
    uint64_t physical = physical_pages_alloc_contiguous(pages);
    if (physical == 0) {
        console_write_string("OPOS: e1000: out of memory\n");
        return false;
    }
    uint8_t *memory = physical_page_to_hhdm(physical);
    for (uint64_t i = 0; i < pages * 0x1000; i++) {
        memory[i] = 0;
    }
    rx_ring = (volatile struct rx_descriptor *)memory;
    tx_ring = (volatile struct tx_descriptor *)(memory + 0x1000);
    rx_buffers = memory + 0x2000;
    rx_buffers_physical = physical + 0x2000;
    tx_buffers = rx_buffers + RING_SIZE * BUFFER_SIZE;
    tx_buffers_physical = rx_buffers_physical + RING_SIZE * BUFFER_SIZE;

    /* Reset, then mask every interrupt. */
    write32(REG_IMC, 0xffffffffu);
    write32(REG_CTRL, read32(REG_CTRL) | CTRL_RST);
    settle(2000);
    for (uint32_t i = 0; i < 100000 && (read32(REG_CTRL) & CTRL_RST) != 0;
         i++) {
    }
    settle(2000);
    write32(REG_IMC, 0xffffffffu);
    (void)read32(REG_ICR);

    uint32_t ctrl = read32(REG_CTRL);
    ctrl |= CTRL_SLU | CTRL_ASDE;
    ctrl &= ~(CTRL_LRST | CTRL_PHY_RST | CTRL_ILOS | CTRL_VME);
    write32(REG_CTRL, ctrl);

    read_mac(e1000e);
    write32(REG_RAL, (uint32_t)mac[0] | (uint32_t)mac[1] << 8
                         | (uint32_t)mac[2] << 16 | (uint32_t)mac[3] << 24);
    write32(REG_RAH, (uint32_t)mac[4] | (uint32_t)mac[5] << 8 | RAH_AV);
    for (uint32_t i = 0; i < 128; i++) {
        write32(REG_MTA + 4 * i, 0);
    }

    for (size_t i = 0; i < RING_SIZE; i++) {
        rx_ring[i].address = rx_buffers_physical + i * BUFFER_SIZE;
        rx_ring[i].status = 0;
        tx_ring[i].address = tx_buffers_physical + i * BUFFER_SIZE;
        tx_ring[i].status = DESC_DD; /* free */
    }
    write32(REG_RDBAL, (uint32_t)physical);
    write32(REG_RDBAH, (uint32_t)(physical >> 32));
    write32(REG_RDLEN, RING_SIZE * sizeof(struct rx_descriptor));
    write32(REG_RDH, 0);
    write32(REG_RDT, RING_SIZE - 1);
    /* 2048-byte buffers (BSIZE 0), broadcasts accepted, CRC stripped. */
    write32(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);
    rx_next = 0;

    write32(REG_TDBAL, (uint32_t)(physical + 0x1000));
    write32(REG_TDBAH, (uint32_t)((physical + 0x1000) >> 32));
    write32(REG_TDLEN, RING_SIZE * sizeof(struct tx_descriptor));
    write32(REG_TDH, 0);
    write32(REG_TDT, 0);
    write32(REG_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);
    write32(REG_TIPG, 10u | 8u << 10 | 6u << 20);
    tx_next = 0;

    ready = true;
    return true;
}

bool e1000_ready(void) {
    return ready;
}

uint32_t e1000_pci_id(void) {
    return ready ? pci_id : 0;
}

void e1000_mac_address(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) {
        out[i] = ready ? mac[i] : 0;
    }
}

bool e1000_link_up(void) {
    return ready && (read32(REG_STATUS) & STATUS_LU) != 0;
}

bool e1000_send(const void *frame, size_t length) {
    if (!ready || length == 0 || length > MAX_FRAME) {
        return false;
    }
    volatile struct tx_descriptor *descriptor = &tx_ring[tx_next];
    /* The slot is free once the card has set Descriptor Done. */
    uint64_t deadline = timer_ticks() + 10;
    while ((descriptor->status & DESC_DD) == 0) {
        if (timer_ticks() > deadline) {
            return false;
        }
    }
    uint8_t *buffer = tx_buffers + tx_next * BUFFER_SIZE;
    const uint8_t *bytes = frame;
    for (size_t i = 0; i < length; i++) {
        buffer[i] = bytes[i];
    }
    descriptor->length = (uint16_t)length;
    descriptor->cso = 0;
    descriptor->command = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
    descriptor->status = 0;
    descriptor->css = 0;
    descriptor->special = 0;
    tx_next = (tx_next + 1) % RING_SIZE;
    __asm__ volatile ("sfence" : : : "memory");
    write32(REG_TDT, (uint32_t)tx_next);
    sent++;
    return true;
}

size_t e1000_receive(void *out, size_t capacity) {
    if (!ready) {
        return 0;
    }
    for (;;) {
        volatile struct rx_descriptor *descriptor = &rx_ring[rx_next];
        uint8_t status = descriptor->status;
        if ((status & DESC_DD) == 0) {
            return 0;
        }
        __asm__ volatile ("lfence" : : : "memory");
        size_t length = descriptor->length;
        bool whole = (status & RX_STATUS_EOP) != 0 && descriptor->errors == 0;
        if (whole) {
            if (length > capacity) {
                length = capacity;
            }
            const uint8_t *buffer = rx_buffers + rx_next * BUFFER_SIZE;
            uint8_t *bytes = out;
            for (size_t i = 0; i < length; i++) {
                bytes[i] = buffer[i];
            }
        }
        descriptor->status = 0;
        /* Hand the slot back: the tail is the last slot the card may use. */
        write32(REG_RDT, (uint32_t)rx_next);
        rx_next = (rx_next + 1) % RING_SIZE;
        if (whole && length != 0) {
            received++;
            return length;
        }
        /* Frames spread over several buffers or with errors are dropped. */
    }
}

uint64_t e1000_sent_count(void) {
    return sent;
}

uint64_t e1000_received_count(void) {
    return received;
}
