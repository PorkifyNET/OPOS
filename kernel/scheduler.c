#include <stddef.h>
#include <stdint.h>

#include "interrupts.h"
#include "paging.h"
#include "scheduler.h"
#include "timer.h"

#define SCHEDULER_STACK_SIZE 16384
/* System calls, and the desktop while a program waits for window events
   (desktop_app_window_event), run on this stack. */
#define SCHEDULER_INTERRUPT_STACK_SIZE 32768
#define KERNEL_CODE_SELECTOR 0x28
#define INITIAL_FLAGS 0x202
#define SELF_TEST_TICKS 6
#define IA32_FS_BASE_MSR 0xc0000100u
#define CR0_MONITOR_COPROCESSOR (1ULL << 1)
#define CR0_EMULATION (1ULL << 2)
#define CR0_TASK_SWITCHED (1ULL << 3)
#define CR0_NUMERIC_ERROR (1ULL << 5)
#define CR4_OS_FXSAVE (1ULL << 9)
#define CR4_OS_XMM_EXCEPTIONS (1ULL << 10)
#define FPU_STATE_SIZE SCHEDULER_FPU_STATE_SIZE

enum scheduler_task_state {
    SCHEDULER_TASK_UNUSED,
    SCHEDULER_TASK_RUNNABLE,
    SCHEDULER_TASK_RUNNING,
    SCHEDULER_TASK_SLEEPING,
    SCHEDULER_TASK_BLOCKED,
    SCHEDULER_TASK_FINISHED,
};

struct scheduler_interrupt_frame {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t instruction_pointer;
    uint64_t code_segment;
    uint64_t flags;
    uint64_t stack_pointer;
    uint64_t stack_segment;
};

struct scheduler_task {
    uint64_t id;
    volatile enum scheduler_task_state state;
    scheduler_task_entry entry;
    void *argument;
    struct scheduler_interrupt_frame *frame;
    uint64_t address_space;
    uintptr_t kernel_stack_top;
    uintptr_t user_return_stack;
    uintptr_t linux_user_stack;
    void *context;
    uint64_t wake_tick;
    const void *wait_event;
    bool kernel_gs_active;
    uint64_t user_fs_base;
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
    uint8_t stack[SCHEDULER_STACK_SIZE] __attribute__((aligned(16)));
    uint8_t interrupt_stack[SCHEDULER_INTERRUPT_STACK_SIZE] __attribute__((aligned(16)));
};

_Static_assert(sizeof(struct scheduler_interrupt_frame) == 176,
               "scheduler interrupt frame layout");
_Static_assert(offsetof(struct scheduler_interrupt_frame, vector) == 120,
               "scheduler interrupt vector layout");

static struct scheduler_task tasks[SCHEDULER_TASK_CAPACITY];
static size_t current_task;
static uint64_t next_task_id;
static bool scheduler_ready;
static uint8_t initial_fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));

static void scheduler_save_fpu(uint8_t state[FPU_STATE_SIZE]) {
    __asm__ volatile ("fxsave64 (%0)" : : "r"(state) : "memory");
}

static void scheduler_restore_fpu(const uint8_t state[FPU_STATE_SIZE]) {
    __asm__ volatile ("fxrstor64 (%0)" : : "r"(state) : "memory");
}

static void scheduler_copy_fpu(uint8_t destination[FPU_STATE_SIZE],
                               const uint8_t source[FPU_STATE_SIZE]) {
    for (size_t i = 0; i < FPU_STATE_SIZE; i++) {
        destination[i] = source[i];
    }
}

static void scheduler_enable_fpu(void) {
    uint64_t cr0;
    uint64_t cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(CR0_EMULATION | CR0_TASK_SWITCHED);
    cr0 |= CR0_MONITOR_COPROCESSOR | CR0_NUMERIC_ERROR;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OS_FXSAVE | CR4_OS_XMM_EXCEPTIONS;
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
    uint32_t default_mxcsr = 0x1f80;
    __asm__ volatile (
        "fninit; fldz; fldz; fldz; fldz; fldz; fldz; fldz; fldz; fninit;"
        "pxor %%xmm0, %%xmm0; pxor %%xmm1, %%xmm1;"
        "pxor %%xmm2, %%xmm2; pxor %%xmm3, %%xmm3;"
        "pxor %%xmm4, %%xmm4; pxor %%xmm5, %%xmm5;"
        "pxor %%xmm6, %%xmm6; pxor %%xmm7, %%xmm7;"
        "pxor %%xmm8, %%xmm8; pxor %%xmm9, %%xmm9;"
        "pxor %%xmm10, %%xmm10; pxor %%xmm11, %%xmm11;"
        "pxor %%xmm12, %%xmm12; pxor %%xmm13, %%xmm13;"
        "pxor %%xmm14, %%xmm14; pxor %%xmm15, %%xmm15;"
        "ldmxcsr %0"
        : : "m"(default_mxcsr) : "memory");
    scheduler_save_fpu(initial_fpu_state);
}

static uint64_t scheduler_read_fs_base(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high)
                      : "c"(IA32_FS_BASE_MSR));
    return ((uint64_t)high << 32) | low;
}

static void scheduler_write_fs_base(uint64_t fs_base) {
    __asm__ volatile ("wrmsr" : : "a"((uint32_t)fs_base),
                      "d"((uint32_t)(fs_base >> 32)),
                      "c"(IA32_FS_BASE_MSR));
}

static void scheduler_task_trampoline(void) {
    struct scheduler_task *task = &tasks[current_task];
    task->entry(task->argument);

    __asm__ volatile ("cli" : : : "memory");
    task->state = SCHEDULER_TASK_FINISHED;
    for (;;) {
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
    }
}

void scheduler_initialize(void) {
    scheduler_enable_fpu();
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY; i++) {
        tasks[i].id = 0;
        tasks[i].state = SCHEDULER_TASK_UNUSED;
        tasks[i].entry = NULL;
        tasks[i].argument = NULL;
        tasks[i].frame = NULL;
        tasks[i].address_space = 0;
        tasks[i].kernel_stack_top = 0;
        tasks[i].user_return_stack = 0;
        tasks[i].linux_user_stack = 0;
        tasks[i].context = NULL;
        tasks[i].wake_tick = 0;
        tasks[i].wait_event = NULL;
        tasks[i].kernel_gs_active = false;
        tasks[i].user_fs_base = 0;
        scheduler_copy_fpu(tasks[i].fpu_state, initial_fpu_state);
    }

    current_task = 0;
    next_task_id = 2;
    tasks[0].id = 1;
    tasks[0].state = SCHEDULER_TASK_RUNNING;
    tasks[0].address_space = paging_kernel_address_space();
    tasks[0].kernel_stack_top = interrupts_task_kernel_stack();
    scheduler_ready = true;
}

void *scheduler_timer_tick(void *interrupt_frame) {
    if (!scheduler_ready || interrupt_frame == NULL) {
        return interrupt_frame;
    }

    struct scheduler_task *current = &tasks[current_task];
    current->frame = interrupt_frame;
    struct scheduler_interrupt_frame *frame = interrupt_frame;
    uint64_t now = timer_ticks();
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY; i++) {
        if (tasks[i].state == SCHEDULER_TASK_SLEEPING
            && now >= tasks[i].wake_tick) {
            tasks[i].state = SCHEDULER_TASK_RUNNABLE;
        }
    }
    if (current->context != NULL && (frame->code_segment & 3) == 0
        && current->state == SCHEDULER_TASK_RUNNING) {
        return interrupt_frame;
    }
    if (current->state == SCHEDULER_TASK_RUNNING) {
        current->state = SCHEDULER_TASK_RUNNABLE;
    }

    for (size_t offset = 1; offset <= SCHEDULER_TASK_CAPACITY; offset++) {
        size_t candidate = (current_task + offset) % SCHEDULER_TASK_CAPACITY;
        if (tasks[candidate].state == SCHEDULER_TASK_RUNNABLE) {
            if (tasks[candidate].address_space != current->address_space
                && !paging_address_space_activate(tasks[candidate].address_space)) {
                current->state = SCHEDULER_TASK_RUNNING;
                return interrupt_frame;
            }
            bool swap_kernel_gs = current->kernel_gs_active
                != tasks[candidate].kernel_gs_active;
            if ((frame->code_segment & 3) == 3) {
                current->user_fs_base = scheduler_read_fs_base();
            }
            struct scheduler_interrupt_frame *candidate_frame =
                tasks[candidate].frame;
            scheduler_write_fs_base(
                candidate_frame != NULL
                    && (candidate_frame->code_segment & 3) == 3
                ? tasks[candidate].user_fs_base : 0);
            scheduler_save_fpu(current->fpu_state);
            scheduler_restore_fpu(tasks[candidate].fpu_state);
            current_task = candidate;
            tasks[candidate].state = SCHEDULER_TASK_RUNNING;
            interrupts_set_task_kernel_stack(tasks[candidate].kernel_stack_top);
            if (swap_kernel_gs) {
                __asm__ volatile ("swapgs" : : : "memory");
            }
            return tasks[candidate].frame;
        }
    }

    if (current->state == SCHEDULER_TASK_RUNNABLE) {
        current->state = SCHEDULER_TASK_RUNNING;
    }
    return interrupt_frame;
}

bool scheduler_spawn(scheduler_task_entry entry, void *argument,
                     uint64_t *task_id) {
    if (!scheduler_ready || entry == NULL || task_id == NULL
        || next_task_id == 0) {
        return false;
    }

    size_t slot = SCHEDULER_TASK_CAPACITY;
    for (size_t i = 1; i < SCHEDULER_TASK_CAPACITY; i++) {
        if (tasks[i].state == SCHEDULER_TASK_UNUSED
            || tasks[i].state == SCHEDULER_TASK_FINISHED) {
            slot = i;
            break;
        }
    }
    if (slot == SCHEDULER_TASK_CAPACITY) {
        return false;
    }

    struct scheduler_task *task = &tasks[slot];
    uintptr_t stack_end = (uintptr_t)(task->stack + sizeof(task->stack));
    stack_end &= ~(uintptr_t)15;
    stack_end -= sizeof(uint64_t);
    *(uint64_t *)stack_end = 0;

    struct scheduler_interrupt_frame *frame =
        (struct scheduler_interrupt_frame *)(stack_end - sizeof(*frame));
    *frame = (struct scheduler_interrupt_frame) {
        .instruction_pointer = (uintptr_t)scheduler_task_trampoline,
        .code_segment = KERNEL_CODE_SELECTOR,
        .flags = INITIAL_FLAGS,
        .stack_pointer = stack_end,
        .stack_segment = 0x30,
    };

    task->id = next_task_id++;
    task->entry = entry;
    task->argument = argument;
    task->frame = frame;
    task->address_space = paging_kernel_address_space();
    uintptr_t interrupt_stack_end =
        (uintptr_t)(task->interrupt_stack + sizeof(task->interrupt_stack));
    task->kernel_stack_top = interrupt_stack_end & ~(uintptr_t)15;
    task->user_return_stack = 0;
    task->linux_user_stack = 0;
    task->context = NULL;
    task->wake_tick = 0;
    task->wait_event = NULL;
    task->kernel_gs_active = false;
    task->user_fs_base = 0;
    scheduler_save_fpu(tasks[current_task].fpu_state);
    scheduler_copy_fpu(task->fpu_state, tasks[current_task].fpu_state);
    task->state = SCHEDULER_TASK_RUNNABLE;
    *task_id = task->id;
    return true;
}

bool scheduler_task_finished(uint64_t task_id) {
    if (!scheduler_ready || task_id == 0) {
        return false;
    }
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY; i++) {
        if (tasks[i].id == task_id) {
            return tasks[i].state == SCHEDULER_TASK_FINISHED;
        }
    }
    return false;
}

bool scheduler_task_slot(uint64_t task_id, size_t *slot) {
    if (!scheduler_ready || task_id == 0 || slot == NULL) {
        return false;
    }
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY; i++) {
        if (tasks[i].id == task_id
            && tasks[i].state != SCHEDULER_TASK_UNUSED
            && tasks[i].state != SCHEDULER_TASK_FINISHED) {
            *slot = i;
            return true;
        }
    }
    return false;
}

size_t scheduler_current_slot(void) {
    return current_task;
}

void scheduler_set_current_context(void *context) {
    tasks[current_task].context = context;
}

void *scheduler_current_context(void) {
    return tasks[current_task].context;
}

bool scheduler_activate_current_address_space(uint64_t address_space) {
    if (!scheduler_ready || address_space == 0) {
        return false;
    }

    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    bool activated = paging_address_space_activate(address_space);
    if (activated) {
        tasks[current_task].address_space = address_space;
    }
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
    return activated;
}

void scheduler_set_user_return_stack(uintptr_t stack_pointer) {
    tasks[current_task].user_return_stack = stack_pointer;
}

uintptr_t scheduler_user_return_stack(void) {
    return tasks[current_task].user_return_stack;
}

void scheduler_linux_syscall_enter(uint64_t fs_base) {
    __asm__ volatile ("movq %%gs:0, %0"
                      : "=r"(tasks[current_task].linux_user_stack));
    tasks[current_task].user_fs_base = fs_base;
    tasks[current_task].kernel_gs_active = true;
}

void scheduler_linux_syscall_leave(uint64_t fs_base) {
    tasks[current_task].user_fs_base = fs_base;
    __asm__ volatile ("movq %0, %%gs:0"
                      : : "r"(tasks[current_task].linux_user_stack) : "memory");
    tasks[current_task].kernel_gs_active = false;
}

void scheduler_linux_syscall_set_user_stack(uintptr_t stack_pointer) {
    tasks[current_task].linux_user_stack = stack_pointer;
    if (tasks[current_task].kernel_gs_active) {
        __asm__ volatile ("movq %0, %%gs:0" : : "r"(stack_pointer) : "memory");
    }
}

uintptr_t scheduler_linux_syscall_user_stack(void) {
    return tasks[current_task].linux_user_stack;
}

void scheduler_set_user_fs_base(uint64_t fs_base) {
    tasks[current_task].user_fs_base = fs_base;
}

void scheduler_reset_fpu_state(void) {
    if (!scheduler_ready) {
        return;
    }
    scheduler_copy_fpu(tasks[current_task].fpu_state, initial_fpu_state);
    scheduler_restore_fpu(tasks[current_task].fpu_state);
}

void scheduler_save_current_fpu(uint8_t state[SCHEDULER_FPU_STATE_SIZE]) {
    scheduler_save_fpu(tasks[current_task].fpu_state);
    scheduler_copy_fpu(state, tasks[current_task].fpu_state);
}

void scheduler_restore_current_fpu(
    const uint8_t state[SCHEDULER_FPU_STATE_SIZE]) {
    uint8_t *saved = tasks[current_task].fpu_state;
    scheduler_copy_fpu(saved, state);
    /* The state comes from user memory (rt_sigreturn). FXRSTOR raises #GP
       in ring 0 when MXCSR sets a bit outside MXCSR_MASK (FXSAVE bytes
       28-31; zero means the default 0xffbf). */
    uint32_t mask = (uint32_t)initial_fpu_state[28]
                  | (uint32_t)initial_fpu_state[29] << 8
                  | (uint32_t)initial_fpu_state[30] << 16
                  | (uint32_t)initial_fpu_state[31] << 24;
    if (mask == 0) {
        mask = 0xffbf;
    }
    uint32_t mxcsr = (uint32_t)saved[24] | (uint32_t)saved[25] << 8
                   | (uint32_t)saved[26] << 16 | (uint32_t)saved[27] << 24;
    mxcsr &= mask;
    saved[24] = (uint8_t)mxcsr;
    saved[25] = (uint8_t)(mxcsr >> 8);
    saved[26] = (uint8_t)(mxcsr >> 16);
    saved[27] = (uint8_t)(mxcsr >> 24);
    scheduler_restore_fpu(saved);
}

bool scheduler_sleep_until(uint64_t wake_tick) {
    if (!scheduler_ready) {
        return false;
    }
    if (timer_ticks() >= wake_tick) {
        return true;
    }

    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    struct scheduler_task *task = &tasks[current_task];
    task->wake_tick = wake_tick;
    task->state = SCHEDULER_TASK_SLEEPING;
    while (task->state == SCHEDULER_TASK_SLEEPING) {
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
    }
    task->wake_tick = 0;
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
    return true;
}

bool scheduler_block_current_locked(const void *event) {
    if (!scheduler_ready || event == NULL) {
        return false;
    }

    struct scheduler_task *task = &tasks[current_task];
    task->wait_event = event;
    task->state = SCHEDULER_TASK_BLOCKED;
    while (task->state == SCHEDULER_TASK_BLOCKED) {
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
    }
    task->wait_event = NULL;
    if (task->state == SCHEDULER_TASK_RUNNABLE) {
        task->state = SCHEDULER_TASK_RUNNING;
    }
    return true;
}

void scheduler_wake_event(const void *event) {
    (void)scheduler_wake_event_count(event, SIZE_MAX);
}

size_t scheduler_wake_event_count(const void *event, size_t maximum) {
    if (!scheduler_ready || event == NULL) {
        return 0;
    }
    size_t woken = 0;
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY; i++) {
        if (tasks[i].state == SCHEDULER_TASK_BLOCKED
            && tasks[i].wait_event == event) {
            tasks[i].state = SCHEDULER_TASK_RUNNABLE;
            woken++;
            if (woken == maximum) {
                break;
            }
        }
    }
    return woken;
}

struct scheduler_self_test_state {
    volatile uint64_t iterations;
};

static struct scheduler_self_test_state self_test_states[2];

static void scheduler_self_test_worker(void *argument) {
    struct scheduler_self_test_state *state = argument;
    uint64_t start = timer_ticks();
    while (timer_ticks() - start < SELF_TEST_TICKS) {
        state->iterations++;
    }
}

bool scheduler_self_test(void) {
    self_test_states[0].iterations = 0;
    self_test_states[1].iterations = 0;
    uint64_t first_id;
    uint64_t second_id;
    if (!scheduler_spawn(scheduler_self_test_worker, &self_test_states[0],
                         &first_id)
        || !scheduler_spawn(scheduler_self_test_worker, &self_test_states[1],
                            &second_id)) {
        return false;
    }

    while (!scheduler_task_finished(first_id)
           || !scheduler_task_finished(second_id)) {
        __asm__ volatile ("hlt" : : : "memory");
    }
    return self_test_states[0].iterations != 0
        && self_test_states[1].iterations != 0;
}
