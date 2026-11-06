#include <stddef.h>
#include <stdint.h>

#include "interrupts.h"
#include "io.h"
#include "keyboard.h"
#include "mouse.h"
#include "paging.h"
#include "scheduler.h"
#include "timer.h"
#include "user.h"

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_COMMAND 0xa0
#define PIC2_DATA 0xa1
#define PIC1_OFFSET 0x20
#define PIC2_OFFSET 0x28
#define SYSCALL_VECTOR 0x80
#define KERNEL_CODE_SELECTOR 0x28
#define KERNEL_DATA_SELECTOR 0x30
#define USER_DATA_SELECTOR 0x38
#define USER_CODE_SELECTOR 0x40
#define TSS_SELECTOR 0x48
#define GDT_ENTRY_COUNT 11
#define KERNEL_STACK_SIZE 16384
#define SYSCALL_STACK_SIZE 16384

#define IA32_APIC_BASE_MSR 0x1b
#define IA32_APIC_BASE_ENABLE (1u << 11)
#define IA32_APIC_BASE_X2APIC (1u << 10)
#define IA32_EFER_MSR 0xc0000080u
#define IA32_STAR_MSR 0xc0000081u
#define IA32_LSTAR_MSR 0xc0000082u
#define IA32_FMASK_MSR 0xc0000084u
#define IA32_GS_BASE_MSR 0xc0000101u
#define IA32_KERNEL_GS_BASE_MSR 0xc0000102u
#define IA32_EFER_SCE (1u << 0)
#define SYSCALL_FMASK ((1u << 8) | (1u << 9) | (1u << 10))
#define APIC_LVT_LINT0 0x350
#define APIC_LVT_DELIVERY_EXTINT (7u << 8)
#define APIC_LVT_DELIVERY_MASK (7u << 8)
#define APIC_LVT_MASKED (1u << 16)

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_register {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base;
} __attribute__((packed));

struct gdt_register {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct syscall_cpu_state {
    uint64_t user_rsp;
    uint64_t kernel_rsp;
};

_Static_assert(offsetof(struct tss, rsp0) == 4, "TSS RSP0 layout");
_Static_assert(sizeof(struct tss) == 104, "TSS descriptor layout");

#define DECLARE_EXCEPTION_STUB(vector) extern void exception_stub_##vector(void)
DECLARE_EXCEPTION_STUB(0);
DECLARE_EXCEPTION_STUB(1);
DECLARE_EXCEPTION_STUB(2);
DECLARE_EXCEPTION_STUB(3);
DECLARE_EXCEPTION_STUB(4);
DECLARE_EXCEPTION_STUB(5);
DECLARE_EXCEPTION_STUB(6);
DECLARE_EXCEPTION_STUB(7);
DECLARE_EXCEPTION_STUB(8);
DECLARE_EXCEPTION_STUB(9);
DECLARE_EXCEPTION_STUB(10);
DECLARE_EXCEPTION_STUB(11);
DECLARE_EXCEPTION_STUB(12);
DECLARE_EXCEPTION_STUB(13);
DECLARE_EXCEPTION_STUB(14);
DECLARE_EXCEPTION_STUB(15);
DECLARE_EXCEPTION_STUB(16);
DECLARE_EXCEPTION_STUB(17);
DECLARE_EXCEPTION_STUB(18);
DECLARE_EXCEPTION_STUB(19);
DECLARE_EXCEPTION_STUB(20);
DECLARE_EXCEPTION_STUB(21);
DECLARE_EXCEPTION_STUB(22);
DECLARE_EXCEPTION_STUB(23);
DECLARE_EXCEPTION_STUB(24);
DECLARE_EXCEPTION_STUB(25);
DECLARE_EXCEPTION_STUB(26);
DECLARE_EXCEPTION_STUB(27);
DECLARE_EXCEPTION_STUB(28);
DECLARE_EXCEPTION_STUB(29);
DECLARE_EXCEPTION_STUB(30);
DECLARE_EXCEPTION_STUB(31);
#undef DECLARE_EXCEPTION_STUB

extern void irq_stub_0(void);
extern void irq_stub_1(void);
extern void irq_stub_2(void);
extern void irq_stub_3(void);
extern void irq_stub_4(void);
extern void irq_stub_5(void);
extern void irq_stub_6(void);
extern void irq_stub_7(void);
extern void irq_stub_8(void);
extern void irq_stub_9(void);
extern void irq_stub_10(void);
extern void irq_stub_11(void);
extern void irq_stub_12(void);
extern void irq_stub_13(void);
extern void irq_stub_14(void);
extern void irq_stub_15(void);
extern void syscall_stub(void);
extern void syscall_entry(void);
extern void gdt_load(const struct gdt_register *gdtr);
extern void tss_load(uint16_t selector);

static struct idt_entry idt[SYSCALL_VECTOR + 1] __attribute__((aligned(16)));
static uint64_t gdt[GDT_ENTRY_COUNT] __attribute__((aligned(16)));
static struct tss task_state_segment;
static uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t syscall_stack[SYSCALL_STACK_SIZE] __attribute__((aligned(16)));
static struct syscall_cpu_state syscall_cpu_state __attribute__((aligned(16)));

static void (*const exception_stubs[32])(void) = {
    exception_stub_0, exception_stub_1, exception_stub_2, exception_stub_3,
    exception_stub_4, exception_stub_5, exception_stub_6, exception_stub_7,
    exception_stub_8, exception_stub_9, exception_stub_10, exception_stub_11,
    exception_stub_12, exception_stub_13, exception_stub_14, exception_stub_15,
    exception_stub_16, exception_stub_17, exception_stub_18, exception_stub_19,
    exception_stub_20, exception_stub_21, exception_stub_22, exception_stub_23,
    exception_stub_24, exception_stub_25, exception_stub_26, exception_stub_27,
    exception_stub_28, exception_stub_29, exception_stub_30, exception_stub_31,
};

static void (*const irq_stubs[16])(void) = {
    irq_stub_0, irq_stub_1, irq_stub_2, irq_stub_3,
    irq_stub_4, irq_stub_5, irq_stub_6, irq_stub_7,
    irq_stub_8, irq_stub_9, irq_stub_10, irq_stub_11,
    irq_stub_12, irq_stub_13, irq_stub_14, irq_stub_15,
};

static void idt_set_gate(size_t vector, uintptr_t handler, uint8_t attributes) {
    idt[vector] = (struct idt_entry) {
        .offset_low = (uint16_t)handler,
        .selector = KERNEL_CODE_SELECTOR,
        .ist = 0,
        .attributes = attributes,
        .offset_middle = (uint16_t)(handler >> 16),
        .offset_high = (uint32_t)(handler >> 32),
        .reserved = 0,
    };
}

static void gdt_set_tss(uintptr_t base, uint32_t limit) {
    gdt[TSS_SELECTOR >> 3] =
        (limit & 0xffffu)
        | ((uint64_t)(base & 0xffffffu) << 16)
        | (0x89ULL << 40)
        | ((uint64_t)((limit >> 16) & 0xfu) << 48)
        | ((uint64_t)((base >> 24) & 0xffu) << 56);
    gdt[(TSS_SELECTOR >> 3) + 1] = base >> 32;
}

static void gdt_initialize(void) {
    gdt[KERNEL_CODE_SELECTOR >> 3] = 0x00af9a000000ffffULL;
    gdt[KERNEL_DATA_SELECTOR >> 3] = 0x00cf92000000ffffULL;
    gdt[USER_DATA_SELECTOR >> 3] = 0x00cff2000000ffffULL;
    gdt[USER_CODE_SELECTOR >> 3] = 0x00affa000000ffffULL;

    task_state_segment = (struct tss) {
        .rsp0 = (uintptr_t)(kernel_stack + sizeof(kernel_stack)),
        .io_map_base = sizeof(struct tss),
    };
    gdt_set_tss((uintptr_t)&task_state_segment, sizeof(task_state_segment) - 1);

    const struct gdt_register gdtr = {
        .limit = sizeof(gdt) - 1,
        .base = (uintptr_t)gdt,
    };
    gdt_load(&gdtr);
    tss_load(TSS_SELECTOR);
}

static void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                      "d"((uint32_t)(value >> 32)));
}

static void syscall_initialize(void) {
    uint32_t efer_low;
    uint32_t efer_high;
    uint32_t efer_msr = IA32_EFER_MSR;
    __asm__ volatile ("rdmsr" : "=a"(efer_low), "=d"(efer_high) : "c"(efer_msr));
    uint64_t efer = ((uint64_t)efer_high << 32) | efer_low;
    write_msr(IA32_EFER_MSR, efer | IA32_EFER_SCE);
    /*
     * SYSRET loads CS from STAR[63:48] + 16 and SS from STAR[63:48] + 8.
     * Intel CPUs force RPL 3 into both, but AMD CPUs copy the SS selector
     * unchanged, so the base selector must already carry RPL 3. Otherwise
     * ring 3 runs with SS = 0x38 and the next iretq back to it raises
     * #GP(0x38).
     */
    write_msr(IA32_STAR_MSR,
              ((uint64_t)(KERNEL_DATA_SELECTOR | 3) << 48)
              | ((uint64_t)KERNEL_CODE_SELECTOR << 32));
    write_msr(IA32_LSTAR_MSR, (uintptr_t)syscall_entry);
    write_msr(IA32_FMASK_MSR, SYSCALL_FMASK);
    syscall_cpu_state = (struct syscall_cpu_state) {
        .user_rsp = 0,
        .kernel_rsp = (uintptr_t)(syscall_stack + sizeof(syscall_stack)),
    };
    write_msr(IA32_GS_BASE_MSR, 0);
    write_msr(IA32_KERNEL_GS_BASE_MSR, (uintptr_t)&syscall_cpu_state);
}

static void pic_remap(void) {
    io_out8(PIC1_DATA, 0xff);
    io_out8(PIC2_DATA, 0xff);

    io_out8(PIC1_COMMAND, 0x11);
    io_wait();
    io_out8(PIC2_COMMAND, 0x11);
    io_wait();
    io_out8(PIC1_DATA, PIC1_OFFSET);
    io_wait();
    io_out8(PIC2_DATA, PIC2_OFFSET);
    io_wait();
    io_out8(PIC1_DATA, 0x04);
    io_wait();
    io_out8(PIC2_DATA, 0x02);
    io_wait();
    io_out8(PIC1_DATA, 0x01);
    io_wait();
    io_out8(PIC2_DATA, 0x01);
    io_wait();

    io_out8(PIC1_DATA, 0xff);
    io_out8(PIC2_DATA, 0xff);
}

bool interrupts_route_legacy_pic(void) {
    uint32_t apic_base_low;
    uint32_t apic_base_high;
    __asm__ volatile ("rdmsr"
                      : "=a"(apic_base_low), "=d"(apic_base_high)
                      : "c"(IA32_APIC_BASE_MSR));

    (void)apic_base_high;
    if ((apic_base_low & IA32_APIC_BASE_ENABLE) == 0) {
        return true;
    }
    if ((apic_base_low & IA32_APIC_BASE_X2APIC) != 0) {
        return false;
    }

    uint64_t apic_physical_base =
        ((uint64_t)apic_base_high << 32) | (apic_base_low & 0xfffff000u);
    if (!paging_map_page(0xffffc00000000000ULL, apic_physical_base,
                         PAGING_WRITABLE | PAGING_WRITE_THROUGH
                             | PAGING_CACHE_DISABLE)) {
        return false;
    }

    volatile uint32_t *apic = (volatile uint32_t *)0xffffc00000000000ULL;
    volatile uint32_t *lint0 = (volatile uint32_t *)
        ((volatile uint8_t *)apic + APIC_LVT_LINT0);
    uint32_t value = *lint0;
    value &= ~(APIC_LVT_DELIVERY_MASK | APIC_LVT_MASKED);
    value |= APIC_LVT_DELIVERY_EXTINT;
    *lint0 = value;
    return true;
}

void interrupts_initialize(void) {
    __asm__ volatile ("cli");
    gdt_initialize();

    for (size_t vector = 0; vector < 32; vector++) {
        idt_set_gate(vector, (uintptr_t)exception_stubs[vector], 0x8e);
    }
    for (size_t irq = 0; irq < 16; irq++) {
        idt_set_gate(PIC1_OFFSET + irq, (uintptr_t)irq_stubs[irq], 0x8e);
    }
    idt_set_gate(SYSCALL_VECTOR, (uintptr_t)syscall_stub, 0xee);

    const struct idt_register idtr = {
        .limit = sizeof(idt) - 1,
        .base = (uintptr_t)idt,
    };
    __asm__ volatile ("lidt %0" : : "m"(idtr));
    pic_remap();
    syscall_initialize();
}

void interrupts_enable(void) {
    __asm__ volatile ("sti" : : : "memory");
}

void interrupts_unmask_irq(uint8_t irq) {
    if (irq >= 16) {
        return;
    }

    if (irq >= 8) {
        io_out8(PIC2_DATA, io_in8(PIC2_DATA) & (uint8_t)~(1u << (irq - 8)));
        io_out8(PIC1_DATA, io_in8(PIC1_DATA) & (uint8_t)~(1u << 2));
    } else {
        io_out8(PIC1_DATA, io_in8(PIC1_DATA) & (uint8_t)~(1u << irq));
    }
}

void *irq_handler(uint64_t vector, void *interrupt_frame) {
    if (vector == PIC1_OFFSET) {
        timer_irq_handler();
    } else if (vector == PIC1_OFFSET + 1) {
        keyboard_irq_handler();
    } else if (vector == PIC2_OFFSET + 4) {
        mouse_irq_handler();
    }

    if (vector >= PIC2_OFFSET) {
        io_out8(PIC2_COMMAND, 0x20);
    }
    io_out8(PIC1_COMMAND, 0x20);

    if (vector == PIC1_OFFSET) {
        return user_preempt_check(scheduler_timer_tick(interrupt_frame));
    }
    return interrupt_frame;
}

uintptr_t interrupts_task_kernel_stack(void) {
    return task_state_segment.rsp0;
}

void interrupts_set_task_kernel_stack(uintptr_t stack_top) {
    if (stack_top == 0) {
        return;
    }
    task_state_segment.rsp0 = stack_top;
    syscall_cpu_state.kernel_rsp = stack_top;
}
