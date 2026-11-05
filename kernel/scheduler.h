#ifndef OPOS_SCHEDULER_H
#define OPOS_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SCHEDULER_TASK_CAPACITY 8
#define SCHEDULER_FPU_STATE_SIZE 512

typedef void (*scheduler_task_entry)(void *argument);

void scheduler_initialize(void);
void *scheduler_timer_tick(void *interrupt_frame);
bool scheduler_spawn(scheduler_task_entry entry, void *argument,
                     uint64_t *task_id);
bool scheduler_task_finished(uint64_t task_id);
bool scheduler_task_slot(uint64_t task_id, size_t *slot);
bool scheduler_self_test(void);
size_t scheduler_current_slot(void);
void scheduler_set_current_context(void *context);
void *scheduler_current_context(void);
bool scheduler_activate_current_address_space(uint64_t address_space);
void scheduler_set_user_return_stack(uintptr_t stack_pointer);
uintptr_t scheduler_user_return_stack(void);
void scheduler_linux_syscall_enter(uint64_t fs_base);
void scheduler_linux_syscall_leave(uint64_t fs_base);
void scheduler_linux_syscall_set_user_stack(uintptr_t stack_pointer);
uintptr_t scheduler_linux_syscall_user_stack(void);
void scheduler_set_user_fs_base(uint64_t fs_base);
void scheduler_reset_fpu_state(void);
void scheduler_save_current_fpu(uint8_t state[SCHEDULER_FPU_STATE_SIZE]);
void scheduler_restore_current_fpu(
    const uint8_t state[SCHEDULER_FPU_STATE_SIZE]);
bool scheduler_sleep_until(uint64_t wake_tick);
bool scheduler_block_current_locked(const void *event);
void scheduler_wake_event(const void *event);
size_t scheduler_wake_event_count(const void *event, size_t maximum);

#endif
