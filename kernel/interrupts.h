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

#endif
