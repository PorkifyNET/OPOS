#ifndef OPOS_INTERRUPTS_H
#define OPOS_INTERRUPTS_H

#include <stdbool.h>
#include <stdint.h>

void interrupts_initialize(void);
bool interrupts_route_legacy_pic(void);
void interrupts_unmask_irq(uint8_t irq);
void interrupts_enable(void);
uintptr_t interrupts_task_kernel_stack(void);
void interrupts_set_task_kernel_stack(uintptr_t stack_top);
void exception_handler(uint64_t vector, uint64_t error_code, uint64_t instruction_pointer,
                       uint64_t code_segment, void *exception_frame);
void *irq_handler(uint64_t vector, void *interrupt_frame);

/* Vector of the one device routed through the IOAPIC. */
#define INTERRUPTS_DEVICE_VECTOR 0x70
/* Routes ACPI GSI gsi to handler (runs in interrupt context; the local
   APIC EOI is sent after it). Returns NULL on success, else the reason.
   Only one GSI can be routed. */
const char *interrupts_route_gsi(uint32_t gsi, bool level, bool active_low,
                                 void (*handler)(void));
/* Masks or unmasks the routed GSI. */
void interrupts_gsi_mask(bool masked);

#endif
