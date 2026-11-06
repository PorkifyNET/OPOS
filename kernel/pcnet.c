#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "io.h"
#include "memory.h"
#include "pci.h"
#include "pcnet.h"
#include "timer.h"

/*
 * AMD PCnet in 32-bit I/O mode (DWIO) with software style 2: 16-byte
 * descriptors holding 32-bit addresses, so every buffer, ring and the
 * initialization block must sit below 4 GiB. Interrupts stay disabled
 * (CSR0 IENA clear); net_poll checks the OWN bits.
 */
#define PORT_APROM 0x00
#define PORT_RDP 0x10
#define PORT_RAP 0x14
#define PORT_RESET 0x18
#define PORT_BDP 0x1c
#define PORT_RESET16 0x14 /* word I/O mode, right after power-up */

#define CSR0_INIT 0x0001
#define CSR0_STRT 0x0002
#define CSR0_STOP 0x0004
#define CSR0_TDMD 0x0008
#define CSR0_IDON 0x0100
#define CSR4_APAD_XMT 0x0800
#define BCR4_LEDOUT 0x8000

#define DESC_OWN 0x8000
#define DESC_ERR 0x4000
#define DESC_STP 0x0200
#define DESC_ENP 0x0100

#define RING_SIZE 32
#define RING_LOG2 5
#define BUFFER_SIZE 1544 /* a frame plus CRC, rounded to 8 */
#define BUFFER_STRIDE 2048
#define MAX_FRAME 1514
#define DESCRIPTOR_SIZE 16

/* Memory: page 0 = init block (0x000), rx ring (0x100), tx ring (0x300);
   then 16 pages of rx buffers and 16 of tx buffers. */
#define PAGE 0x1000
#define OFFSET_INIT 0x000
#define OFFSET_RX_RING 0x100
#define OFFSET_TX_RING 0x300
#define OFFSET_RX_BUFFERS PAGE
#define OFFSET_TX_BUFFERS (PAGE + RING_SIZE * BUFFER_STRIDE)
#define TOTAL_PAGES (1 + 2 * (RING_SIZE * BUFFER_STRIDE / PAGE))

static uint16_t io;
static bool ready;
static bool link_known; /* BCR4 reports the link */
static uint32_t pci_id;
static uint8_t mac[6];
static uint8_t *memory;
static uint32_t memory_physical;
static size_t rx_next;
static size_t tx_next;
static uint64_t sent;
static uint64_t received;

static uint32_t csr_read(uint32_t index) {
    io_out32(io + PORT_RAP, index);
    return io_in32(io + PORT_RDP) & 0xffff;
}

static void csr_write(uint32_t index, uint32_t value) {
    io_out32(io + PORT_RAP, index);
    io_out32(io + PORT_RDP, value);
}

static uint32_t bcr_read(uint32_t index) {
    io_out32(io + PORT_RAP, index);
    return io_in32(io + PORT_BDP) & 0xffff;
}

static void bcr_write(uint32_t index, uint32_t value) {
    io_out32(io + PORT_RAP, index);
    io_out32(io + PORT_BDP, value);
}

static volatile uint32_t *word(size_t offset) {
    return (volatile uint32_t *)(memory + offset);
}

static volatile uint16_t *half(size_t offset) {
    return (volatile uint16_t *)(memory + offset);
}

/* Hands receive slot `index` back to the card. */
static void rx_give(size_t index) {
    size_t slot = OFFSET_RX_RING + index * DESCRIPTOR_SIZE;
    *word(slot) = memory_physical + OFFSET_RX_BUFFERS
                + (uint32_t)index * BUFFER_STRIDE;
    *half(slot + 4) = (uint16_t)(-BUFFER_SIZE) | 0xf000;
    *word(slot + 8) = 0;
    __asm__ volatile ("sfence" : : : "memory");
    *half(slot + 6) = DESC_OWN;
}

bool pcnet_initialize(void) {
    struct pci_device device;
    if (ready) {
        return true;
    }
    if (!pci_find_device(0x1022, 0x2000, &device)) {
        return false;
    }
    bool io_space = false;
    uint64_t bar = pci_bar_address(&device, 0, &io_space);
    if (bar == 0 || !io_space) {
        console_write_string("OPOS: pcnet: no I/O BAR\n");
        return false;
    }
    pci_enable_device(&device);
    io = (uint16_t)bar;
    pci_id = (uint32_t)device.vendor_id << 16 | device.device_id;

    /* Reset (reading the reset register, in both I/O modes), then
       switch to 32-bit I/O by writing a dword to RDP. */
    (void)io_in32(io + PORT_RESET);
    (void)io_in16(io + PORT_RESET16);
    for (int i = 0; i < 100; i++) {
        io_wait();
    }
    io_out32(io + PORT_RDP, 0);
    csr_write(0, CSR0_STOP);
    /* Software style 2: 32-bit PCnet-PCI structures. */
    bcr_write(20, (bcr_read(20) & ~0xffu) | 2);

    /* In 32-bit I/O mode the address PROM answers dword reads only. */
    uint32_t low = io_in32(io + PORT_APROM);
    uint32_t high = io_in32(io + PORT_APROM + 4);
    for (int i = 0; i < 4; i++) {
        mac[i] = (uint8_t)(low >> (8 * i));
    }
    mac[4] = (uint8_t)high;
    mac[5] = (uint8_t)(high >> 8);

    uint64_t physical = physical_pages_alloc_contiguous(TOTAL_PAGES);
    if (physical == 0
        || physical + TOTAL_PAGES * PAGE > 0x100000000ULL) {
        console_write_string("OPOS: pcnet: no memory below 4 GiB\n");
        return false;
    }
    memory_physical = (uint32_t)physical;
    memory = physical_page_to_hhdm(physical);
    for (size_t i = 0; i < TOTAL_PAGES * PAGE; i++) {
        memory[i] = 0;
    }

    for (size_t i = 0; i < RING_SIZE; i++) {
        rx_give(i);
        size_t slot = OFFSET_TX_RING + i * DESCRIPTOR_SIZE;
        *word(slot) = memory_physical + OFFSET_TX_BUFFERS
                    + (uint32_t)i * BUFFER_STRIDE;
        *half(slot + 6) = 0; /* ours */
    }

    /* Initialization block: mode 0, ring sizes, MAC, no multicast. */
    *half(OFFSET_INIT) = 0;
    memory[OFFSET_INIT + 2] = RING_LOG2 << 4;
    memory[OFFSET_INIT + 3] = RING_LOG2 << 4;
    for (int i = 0; i < 6; i++) {
        memory[OFFSET_INIT + 4 + i] = mac[i];
    }
    *word(OFFSET_INIT + 20) = memory_physical + OFFSET_RX_RING;
    *word(OFFSET_INIT + 24) = memory_physical + OFFSET_TX_RING;
    __asm__ volatile ("mfence" : : : "memory");

    uint32_t init = memory_physical + OFFSET_INIT;
    csr_write(1, init & 0xffff);
    csr_write(2, init >> 16);
    /* Pad short frames to 64 bytes in hardware. */
    csr_write(4, csr_read(4) | CSR4_APAD_XMT);
    csr_write(0, CSR0_INIT);
    uint64_t deadline = timer_ticks() + 50;
    while ((csr_read(0) & CSR0_IDON) == 0) {
        if (timer_ticks() > deadline) {
            console_write_string("OPOS: pcnet: initialization timed out\n");
            return false;
        }
    }
    csr_write(0, CSR0_IDON | CSR0_STRT);
    rx_next = 0;
    tx_next = 0;
    /* BCR4 drives LED0, which shows the link by default (LNKSTE). */
    link_known = (bcr_read(4) & 0x00c0) != 0;
    ready = true;
    return true;
}

uint32_t pcnet_pci_id(void) {
    return ready ? pci_id : 0;
}

void pcnet_mac_address(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) {
        out[i] = ready ? mac[i] : 0;
    }
}

bool pcnet_link_up(void) {
    if (!ready) {
        return false;
    }
    return !link_known || (bcr_read(4) & BCR4_LEDOUT) != 0;
}

bool pcnet_send(const void *frame, size_t length) {
    if (!ready || length == 0 || length > MAX_FRAME) {
        return false;
    }
    size_t slot = OFFSET_TX_RING + tx_next * DESCRIPTOR_SIZE;
    uint64_t deadline = timer_ticks() + 10;
    while ((*half(slot + 6) & DESC_OWN) != 0) {
        if (timer_ticks() > deadline) {
            return false;
        }
    }
    uint8_t *buffer = memory + OFFSET_TX_BUFFERS + tx_next * BUFFER_STRIDE;
    const uint8_t *bytes = frame;
    for (size_t i = 0; i < length; i++) {
        buffer[i] = bytes[i];
    }
    *half(slot + 4) = (uint16_t)(-(int)length) | 0xf000;
    *word(slot + 8) = 0;
    __asm__ volatile ("sfence" : : : "memory");
    *half(slot + 6) = DESC_OWN | DESC_STP | DESC_ENP;
    tx_next = (tx_next + 1) % RING_SIZE;
    csr_write(0, CSR0_TDMD);
    sent++;
    return true;
}

size_t pcnet_receive(void *out, size_t capacity) {
    if (!ready) {
        return 0;
    }
    for (;;) {
        size_t slot = OFFSET_RX_RING + rx_next * DESCRIPTOR_SIZE;
        uint16_t status = *half(slot + 6);
        if ((status & DESC_OWN) != 0) {
            return 0;
        }
        __asm__ volatile ("lfence" : : : "memory");
        size_t length = *word(slot + 8) & 0xfff;
        bool whole = (status & (DESC_ERR | DESC_STP | DESC_ENP))
                  == (DESC_STP | DESC_ENP);
        /* The count includes the 4-byte CRC. */
        length = length > 4 ? length - 4 : 0;
        if (whole && length != 0) {
            if (length > capacity) {
                length = capacity;
            }
            const uint8_t *buffer = memory + OFFSET_RX_BUFFERS
                                  + rx_next * BUFFER_STRIDE;
            uint8_t *bytes = out;
            for (size_t i = 0; i < length; i++) {
                bytes[i] = buffer[i];
            }
        }
        rx_give(rx_next);
        rx_next = (rx_next + 1) % RING_SIZE;
        if (whole && length != 0) {
            received++;
            return length;
        }
    }
}

uint64_t pcnet_sent_count(void) {
    return sent;
}

uint64_t pcnet_received_count(void) {
    return received;
}
