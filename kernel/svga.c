#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "io.h"
#include "paging.h"
#include "pci.h"
#include "svga.h"

/* Register and FIFO layout from VMware's svga_reg.h (SVGA II). */
#define SVGA_VENDOR_ID 0x15ad
#define SVGA_DEVICE_ID 0x0405
#define SVGA_INDEX_PORT 0
#define SVGA_VALUE_PORT 1
#define SVGA_ID_2 0x90000002u

#define SVGA_REG_ID 0
#define SVGA_REG_ENABLE 1
#define SVGA_REG_WIDTH 2
#define SVGA_REG_HEIGHT 3
#define SVGA_REG_MAX_WIDTH 4
#define SVGA_REG_MAX_HEIGHT 5
#define SVGA_REG_BITS_PER_PIXEL 7
#define SVGA_REG_RED_MASK 9
#define SVGA_REG_GREEN_MASK 10
#define SVGA_REG_BLUE_MASK 11
#define SVGA_REG_BYTES_PER_LINE 12
#define SVGA_REG_FB_START 13
#define SVGA_REG_FB_OFFSET 14
#define SVGA_REG_VRAM_SIZE 15
#define SVGA_REG_MEM_START 18
#define SVGA_REG_MEM_SIZE 19
#define SVGA_REG_CONFIG_DONE 20
#define SVGA_REG_SYNC 21

#define SVGA_FIFO_MIN 0
#define SVGA_FIFO_MAX 1
#define SVGA_FIFO_NEXT_CMD 2
#define SVGA_FIFO_STOP 3
#define SVGA_FIFO_REGISTERS_BYTES 16
#define SVGA_CMD_UPDATE 1

#define PAGE_SIZE 4096ULL
/* Kernel virtual windows, clear of the other MMIO windows near
   0xffffc00000000000. */
#define SVGA_FIFO_VIRTUAL 0xffffc00030000000ULL
#define SVGA_FB_VIRTUAL 0xffffc00040000000ULL
/* Two windows, alternated on each mode change (see set_dimensions). */
#define SVGA_FB_WINDOW_STRIDE 0x0000000008000000ULL /* 128 MiB */
#define SVGA_FIFO_MAP_LIMIT (256 * 1024ULL)
#define SVGA_FB_MAP_LIMIT (64 * 1024 * 1024ULL)

#define IA32_PAT_MSR 0x277
#define PAT_TYPE_WRITE_COMBINING 0x01

struct svga_mode_size {
    uint16_t width;
    uint16_t height;
};

/* Widths are multiples of 16: QEMU's vmware-vga shears odd widths such as
   1366, and aligned pitches are safest on other hosts too. Settings store
   the index, so only append to this list. */
static const struct svga_mode_size svga_modes[] = {
    {0, 0}, /* boot default, filled from the Limine mode */
    {1280, 720},
    {1280, 800},
    {1440, 900},
    {1600, 900},
    {1920, 1080},
    {1920, 1200},
    {2560, 1440},
    {3840, 2160},
};
#define SVGA_MODE_COUNT (sizeof(svga_modes) / sizeof(svga_modes[0]))

static bool present;
static bool enabled;
static uint16_t io_base;
static uint64_t framebuffer_physical;
static uint32_t vram_size;
static uint32_t max_width;
static uint32_t max_height;
static volatile uint32_t *fifo;
static uint32_t fifo_size;
static uint64_t framebuffer_mapped_pages;
/* Which of the two framebuffer windows the console uses (0 or 1). */
static uint8_t framebuffer_window;
static uint64_t framebuffer_page_flags;
static size_t current_mode;
static uint32_t current_width;
static uint32_t current_height;
static bool update_pending;

static uint32_t svga_read(uint32_t index) {
    io_out32((uint16_t)(io_base + SVGA_INDEX_PORT), index);
    return io_in32((uint16_t)(io_base + SVGA_VALUE_PORT));
}

static void svga_write(uint32_t index, uint32_t value) {
    io_out32((uint16_t)(io_base + SVGA_INDEX_PORT), index);
    io_out32((uint16_t)(io_base + SVGA_VALUE_PORT), value);
}

static uint64_t interrupts_save_disable(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void interrupts_restore(uint64_t flags) {
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

/* Write-combining if Limine's PAT has WC in entry 5 (PAT=1, PCD=0, PWT=1),
   else uncached. WC makes full-frame copies many times faster. */
static uint64_t choose_framebuffer_flags(void) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(IA32_PAT_MSR));
    uint8_t entry5 = (uint8_t)((high >> 8) & 0x07);
    uint64_t flags = PAGING_WRITABLE | PAGING_NO_EXECUTE;
    if (entry5 == PAT_TYPE_WRITE_COMBINING) {
        return flags | PAGING_PAT_4K | PAGING_WRITE_THROUGH;
    }
    return flags | PAGING_WRITE_THROUGH | PAGING_CACHE_DISABLE;
}

static void unmap_range(uint64_t virtual_base, uint64_t pages) {
    for (uint64_t i = 0; i < pages; i++) {
        (void)paging_unmap_page(virtual_base + i * PAGE_SIZE, NULL);
    }
}

static bool map_range(uint64_t virtual_base, uint64_t physical_base,
                      uint64_t pages, uint64_t flags) {
    for (uint64_t i = 0; i < pages; i++) {
        if (!paging_map_page(virtual_base + i * PAGE_SIZE,
                             physical_base + i * PAGE_SIZE, flags)) {
            unmap_range(virtual_base, i);
            return false;
        }
    }
    return true;
}

static void mask_to_channel(uint32_t mask, uint8_t *shift, uint8_t *size) {
    uint8_t s = 0;
    while (s < 32 && (mask & (1u << s)) == 0) {
        s++;
    }
    uint8_t n = 0;
    while (s + n < 32 && (mask & (1u << (s + n))) != 0) {
        n++;
    }
    *shift = s;
    *size = n;
}

bool svga_initialize(void) {
    present = false;
    struct pci_device device;
    if (!pci_find_device(SVGA_VENDOR_ID, SVGA_DEVICE_ID, &device)) {
        return false;
    }
    bool io_space = false;
    uint64_t port = pci_bar_address(&device, 0, &io_space);
    if (!io_space || port == 0 || port > 0xfff0) {
        return false;
    }
    pci_enable_device(&device);
    io_base = (uint16_t)port;

    svga_write(SVGA_REG_ID, SVGA_ID_2);
    if (svga_read(SVGA_REG_ID) != SVGA_ID_2) {
        return false;
    }
    framebuffer_physical = svga_read(SVGA_REG_FB_START);
    vram_size = svga_read(SVGA_REG_VRAM_SIZE);
    max_width = svga_read(SVGA_REG_MAX_WIDTH);
    max_height = svga_read(SVGA_REG_MAX_HEIGHT);
    uint64_t fifo_physical = svga_read(SVGA_REG_MEM_START);
    uint32_t mem_size = svga_read(SVGA_REG_MEM_SIZE);
    if (framebuffer_physical == 0 || (framebuffer_physical & (PAGE_SIZE - 1)) != 0
        || fifo_physical == 0 || (fifo_physical & (PAGE_SIZE - 1)) != 0
        || mem_size < PAGE_SIZE || vram_size == 0) {
        return false;
    }

    fifo_size = mem_size < SVGA_FIFO_MAP_LIMIT ? mem_size
                                               : (uint32_t)SVGA_FIFO_MAP_LIMIT;
    fifo_size &= ~(uint32_t)(PAGE_SIZE - 1);
    if (!map_range(SVGA_FIFO_VIRTUAL, fifo_physical, fifo_size / PAGE_SIZE,
                   PAGING_WRITABLE | PAGING_NO_EXECUTE | PAGING_WRITE_THROUGH
                       | PAGING_CACHE_DISABLE)) {
        return false;
    }
    fifo = (volatile uint32_t *)SVGA_FIFO_VIRTUAL;
    framebuffer_page_flags = choose_framebuffer_flags();
    present = true;
    return true;
}

bool svga_available(void) {
    return present;
}

size_t svga_mode_count(void) {
    return SVGA_MODE_COUNT;
}

bool svga_mode_size(size_t index, uint32_t *width, uint32_t *height) {
    if (width == NULL || height == NULL) {
        return false;
    }
    if (index == SVGA_MODE_CUSTOM) {
        *width = current_width;
        *height = current_height;
        return current_mode == SVGA_MODE_CUSTOM;
    }
    if (index >= SVGA_MODE_COUNT) {
        return false;
    }
    if (index == 0) {
        struct console_mode boot;
        console_boot_mode(&boot);
        *width = (uint32_t)boot.width;
        *height = (uint32_t)boot.height;
    } else {
        *width = svga_modes[index].width;
        *height = svga_modes[index].height;
    }
    return true;
}

static bool size_supported(uint32_t width, uint32_t height) {
    uint64_t bytes = (uint64_t)width * height * 4;
    return width != 0 && height != 0 && width <= max_width
        && height <= max_height && bytes <= vram_size
        && bytes <= SVGA_FB_MAP_LIMIT;
}

bool svga_mode_supported(size_t index) {
    uint32_t width, height;
    if (index == SVGA_MODE_CUSTOM || !present
        || !svga_mode_size(index, &width, &height)) {
        return index == 0;
    }
    return size_supported(width, height);
}

size_t svga_current_mode(void) {
    return current_mode;
}

/* Called with interrupts disabled. Empties and restarts the command FIFO. */
static void fifo_reset(void) {
    /* Hosts with the extended FIFO keep extra registers after the first
       four; like Linux, start commands one page in so none get
       overwritten. */
    uint32_t min = fifo_size >= 2 * PAGE_SIZE ? (uint32_t)PAGE_SIZE
                                              : SVGA_FIFO_REGISTERS_BYTES;
    fifo[SVGA_FIFO_MIN] = min;
    fifo[SVGA_FIFO_MAX] = fifo_size;
    fifo[SVGA_FIFO_NEXT_CMD] = min;
    fifo[SVGA_FIFO_STOP] = min;
    svga_write(SVGA_REG_CONFIG_DONE, 1);
}

/* Queues an UPDATE for the whole screen. Returns false if the FIFO is full
   (the host has not caught up); the caller retries next tick. */
static bool fifo_update_screen(void) {
    uint32_t min = fifo[SVGA_FIFO_MIN];
    uint32_t max = fifo[SVGA_FIFO_MAX];
    uint32_t next = fifo[SVGA_FIFO_NEXT_CMD];
    uint32_t stop = fifo[SVGA_FIFO_STOP];
    if (min >= max || next < min || next >= max || stop < min || stop >= max) {
        return false;
    }
    uint32_t size = max - min;
    uint32_t used = (next - stop + size) % size;
    const uint32_t command[5] = {
        SVGA_CMD_UPDATE, 0, 0, current_width, current_height,
    };
    if (size - used <= sizeof(command)) {
        svga_write(SVGA_REG_SYNC, 1);
        return false;
    }
    for (size_t i = 0; i < 5; i++) {
        fifo[next / 4] = command[i];
        next += 4;
        if (next == max) {
            next = min;
        }
    }
    __asm__ volatile ("" : : : "memory");
    fifo[SVGA_FIFO_NEXT_CMD] = next;
    /* Ask the host to process the FIFO now rather than on its own timer. */
    svga_write(SVGA_REG_SYNC, 1);
    return true;
}

static bool set_dimensions(uint32_t width, uint32_t height);

bool svga_set_mode(size_t index) {
    uint32_t width, height;
    if (!present || !svga_mode_supported(index)
        || !svga_mode_size(index, &width, &height)) {
        return false;
    }
    if (index == 0 && !enabled) {
        current_mode = 0;
        return true; /* still on the untouched boot framebuffer */
    }
    if (!set_dimensions(width, height)) {
        return false;
    }
    current_mode = index;
    return true;
}

bool svga_set_size(uint32_t width, uint32_t height) {
    if (!present) {
        return false;
    }
    /* The desktop needs 640x480; hosts may ask for less while resizing. */
    if (width < 640) {
        width = 640;
    }
    if (height < 480) {
        height = 480;
    }
    if (width > max_width) {
        width = max_width;
    }
    if (height > max_height) {
        height = max_height;
    }
    if (!size_supported(width, height)) {
        return false;
    }
    if (enabled && width == current_width && height == current_height) {
        return true;
    }
    if (!set_dimensions(width, height)) {
        return false;
    }
    current_mode = SVGA_MODE_CUSTOM;
    return true;
}

static bool set_dimensions(uint32_t width, uint32_t height) {

    uint64_t flags = interrupts_save_disable();
    svga_write(SVGA_REG_WIDTH, width);
    svga_write(SVGA_REG_HEIGHT, height);
    svga_write(SVGA_REG_BITS_PER_PIXEL, 32);
    svga_write(SVGA_REG_ENABLE, 1);
    uint32_t pitch = svga_read(SVGA_REG_BYTES_PER_LINE);
    uint32_t offset = svga_read(SVGA_REG_FB_OFFSET);
    uint32_t red = svga_read(SVGA_REG_RED_MASK);
    uint32_t green = svga_read(SVGA_REG_GREEN_MASK);
    uint32_t blue = svga_read(SVGA_REG_BLUE_MASK);
    bool was_enabled = enabled;
    enabled = true;
    if (!was_enabled) {
        fifo_reset();
    }
    current_width = width;
    current_height = height;
    interrupts_restore(flags);

    uint64_t bytes = (uint64_t)offset + (uint64_t)pitch * height;
    if (pitch < width * 4 || bytes > vram_size || bytes > SVGA_FB_MAP_LIMIT) {
        return false;
    }
    /*
     * Map the new mode into the window the console is NOT using. The
     * console still draws through the old mapping until console_set_mode()
     * switches (it first erases the cursor at its old position, which may
     * lie past the end of a smaller new mode). Only then unmap the old one.
     */
    uint64_t new_window = SVGA_FB_VIRTUAL
        + (framebuffer_window == 0 ? SVGA_FB_WINDOW_STRIDE : 0);
    uint64_t new_pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!map_range(new_window, framebuffer_physical, new_pages,
                   framebuffer_page_flags)) {
        return false;
    }

    struct console_mode mode = {
        .address = (void *)(new_window + offset),
        .width = width,
        .height = height,
        .pitch = pitch,
    };
    mask_to_channel(red ? red : 0xff0000u, &mode.red_shift, &mode.red_size);
    mask_to_channel(green ? green : 0x00ff00u, &mode.green_shift,
                    &mode.green_size);
    mask_to_channel(blue ? blue : 0x0000ffu, &mode.blue_shift, &mode.blue_size);
    if (!console_set_mode(&mode)) {
        unmap_range(new_window, new_pages);
        return false;
    }
    uint64_t old_window = SVGA_FB_VIRTUAL
        + (framebuffer_window == 0 ? 0 : SVGA_FB_WINDOW_STRIDE);
    unmap_range(old_window, framebuffer_mapped_pages);
    framebuffer_window ^= 1;
    framebuffer_mapped_pages = new_pages;
    return true;
}

void svga_timer_tick(void) {
    if (!enabled) {
        return;
    }
    if (console_take_dirty()) {
        update_pending = true;
    }
    if (update_pending && fifo_update_screen()) {
        update_pending = false;
    }
}
