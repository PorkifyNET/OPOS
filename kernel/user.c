#include <stddef.h>
#include <stdint.h>

#include "console.h"
#include "desktop.h"
#include "elf.h"
#include "initramfs.h"
#include "keyboard.h"
#include "memory.h"
#include "paging.h"
#include "permissions.h"
#include "accounts.h"
#include "scheduler.h"
#include "cmos.h"
#include "net.h"
#include "timer.h"
#include "user.h"
#include "vfs.h"

#define PAGE_SIZE 4096
#define USER_APP_MIN_ADDRESS 0x00400000ULL
#define USER_PIE_BASE 0x04000000ULL
#define USER_STACK_ADDRESS 0x7ffff000ULL
#define USER_STACK_TOP (USER_STACK_ADDRESS + PAGE_SIZE)
/* The main thread's stack: USER_STACK_PAGES pages ending at
   USER_STACK_TOP. The top page (at USER_STACK_ADDRESS) holds argv,
   envp and the auxiliary vector. musl's resolver alone needs over 4 KiB. */
#define USER_STACK_PAGES 32
#define USER_STACK_BASE (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)
#define USER_STACK_ARGUMENT_PAGE (USER_STACK_PAGES - 1)
#define USER_AT_NULL 0
#define USER_AT_PHDR 3
#define USER_AT_PHENT 4
#define USER_AT_PHNUM 5
#define USER_AT_PAGESZ 6
#define USER_AT_BASE 7
#define USER_AT_FLAGS 8
#define USER_AT_ENTRY 9
#define USER_AT_UID 11
#define USER_AT_EUID 12
#define USER_AT_GID 13
#define USER_AT_EGID 14
#define USER_AT_PLATFORM 15
#define USER_AT_HWCAP 16
#define USER_AT_CLKTCK 17
#define USER_AT_SECURE 23
#define USER_AT_RANDOM 25
#define USER_AT_HWCAP2 26
#define USER_AT_EXECFN 31
#define USER_CLOCK_TICKS_PER_SECOND 100
#define USER_SYSCALL_VECTOR 0x80
#define USER_SYSCALL_WRITE 1
#define USER_SYSCALL_EXIT 2
#define USER_SYSCALL_READ 3
#define USER_SYSCALL_SPAWN 4
#define USER_SYSCALL_WAIT 5
#define USER_WRITE_MAX 256
#define USER_READ_MAX 128
#define USER_HEAP_MAX_PAGES 128
#define USER_IOV_MAX 16
#define USER_DESCRIPTOR_CAPACITY 11
#define USER_FIRST_FILE_DESCRIPTOR 3
#define USER_OPEN_FILE_CAPACITY 32
#define USER_PIPE_CAPACITY 8
#define USER_PIPE_BUFFER_CAPACITY 4096
#define USER_FILE_PATH_CAPACITY 256
#define USER_FILE_IO_MAX 4096
/* Sockets move at most this much per call (short reads/writes are
   normal for them). */
#define USER_SOCKET_IO_MAX 65536
/* Largest program file; interpreters (ld.so) are smaller. */
#define USER_EXEC_IMAGE_MAX (1024 * 1024)
#define USER_INTERPRETER_IMAGE_MAX 65536
#define USER_EXEC_VECTOR_MAX 16
#define USER_EXEC_STRING_MAX 1024
#define USER_MMAP_BASE 0x10000000ULL
#define USER_MMAP_MAX_PAGES 16384
#define USER_MMAP_ANONYMOUS 0x20
#define USER_MMAP_PRIVATE 0x02
#define USER_PROT_WRITE 0x02
#define USER_PROT_EXEC 0x04
#define USER_PROT_READ 0x01
#define USER_MMAP_RESERVED (1ULL << 62)
#define USER_ADDRESS_LIMIT 0x00007ffffffff000ULL
#define LINUX_CLONE_VM 0x00000100ULL
#define LINUX_CLONE_FS 0x00000200ULL
#define LINUX_CLONE_FILES 0x00000400ULL
#define LINUX_CLONE_SIGHAND 0x00000800ULL
#define LINUX_CLONE_THREAD 0x00010000ULL
#define LINUX_CLONE_SYSVSEM 0x00040000ULL
#define LINUX_CLONE_SETTLS 0x00080000ULL
#define LINUX_CLONE_PARENT_SETTID 0x00100000ULL
#define LINUX_CLONE_CHILD_CLEARTID 0x00200000ULL
#define LINUX_CLONE_DETACHED 0x00400000ULL
#define LINUX_CLONE_CHILD_SETTID 0x01000000ULL
#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_PRIVATE_FLAG 128
#define LINUX_SIG_BLOCK 0
#define LINUX_SIG_UNBLOCK 1
#define LINUX_SIG_SETMASK 2
#define LINUX_SIGNAL_SET_SIZE 8
#define LINUX_SIGNAL_COUNT 64
#define LINUX_SIGKILL 9
#define LINUX_SIGCHLD 17
#define LINUX_SIGCONT 18
#define LINUX_SIGSTOP 19
#define LINUX_SIGURG 23
#define LINUX_SIGWINCH 28
#define LINUX_SIG_IGN 1
#define LINUX_SA_RESTORER 0x04000000ULL
#define LINUX_SA_RESTART 0x10000000ULL
#define LINUX_SA_NODEFER 0x40000000ULL
#define LINUX_SA_RESETHAND 0x80000000ULL
#define LINUX_SIGNAL_RED_ZONE_SIZE 128
#define USER_SIGNAL_FRAME_MAGIC 0x4f504f535349474eULL
#define LINUX_ARCH_SET_FS 0x1002
#define LINUX_ARCH_GET_FS 0x1003
#define LINUX_CLOCK_REALTIME 0
#define LINUX_CLOCK_MONOTONIC 1
#define LINUX_UTS_FIELD_SIZE 65
#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_O_ACCMODE 3
#define LINUX_O_RDONLY 0
#define LINUX_O_WRONLY 1
#define LINUX_O_RDWR 2
#define LINUX_O_CREAT 64
#define LINUX_O_EXCL 128
#define LINUX_O_TRUNC 512
#define LINUX_O_APPEND 1024
#define LINUX_O_DIRECTORY 65536
#define LINUX_O_CLOEXEC 524288
#define LINUX_O_NOCTTY 0x100
#define LINUX_O_NONBLOCK 0x800
#define LINUX_O_LARGEFILE 0x8000 /* always on for 64-bit files */
#define LINUX_MODE_DIRECTORY 0040000
#define LINUX_MODE_CHARACTER 0020000
#define LINUX_MODE_FIFO 0010000
#define LINUX_MODE_REGULAR 0100000
#define LINUX_DIRECTORY_TYPE 4
#define LINUX_REGULAR_TYPE 8
#define KERNEL_CODE_SELECTOR 0x28
#define KERNEL_DATA_SELECTOR 0x30

struct exception_frame {
    uint64_t vector;
    uint64_t error_code;
    uint64_t instruction_pointer;
    uint64_t code_segment;
    uint64_t flags;
    uint64_t stack_pointer;
    uint64_t stack_segment;
};

_Static_assert(offsetof(struct exception_frame, instruction_pointer) == 16,
               "exception instruction pointer layout");
_Static_assert(offsetof(struct exception_frame, code_segment) == 24,
               "exception code segment layout");
_Static_assert(offsetof(struct exception_frame, stack_pointer) == 40,
               "exception stack frame layout");

struct syscall_frame {
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
    uint64_t user_stack;
    uint64_t stack_segment;
};

_Static_assert(offsetof(struct syscall_frame, rax) == 112, "syscall register frame layout");
_Static_assert(offsetof(struct syscall_frame, instruction_pointer) == 136,
               "syscall return frame layout");
_Static_assert(offsetof(struct syscall_frame, code_segment) == 144,
               "syscall privilege frame layout");

extern const uint8_t user_program_start[];
extern const uint8_t user_program_end[];
extern const uint8_t user_fault_program_start[];
extern const uint8_t user_fault_program_end[];
extern const uint8_t user_nx_program_start[];
extern const uint8_t user_nx_program_end[];
extern const uint8_t user_input_program_start[];
extern const uint8_t user_input_program_end[];
extern const uint8_t user_shell_program_start[];
extern const uint8_t user_shell_program_end[];
extern const uint8_t user_linux_read_program_start[];
extern const uint8_t user_linux_read_program_end[];
extern const uint8_t user_file_program_start[];
extern const uint8_t user_file_program_end[];
extern const uint8_t user_preempt_program_start[];
extern const uint8_t user_preempt_program_end[];
extern const uint8_t user_spawn_program_start[];
extern const uint8_t user_spawn_program_end[];
struct linux_syscall_frame;
extern uint64_t user_enter(uintptr_t entry, uintptr_t stack_top);
extern uint64_t user_linux_resume(const struct linux_syscall_frame *frame,
                                  uintptr_t stack_pointer);
extern void user_exit_trampoline(void);

enum user_process_state {
    USER_PROCESS_UNUSED,
    USER_PROCESS_RUNNING,
    USER_PROCESS_EXITED,
};

enum user_open_file_kind {
    USER_OPEN_FILE_VFS,
    USER_OPEN_FILE_PIPE_READ,
    USER_OPEN_FILE_PIPE_WRITE,
    USER_OPEN_FILE_SOCKET,
};

struct user_file_descriptor {
    bool used;
    uint32_t references;
    enum user_open_file_kind kind;
    bool directory;
    bool readable;
    bool writable;
    bool append;
    size_t offset;
    vfs_object_handle object;
    uint8_t pipe_slot;
    bool nonblocking;
    /* Sockets: SOCK_STREAM or SOCK_DGRAM, enum user_socket_state, the
       net.c handle (TCP connection, listener or UDP socket; -1 = none). */
    uint8_t socket_type;
    uint8_t socket_state;
    bool socket_read_shut;
    bool socket_write_shut;
    int socket_handle;
    uint16_t socket_port; /* bound TCP port before connect/listen */
    int socket_error;     /* pending SO_ERROR (negative errno) */
    uint8_t socket_peer_ip[4];
    uint16_t socket_peer_port;
};

struct user_pipe {
    bool used;
    uint32_t readers;
    uint32_t writers;
    size_t read_offset;
    size_t write_offset;
    size_t size;
    uint8_t data[USER_PIPE_BUFFER_CAPACITY];
};

struct user_descriptor {
    bool used;
    bool closed;
    bool close_on_exec;
    uint8_t file_slot;
};

struct linux_syscall_frame {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbp;
    uint64_t rbx;
    uint64_t r9;
    uint64_t r8;
    uint64_t r10;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rax;
    uint64_t instruction_pointer;
    uint64_t flags;
    uint64_t fs_base;
};

struct user_image_segment {
    uintptr_t base;
    size_t page_index;
    size_t page_count;
    uint32_t flags;
};

struct linux_signal_action {
    uintptr_t handler;
    uint64_t flags;
    uintptr_t restorer;
    uint64_t mask;
};

struct user_signal_frame {
    uint64_t magic;
    struct linux_syscall_frame registers;
    uintptr_t stack_pointer;
    uint64_t signal_mask;
    uint8_t fpu_state[SCHEDULER_FPU_STATE_SIZE] __attribute__((aligned(16)));
};

struct user_process {
    uint64_t id;
    enum user_process_state state;
    uint64_t fault_status;
    bool faulted;
    volatile bool kill_requested;
    /* exit_group with other threads alive: they end with this status. */
    volatile bool group_exit;
    uint64_t group_status;
    char name[32];
    bool elf_image;
    uintptr_t initial_break;
    uintptr_t current_break;
    uintptr_t heap_base;
    uint64_t heap_pages[USER_HEAP_MAX_PAGES];
    size_t heap_page_count;
    uint64_t mmap_pages[USER_MMAP_MAX_PAGES];
    uint64_t mmap_flags[USER_MMAP_MAX_PAGES];
    uint64_t address_space;
    struct user_image_segment image_segments[ELF_MAX_LOAD_SEGMENTS];
    size_t image_segment_count;
    uint64_t image_pages[ELF_MAX_SEGMENT_PAGES];
    size_t image_page_count;
    uint64_t stack_pages[USER_STACK_PAGES];
    size_t cwd_length;
    char cwd[USER_FILE_PATH_CAPACITY];
    struct user_descriptor descriptors[USER_DESCRIPTOR_CAPACITY];
    struct linux_signal_action signal_actions[LINUX_SIGNAL_COUNT];
    size_t active_threads;
    /* Who the program runs as; checked on every path it touches. */
    struct permission_identity identity;
};

struct user_thread {
    bool active;
    uint64_t id;
    struct user_process *process;
    uintptr_t clear_child_tid;
    uintptr_t robust_list;
    uint64_t signal_mask;
    uint64_t pending_signals;
};

struct user_thread_job {
    bool active;
    uint64_t task_id;
    uint64_t thread_id;
    struct user_process *process;
    uintptr_t stack_pointer;
    uintptr_t clear_child_tid;
    uintptr_t robust_list;
    uint64_t signal_mask;
    struct linux_syscall_frame frame;
};

struct user_futex_queue {
    bool used;
    struct user_process *process;
    uintptr_t address;
    size_t waiters;
};

enum user_child_state {
    USER_CHILD_UNUSED,
    USER_CHILD_RUNNING,
    USER_CHILD_FINISHED,
};

struct user_child_job {
    uint64_t handle;
    uint64_t parent_process_id;
    uint64_t task_id;
    enum user_program program;
    const uint8_t *image_data;
    size_t image_size;
    bool elf_image;
    bool forked;
    uintptr_t fork_stack_pointer;
    struct linux_syscall_frame fork_frame;
    uint64_t fork_signal_mask;
    size_t cwd_length;
    char cwd[USER_FILE_PATH_CAPACITY];
    struct permission_identity identity;
    volatile enum user_child_state state;
    bool started;
    uint64_t process_id;
    uint64_t exit_status;
};

static uint64_t next_process_id = 1;
static struct user_process process_slots[SCHEDULER_TASK_CAPACITY];
static struct user_file_descriptor open_files[USER_OPEN_FILE_CAPACITY];
static struct user_pipe pipes[USER_PIPE_CAPACITY];
static uint8_t user_interpreter_images[SCHEDULER_TASK_CAPACITY][USER_INTERPRETER_IMAGE_MAX];

/* execve reads the new program here. 1 MiB per task would be a lot of
   kernel memory up front, so each buffer is allocated on first use and
   kept (the contiguous allocator cannot free). */
static uint8_t *user_exec_image_buffer(size_t task_slot) {
    static uint8_t *buffers[SCHEDULER_TASK_CAPACITY];
    if (task_slot >= SCHEDULER_TASK_CAPACITY) {
        return NULL;
    }
    if (buffers[task_slot] == NULL) {
        uint64_t physical = physical_pages_alloc_contiguous(
            USER_EXEC_IMAGE_MAX / PAGE_SIZE);
        if (physical != 0) {
            buffers[task_slot] = physical_page_to_hhdm(physical);
        }
    }
    return buffers[task_slot];
}

struct user_load_pages {
    uint64_t image_physical[ELF_MAX_SEGMENT_PAGES];
    uint8_t *image_buffers[ELF_MAX_SEGMENT_PAGES];
    uint64_t interpreter_physical[ELF_MAX_SEGMENT_PAGES];
    uint8_t *interpreter_buffers[ELF_MAX_SEGMENT_PAGES];
};
static struct user_load_pages user_load_scratch[SCHEDULER_TASK_CAPACITY];
static struct user_child_job child_jobs[SCHEDULER_TASK_CAPACITY - 1];
static struct user_thread user_threads[SCHEDULER_TASK_CAPACITY];
static struct user_thread_job thread_jobs[SCHEDULER_TASK_CAPACITY - 1];
static struct user_futex_queue futex_queues[SCHEDULER_TASK_CAPACITY];
static uint64_t next_child_handle = 1;

/* A process's name for the Task Manager: the last part of its path. */
static void user_set_process_name(struct user_process *process,
                                  const char *path, size_t length) {
    size_t start = 0;
    for (size_t i = 0; i < length; i++) {
        if (path[i] == '/') {
            start = i + 1;
        }
    }
    size_t used = 0;
    if (path == NULL || start == length) {
        static const char builtin[] = "BUILT-IN";
        for (; builtin[used] != '\0'; used++) {
            process->name[used] = builtin[used];
        }
    } else {
        for (size_t i = start; i < length && used + 1 < sizeof(process->name);
             i++) {
            process->name[used++] = path[i];
        }
    }
    process->name[used] = '\0';
}

/* Wakes every thread of a process from its waits (see
   scheduler_interrupt_task); they then end on the way back to user mode. */
static void user_process_interrupt_threads(struct user_process *process) {
    for (size_t slot = 0; slot < SCHEDULER_TASK_CAPACITY; slot++) {
        if (user_threads[slot].active
            && user_threads[slot].process == process
            && slot != scheduler_current_slot()) {
            scheduler_interrupt_task(slot);
        }
    }
}

static uint64_t user_random_sequence = 0x6a09e667f3bcc909ULL;
static const uint8_t *user_elf_image;
static size_t user_elf_image_size;

static int64_t user_spawn_child(struct user_process *parent,
                                uint64_t program_selector,
                                const char *path, size_t path_length);
static int64_t user_wait_child(struct user_process *parent, uint64_t handle,
                               uint64_t *process_id);
static int64_t user_wait_linux_child(struct user_process *parent,
                                     int64_t requested_process_id,
                                     uint32_t *wait_status);
static void user_orphan_children(uint64_t parent_process_id);
static void user_console_write(uint64_t file_descriptor, const char *data,
                               size_t length);
static void user_child_worker(void *argument);
static void user_thread_worker(void *argument);
static struct user_file_descriptor *user_file_descriptor(
    struct user_process *process, uint64_t descriptor);
static int64_t user_pipe_write(struct user_file_descriptor *file,
                               const uint8_t *buffer, size_t length,
                               bool user_buffer);

static uint64_t user_random_fallback(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    uint64_t value = ((uint64_t)high << 32) | low;
    value ^= timer_ticks();
    value ^= __atomic_add_fetch(&user_random_sequence,
                                0x9e3779b97f4a7c15ULL,
                                __ATOMIC_RELAXED);
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

static bool user_rdrand_supported(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1), "c"(0));
    return (ecx & (1u << 30)) != 0;
}

static uint32_t user_cpu_hwcap(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(1), "c"(0));
    return edx;
}

static uint64_t user_random_word(bool hardware_random) {
    if (hardware_random) {
        for (size_t attempt = 0; attempt < 10; attempt++) {
            uint64_t value;
            uint8_t ready;
            __asm__ volatile ("rdrand %0; setc %1"
                              : "=r"(value), "=qm"(ready));
            if (ready != 0) {
                return value;
            }
        }
    }
    return user_random_fallback();
}

static void user_fill_random(uint8_t destination[16]) {
    bool hardware_random = user_rdrand_supported();
    uint64_t first = user_random_word(hardware_random);
    uint64_t second = user_random_word(hardware_random);
    for (size_t i = 0; i < sizeof(first); i++) {
        destination[i] = (uint8_t)(first >> (i * 8));
        destination[sizeof(first) + i] = (uint8_t)(second >> (i * 8));
    }
}

static bool allocate_user_pages(size_t count, uint64_t physical_pages[],
                                uint8_t *page_buffers[]) {
    for (size_t i = 0; i < count; i++) {
        physical_pages[i] = physical_page_alloc();
        if (physical_pages[i] == 0) {
            for (size_t j = 0; j < i; j++) {
                physical_page_free(physical_pages[j]);
                physical_pages[j] = 0;
            }
            return false;
        }
        page_buffers[i] = physical_page_to_hhdm(physical_pages[i]);
        if (page_buffers[i] == NULL) {
            for (size_t j = 0; j <= i; j++) {
                physical_page_free(physical_pages[j]);
                physical_pages[j] = 0;
            }
            return false;
        }
    }

    return true;
}

static void zero_user_pages(size_t count, uint8_t *page_buffers[]) {
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < PAGE_SIZE; j++) {
            page_buffers[i][j] = 0;
        }
    }
}

static size_t map_user_pages(uintptr_t base, const uint64_t physical_pages[],
                             size_t count, uint64_t flags) {
    size_t mapped = 0;
    while (mapped < count
           && paging_map_page(base + mapped * PAGE_SIZE,
                              physical_pages[mapped], flags)) {
        mapped++;
    }
    return mapped;
}

static bool release_user_pages(uintptr_t base, uint64_t physical_pages[],
                               size_t count, size_t mapped_count);

static uint64_t user_segment_paging_flags(uint32_t flags) {
    uint64_t paging_flags = PAGING_USER;
    if ((flags & ELF_SEGMENT_WRITE) != 0) {
        paging_flags |= PAGING_WRITABLE | PAGING_NO_EXECUTE;
    } else if ((flags & ELF_SEGMENT_EXECUTE) == 0) {
        paging_flags |= PAGING_NO_EXECUTE;
    }
    return paging_flags;
}

static bool map_elf_plan(const struct elf_load_plan *plan,
                         const uint64_t physical_pages[],
                         size_t mapped_pages[ELF_MAX_LOAD_SEGMENTS]) {
    if (plan == NULL || physical_pages == NULL || mapped_pages == NULL) {
        return false;
    }
    for (size_t i = 0; i < plan->segment_count; i++) {
        const struct elf_load_segment *segment = &plan->segments[i];
        mapped_pages[i] = map_user_pages(
            segment->mapping_base, physical_pages + segment->page_index,
            segment->page_count, user_segment_paging_flags(segment->flags));
        if (mapped_pages[i] != segment->page_count) return false;
    }
    return true;
}

static bool elf_plan_permissions_valid(const struct elf_load_plan *plan) {
    if (plan == NULL) return false;
    for (size_t i = 0; i < plan->segment_count; i++) {
        const struct elf_load_segment *segment = &plan->segments[i];
        size_t size = segment->page_count * PAGE_SIZE;
        bool writable = (segment->flags & ELF_SEGMENT_WRITE) != 0;
        bool executable = (segment->flags & ELF_SEGMENT_EXECUTE) != 0;
        if (!paging_user_range_valid(segment->mapping_base, size,
                                     writable, executable)
            || (writable && paging_user_range_valid(
                    segment->mapping_base, size, false, true))
            || (executable && paging_user_range_valid(
                    segment->mapping_base, size, true, false))) return false;
    }
    return true;
}

static bool release_elf_plan_pages(
    const struct elf_load_plan *plan, uint64_t physical_pages[],
    const size_t mapped_pages[ELF_MAX_LOAD_SEGMENTS]) {
    if (plan == NULL || physical_pages == NULL || mapped_pages == NULL) {
        return false;
    }
    bool released = true;
    for (size_t i = 0; i < plan->segment_count; i++) {
        const struct elf_load_segment *segment = &plan->segments[i];
        if (!release_user_pages(segment->mapping_base,
                                physical_pages + segment->page_index,
                                segment->page_count, mapped_pages[i])) {
            released = false;
        }
    }
    return released;
}

static bool release_user_pages(uintptr_t base, uint64_t physical_pages[],
                               size_t count, size_t mapped_count) {
    bool released = true;
    for (size_t i = 0; i < count; i++) {
        bool unmapped = i >= mapped_count
            || paging_unmap_page(base + i * PAGE_SIZE, NULL);
        if (!unmapped) {
            released = false;
            continue;
        }
        if (physical_pages[i] != 0 && !physical_page_free(physical_pages[i])) {
            released = false;
        }
        physical_pages[i] = 0;
    }
    return released;
}

static bool restore_kernel_address_space(uint64_t kernel_address_space,
                                         uint64_t process_address_space) {
    if (!scheduler_activate_current_address_space(kernel_address_space)) {
        console_write_string("OPOS: failed to restore kernel address space\n");
        __asm__ volatile ("cli");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }
    return paging_address_space_destroy(process_address_space);
}

static bool release_mmap_pages(struct user_process *process, size_t first_page,
                               size_t page_count) {
    if (process == NULL) {
        return false;
    }
    bool released = true;
    for (size_t i = first_page; i < first_page + page_count; i++) {
        uint64_t physical_page = process->mmap_pages[i];
        if (physical_page == 0) {
            continue;
        }
        uintptr_t virtual_address = USER_MMAP_BASE + i * PAGE_SIZE;
        if (process->mmap_flags[i] != USER_MMAP_RESERVED
            && !paging_unmap_page(virtual_address, NULL)) {
            released = false;
            continue;
        }
        if (!physical_page_free(physical_page)) {
            released = false;
            continue;
        }
        process->mmap_pages[i] = 0;
        process->mmap_flags[i] = 0;
    }
    return released;
}

static bool process_mmap_state_clean(const struct user_process *process) {
    if (process == NULL) {
        return false;
    }
    for (size_t i = 0; i < USER_MMAP_MAX_PAGES; i++) {
        if (process->mmap_pages[i] != 0 || process->mmap_flags[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool process_resource_state_clean(const struct user_process *process) {
    if (process == NULL || process->state == USER_PROCESS_RUNNING
        || process->address_space != 0 || process->stack_pages[USER_STACK_ARGUMENT_PAGE] != 0
        || process->heap_page_count != 0 || process->image_segment_count != 0
        || process->image_page_count != 0
        || !process_mmap_state_clean(process)) {
        return false;
    }
    for (size_t i = 0; i < ELF_MAX_SEGMENT_PAGES; i++) {
        if (process->image_pages[i] != 0) return false;
    }
    for (size_t i = 0; i < USER_DESCRIPTOR_CAPACITY; i++) {
        if (process->descriptors[i].used) {
            return false;
        }
    }
    return true;
}

bool user_set_elf_image(const uint8_t *image, size_t image_size) {
    if (image == NULL || image_size == 0 || user_elf_image != NULL
        || scheduler_current_context() != NULL) {
        return false;
    }

    user_elf_image = image;
    user_elf_image_size = image_size;
    return true;
}

bool user_exception_dispatch(uint64_t vector, void *saved_frame) {
    struct exception_frame *frame = saved_frame;
    struct user_process *process = scheduler_current_context();
    if (process == NULL || vector > 31 || frame == NULL
        || (frame->code_segment & 3) != 3) {
        return false;
    }

    process->fault_status = 128 + vector;
    process->faulted = true;
    frame->instruction_pointer = (uintptr_t)user_exit_trampoline;
    frame->code_segment = KERNEL_CODE_SELECTOR;
    frame->flags = 0x202;
    frame->stack_pointer = (uintptr_t)(frame + 1);
    frame->stack_segment = KERNEL_DATA_SELECTOR;
    return true;
}

void user_syscall_dispatch(void *saved_registers) {
    struct syscall_frame *frame = saved_registers;
    struct user_process *process = scheduler_current_context();
    if (process == NULL || frame == NULL
        || frame->vector != USER_SYSCALL_VECTOR
        || frame->code_segment != 0x43) {
        return;
    }

    if (frame->rax == USER_SYSCALL_WRITE) {
        if (frame->rdi != 1) {
            frame->rax = (uint64_t)(int64_t)-9;
            return;
        }
        if (frame->rdx > USER_WRITE_MAX) {
            frame->rax = (uint64_t)(int64_t)-22;
            return;
        }
        if (frame->rdx != 0
            && !paging_user_range_valid((uintptr_t)frame->rsi, (size_t)frame->rdx,
                                        false, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
            return;
        }

        console_write_bytes((const char *)(uintptr_t)frame->rsi, (size_t)frame->rdx);
        frame->rax = frame->rdx;
        return;
    }

    if (frame->rax == USER_SYSCALL_EXIT) {
        frame->rax = frame->rdi;
        frame->instruction_pointer = (uintptr_t)user_exit_trampoline;
        frame->code_segment = KERNEL_CODE_SELECTOR;
        frame->flags = 0x202;
        /* Return through a kernel stack frame before restoring the saved C stack. */
        frame->user_stack = (uintptr_t)(frame + 1);
        frame->stack_segment = KERNEL_DATA_SELECTOR;
        return;
    }

    if (frame->rax == USER_SYSCALL_READ) {
        if (frame->rdi != 0) {
            frame->rax = (uint64_t)(int64_t)-9;
            return;
        }
        if (frame->rdx > USER_READ_MAX) {
            frame->rax = (uint64_t)(int64_t)-22;
            return;
        }
        if (frame->rdx != 0
            && !paging_user_range_valid((uintptr_t)frame->rsi, (size_t)frame->rdx,
                                        true, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
            return;
        }

        char *buffer = (char *)(uintptr_t)frame->rsi;
        size_t length = 0;
        while (length < frame->rdx) {
            char character;
            if (!keyboard_wait_character(&character)) {
                frame->rax = (uint64_t)(int64_t)-5;
                return;
            }

            if (character == '\b') {
                if (length != 0) {
                    length--;
                    user_console_write(1, &character, 1);
                }
                continue;
            }
            if (character != '\n' && (character < ' ' || character > '~')) {
                continue;
            }

            /* Another thread may have unmapped it while we waited. */
            if (!paging_user_range_valid((uintptr_t)(buffer + length), 1,
                                         true, false)) {
                frame->rax = (uint64_t)(int64_t)-14;
                return;
            }
            buffer[length++] = character;
            user_console_write(1, &character, 1);
            if (character == '\n') {
                break;
            }
        }
        frame->rax = length;
        return;
    }

    if (frame->rax == USER_SYSCALL_SPAWN) {
        if (frame->rdi == 1
            && (frame->rdx == 0 || frame->rdx > 255)) {
            frame->rax = (uint64_t)(int64_t)-36;
            return;
        }
        if (frame->rdi == 1
            && !paging_user_range_valid((uintptr_t)frame->rsi,
                                         (size_t)frame->rdx, false, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
            return;
        }
        frame->rax = (uint64_t)user_spawn_child(
            process, frame->rdi, (const char *)(uintptr_t)frame->rsi,
            (size_t)frame->rdx);
        return;
    }

    if (frame->rax == USER_SYSCALL_WAIT) {
        if (frame->rsi != 0
            && !paging_user_range_valid((uintptr_t)frame->rsi,
                                         sizeof(uint64_t), true, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
            return;
        }
        uint64_t child_process_id = 0;
        int64_t status = user_wait_child(process, frame->rdi,
                                         &child_process_id);
        if (status >= 0 && frame->rsi != 0
            && paging_user_range_valid((uintptr_t)frame->rsi,
                                       sizeof(uint64_t), true, false)) {
            *(uint64_t *)(uintptr_t)frame->rsi = child_process_id;
        }
        frame->rax = (uint64_t)status;
        return;
    }

    frame->rax = (uint64_t)(int64_t)-38;
}

struct user_exec_vectors {
    size_t argument_count;
    size_t environment_count;
    size_t string_size;
    uint16_t argument_offsets[USER_EXEC_VECTOR_MAX];
    uint16_t environment_offsets[USER_EXEC_VECTOR_MAX];
    char strings[USER_EXEC_STRING_MAX];
};

_Static_assert(offsetof(struct linux_syscall_frame, rax) == 96,
               "Linux syscall register frame layout");
_Static_assert(offsetof(struct linux_syscall_frame, fs_base) == 120,
               "Linux syscall FS-base frame layout");

struct linux_iovec {
    uintptr_t base;
    uint64_t length;
};

struct linux_timespec {
    int64_t seconds;
    int64_t nanoseconds;
};

struct linux_stat {
    uint64_t device;
    uint64_t inode;
    uint64_t link_count;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t padding;
    uint64_t device_type;
    int64_t size;
    int64_t block_size;
    int64_t block_count;
    struct linux_timespec access_time;
    struct linux_timespec modification_time;
    struct linux_timespec change_time;
    int64_t reserved[3];
};

struct linux_dirent64 {
    uint64_t inode;
    int64_t next_offset;
    uint16_t record_length;
    uint8_t type;
    char name[1];
} __attribute__((packed));

_Static_assert(sizeof(struct linux_stat) == 144, "Linux stat layout");
_Static_assert(offsetof(struct linux_dirent64, name) == 19,
               "Linux dirent64 layout");

struct linux_timeval {
    int64_t seconds;
    int64_t microseconds;
};

struct linux_timezone {
    int32_t minutes_west;
    int32_t dst_time;
};

struct linux_utsname {
    char sysname[LINUX_UTS_FIELD_SIZE];
    char nodename[LINUX_UTS_FIELD_SIZE];
    char release[LINUX_UTS_FIELD_SIZE];
    char version[LINUX_UTS_FIELD_SIZE];
    char machine[LINUX_UTS_FIELD_SIZE];
    char domainname[LINUX_UTS_FIELD_SIZE];
};

_Static_assert(sizeof(struct linux_utsname) == 390, "Linux utsname layout");

static void copy_utsname_field(char destination[LINUX_UTS_FIELD_SIZE],
                               const char *source) {
    size_t i = 0;
    while (i + 1 < LINUX_UTS_FIELD_SIZE && source[i] != '\0') {
        destination[i] = source[i];
        i++;
    }
    destination[i] = '\0';
    for (i++; i < LINUX_UTS_FIELD_SIZE; i++) {
        destination[i] = '\0';
    }
}

/* Console output for fd 1/2; stderr is drawn in the error colour. */
static user_console_sink console_sink;

void user_set_console_sink(user_console_sink sink) {
    console_sink = sink;
}

static user_console_input console_input;

void user_set_console_input(user_console_input input) {
    console_input = input;
}

static void user_console_write(uint64_t file_descriptor, const char *data,
                               size_t length) {
    if (console_sink != NULL) {
        console_sink(data, length, file_descriptor == 2);
        return;
    }
    if (file_descriptor == 2) {
        console_set_text_style(CONSOLE_TEXT_ERROR);
    }
    console_write_bytes(data, length);
    if (file_descriptor == 2) {
        console_set_text_style(CONSOLE_TEXT_NORMAL);
    }
}

static int64_t user_socket_send(struct user_process *process,
                                uint64_t descriptor, uintptr_t buffer,
                                size_t length, uint64_t flags,
                                const uint8_t *to_ip, uint16_t to_port,
                                bool kernel_buffer);
static int64_t user_socket_receive(struct user_process *process,
                                   uint64_t descriptor, uintptr_t buffer,
                                   size_t length, uint64_t flags,
                                   uint8_t from_ip[4], uint16_t *from_port,
                                   bool kernel_buffer);
static void user_socket_release(struct user_file_descriptor *file);

static bool user_linux_writev(struct user_process *process,
                              struct linux_syscall_frame *frame) {
    if (process == NULL || frame->rdi >= USER_DESCRIPTOR_CAPACITY) {
        frame->rax = (uint64_t)(int64_t)-9;
        return false;
    }
    bool default_output = (frame->rdi == 1 || frame->rdi == 2)
        && !process->descriptors[frame->rdi].used
        && !process->descriptors[frame->rdi].closed;
    struct user_file_descriptor *file = default_output ? NULL
        : user_file_descriptor(process, frame->rdi);
    if (!default_output
        && (file == NULL || !file->writable || file->directory)) {
        frame->rax = (uint64_t)(int64_t)-9;
        return false;
    }
    if (frame->rdx == 0 || frame->rdx > USER_IOV_MAX) {
        frame->rax = (uint64_t)(int64_t)-22;
        return false;
    }

    size_t iovec_count = (size_t)frame->rdx;
    if (iovec_count > SIZE_MAX / sizeof(struct linux_iovec)
        || !paging_user_range_valid((uintptr_t)frame->rsi,
                                    iovec_count * sizeof(struct linux_iovec),
                                    false, false)) {
        frame->rax = (uint64_t)(int64_t)-14;
        return false;
    }

    const struct linux_iovec *iovecs =
        (const struct linux_iovec *)(uintptr_t)frame->rsi;
    size_t total_length = 0;
    for (size_t i = 0; i < iovec_count; i++) {
        if (iovecs[i].length > USER_WRITE_MAX - total_length) {
            frame->rax = (uint64_t)(int64_t)-22;
            return false;
        }
        size_t length = (size_t)iovecs[i].length;
        if (length != 0
            && !paging_user_range_valid(iovecs[i].base, length, false, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
            return false;
        }
        total_length += length;
    }

    uint8_t buffer[USER_WRITE_MAX];
    size_t offset = 0;
    for (size_t i = 0; i < iovec_count; i++) {
        for (size_t j = 0; j < iovecs[i].length; j++) {
            buffer[offset++] = ((const uint8_t *)iovecs[i].base)[j];
        }
    }
    if (default_output) {
        user_console_write(frame->rdi, (const char *)buffer, total_length);
        frame->rax = total_length;
    } else if (file->kind == USER_OPEN_FILE_PIPE_WRITE) {
        frame->rax = (uint64_t)user_pipe_write(file, buffer, total_length,
                                               false);
    } else if (file->kind == USER_OPEN_FILE_SOCKET) {
        frame->rax = (uint64_t)user_socket_send(
            process, frame->rdi, (uintptr_t)buffer, total_length, 0, NULL, 0,
            true);
    } else {
        if (file->append) {
            struct vfs_metadata metadata;
            if (!vfs_object_metadata(file->object, &metadata)) {
                frame->rax = (uint64_t)(int64_t)-5;
                return false;
            }
            file->offset = metadata.size;
        }
        size_t bytes_written = 0;
        if (!vfs_object_write_at(file->object, file->offset, buffer,
                                 total_length, &bytes_written)) {
            frame->rax = (uint64_t)(int64_t)-5;
        } else {
            file->offset += bytes_written;
            frame->rax = bytes_written;
        }
    }
    return true;
}

static int64_t user_copy_file_path(uintptr_t address, char path[],
                                   size_t *path_length) {
    if (address == 0 || path == NULL || path_length == NULL) {
        return -14;
    }
    for (size_t i = 0; i < USER_FILE_PATH_CAPACITY; i++) {
        if (address > UINTPTR_MAX - i
            || !paging_user_range_valid(address + i, 1, false, false)) {
            return -14;
        }
        char character = *(const char *)(address + i);
        if (character == '\0') {
            if (i == 0) {
                return -2;
            }
            *path_length = i;
            return 0;
        }
        path[i] = character;
    }
    return -36;
}

static struct user_file_descriptor *user_file_descriptor(
    struct user_process *process, uint64_t descriptor) {
    if (process == NULL || descriptor >= USER_DESCRIPTOR_CAPACITY) {
        return NULL;
    }
    struct user_descriptor *entry = &process->descriptors[descriptor];
    if (!entry->used || entry->file_slot >= USER_OPEN_FILE_CAPACITY) {
        return NULL;
    }
    struct user_file_descriptor *file = &open_files[entry->file_slot];
    return file->used && file->references != 0 ? file : NULL;
}

static struct user_pipe *user_file_pipe(
    const struct user_file_descriptor *file) {
    if (file == NULL
        || (file->kind != USER_OPEN_FILE_PIPE_READ
            && file->kind != USER_OPEN_FILE_PIPE_WRITE)
        || file->pipe_slot >= USER_PIPE_CAPACITY
        || !pipes[file->pipe_slot].used) {
        return NULL;
    }
    return &pipes[file->pipe_slot];
}

static int64_t user_pipe_read(struct user_file_descriptor *file,
                              uint8_t *buffer, size_t length) {
    struct user_pipe *pipe = user_file_pipe(file);
    if (pipe == NULL || file->kind != USER_OPEN_FILE_PIPE_READ) {
        return -9;
    }
    if (length == 0) {
        return 0;
    }
    for (;;) {
        uint64_t flags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
        if (pipe->size != 0) {
            size_t copied = length < pipe->size ? length : pipe->size;
            /* Checked again here: blocking let other threads run. */
            if (!paging_user_range_valid((uintptr_t)buffer, copied, true,
                                         false)) {
                if ((flags & (1u << 9)) != 0) {
                    __asm__ volatile ("sti" : : : "memory");
                }
                return -14;
            }
            for (size_t i = 0; i < copied; i++) {
                buffer[i] = pipe->data[pipe->read_offset];
                pipe->read_offset =
                    (pipe->read_offset + 1) % USER_PIPE_BUFFER_CAPACITY;
            }
            pipe->size -= copied;
            scheduler_wake_event(pipe);
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return (int64_t)copied;
        }
        if (pipe->writers == 0) {
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return 0;
        }
        if (!scheduler_block_current_locked(pipe)) {
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return -5;
        }
        if ((flags & (1u << 9)) != 0) {
            __asm__ volatile ("sti" : : : "memory");
        }
    }
}

/* user_buffer: buffer is user memory (checked again after blocking). */
static int64_t user_pipe_write(struct user_file_descriptor *file,
                               const uint8_t *buffer, size_t length,
                               bool user_buffer) {
    struct user_pipe *pipe = user_file_pipe(file);
    if (pipe == NULL || file->kind != USER_OPEN_FILE_PIPE_WRITE) {
        return -9;
    }
    if (length == 0) {
        return 0;
    }
    for (;;) {
        uint64_t flags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
        if (pipe->readers == 0) {
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return -32;
        }
        if (length <= USER_PIPE_BUFFER_CAPACITY - pipe->size) {
            /* Checked again here: blocking let other threads run. */
            if (user_buffer
                && !paging_user_range_valid((uintptr_t)buffer, length, false,
                                            false)) {
                if ((flags & (1u << 9)) != 0) {
                    __asm__ volatile ("sti" : : : "memory");
                }
                return -14;
            }
            for (size_t i = 0; i < length; i++) {
                pipe->data[pipe->write_offset] = buffer[i];
                pipe->write_offset =
                    (pipe->write_offset + 1) % USER_PIPE_BUFFER_CAPACITY;
            }
            pipe->size += length;
            scheduler_wake_event(pipe);
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return (int64_t)length;
        }
        if (!scheduler_block_current_locked(pipe)) {
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return -5;
        }
        if ((flags & (1u << 9)) != 0) {
            __asm__ volatile ("sti" : : : "memory");
        }
    }
}

static bool user_close_open_file(struct user_file_descriptor *file) {
    if (file == NULL || !file->used) {
        return false;
    }
    if (file->kind == USER_OPEN_FILE_VFS) {
        return vfs_close_object(file->object);
    }
    if (file->kind == USER_OPEN_FILE_SOCKET) {
        user_socket_release(file);
        return true;
    }
    struct user_pipe *pipe = user_file_pipe(file);
    if (pipe == NULL) {
        return false;
    }
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    if (file->kind == USER_OPEN_FILE_PIPE_READ) {
        if (pipe->readers == 0) {
            if ((flags & (1u << 9)) != 0) __asm__ volatile ("sti");
            return false;
        }
        pipe->readers--;
    } else {
        if (pipe->writers == 0) {
            if ((flags & (1u << 9)) != 0) __asm__ volatile ("sti");
            return false;
        }
        pipe->writers--;
    }
    scheduler_wake_event(pipe);
    if (pipe->readers == 0 && pipe->writers == 0) {
        *pipe = (struct user_pipe){0};
    }
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
    return true;
}

static bool user_path_component_equal(const char *component, size_t length,
                                      const char *expected) {
    size_t i = 0;
    while (expected[i] != '\0') {
        if (i == length || component[i] != expected[i]) {
            return false;
        }
        i++;
    }
    return i == length;
}

static bool user_path_append_component(char path[], size_t *path_length,
                                       const char *component,
                                       size_t component_length) {
    size_t separator = *path_length > 1 ? 1 : 0;
    if (*path_length + separator + component_length
        >= USER_FILE_PATH_CAPACITY) {
        return false;
    }
    if (separator != 0) {
        path[(*path_length)++] = '/';
    }
    for (size_t i = 0; i < component_length; i++) {
        path[(*path_length)++] = component[i];
    }
    return true;
}

static void user_path_remove_component(char path[], size_t *path_length) {
    while (*path_length > 1 && path[*path_length - 1] != '/') {
        (*path_length)--;
    }
    if (*path_length > 1) {
        (*path_length)--;
    }
}

static int64_t user_resolve_file_path(struct user_process *process,
                                      int64_t directory_descriptor,
                                      uintptr_t path_address, char path[],
                                      size_t *path_length) {
    if (process == NULL || path == NULL || path_length == NULL) {
        return -22;
    }
    char input[USER_FILE_PATH_CAPACITY];
    size_t input_length;
    int64_t copied = user_copy_file_path(path_address, input, &input_length);
    if (copied != 0) {
        return copied;
    }

    size_t output_length;
    if (input[0] == '/') {
        path[0] = '/';
        output_length = 1;
    } else {
        const char *base;
        size_t base_length;
        if (directory_descriptor == LINUX_AT_FDCWD) {
            base = process->cwd;
            base_length = process->cwd_length;
        } else {
            struct user_file_descriptor *directory = user_file_descriptor(
                process, (uint64_t)directory_descriptor);
            if (directory == NULL) {
                return -9;
            }
            if (!directory->directory) {
                return -20;
            }
            if (!vfs_object_namespace_path(directory->object, path,
                                           USER_FILE_PATH_CAPACITY,
                                           &output_length)) {
                return -2;
            }
            base = path;
            base_length = output_length;
        }
        output_length = base_length;
        if (base != path) {
            for (size_t i = 0; i < base_length; i++) {
                path[i] = base[i];
            }
        }
    }

    size_t start = 0;
    while (start < input_length) {
        while (start < input_length && input[start] == '/') {
            start++;
        }
        size_t end = start;
        while (end < input_length && input[end] != '/') {
            end++;
        }
        size_t component_length = end - start;
        if (component_length != 0
            && !user_path_component_equal(input + start, component_length, ".")) {
            if (user_path_component_equal(input + start, component_length, "..")) {
                user_path_remove_component(path, &output_length);
            } else if (!user_path_append_component(path, &output_length,
                                                   input + start,
                                                   component_length)) {
                return -36;
            }
        }
        start = end + 1;
    }
    if (output_length == 0 || path[0] != '/') {
        return -2;
    }
    *path_length = output_length;
    return 0;
}

/* True when the process's identity may access the resolved path. */
static bool user_path_allowed(const struct user_process *process,
                              const char *path, size_t path_length,
                              enum permission_access access) {
    return permission_program_allows(&process->identity, path, path_length,
                                     access);
}

static int64_t user_linux_open_file(struct user_process *process,
                                    int64_t directory_descriptor,
                                    uintptr_t path_address, uint64_t flags) {
    const uint64_t supported_flags = LINUX_O_ACCMODE | LINUX_O_CREAT
        | LINUX_O_EXCL | LINUX_O_TRUNC | LINUX_O_APPEND | LINUX_O_DIRECTORY
        | LINUX_O_CLOEXEC | LINUX_O_NOCTTY | LINUX_O_NONBLOCK
        | LINUX_O_LARGEFILE;
    uint64_t access = flags & LINUX_O_ACCMODE;
    if (process == NULL || (flags & ~supported_flags) != 0
        || access == LINUX_O_ACCMODE
        || ((flags & LINUX_O_TRUNC) != 0 && access == LINUX_O_RDONLY)) {
        return -22;
    }

    char path[USER_FILE_PATH_CAPACITY];
    size_t path_length;
    int64_t copied = user_resolve_file_path(process, directory_descriptor,
                                            path_address, path, &path_length);
    if (copied != 0) {
        return copied;
    }
    bool modifies = access != LINUX_O_RDONLY
        || (flags & (LINUX_O_CREAT | LINUX_O_TRUNC)) != 0;
    if (!user_path_allowed(process, path, path_length,
                           modifies ? PERMISSION_WRITE : PERMISSION_READ)) {
        return -13;
    }

    size_t descriptor_slot = USER_DESCRIPTOR_CAPACITY;
    for (size_t i = USER_FIRST_FILE_DESCRIPTOR;
         i < USER_DESCRIPTOR_CAPACITY; i++) {
        if (!process->descriptors[i].used) {
            descriptor_slot = i;
            break;
        }
    }
    size_t file_slot = USER_OPEN_FILE_CAPACITY;
    for (size_t i = 0; i < USER_OPEN_FILE_CAPACITY; i++) {
        if (!open_files[i].used) {
            file_slot = i;
            break;
        }
    }
    if (descriptor_slot == USER_DESCRIPTOR_CAPACITY
        || file_slot == USER_OPEN_FILE_CAPACITY) {
        return -24;
    }

    struct vfs_metadata metadata;
    bool exists = vfs_stat(path, path_length, &metadata);
    if (!exists && (flags & LINUX_O_CREAT) == 0) {
        return -2;
    }
    if (!exists && (flags & LINUX_O_CREAT) != 0
        && vfs_path_read_only(path, path_length)) {
        return -30;
    }
    if (exists && (flags & (LINUX_O_CREAT | LINUX_O_EXCL))
                      == (LINUX_O_CREAT | LINUX_O_EXCL)) {
        return -17;
    }
    if (exists && metadata.type == VFS_NODE_DIRECTORY) {
        if (access != LINUX_O_RDONLY
            || (flags & (LINUX_O_CREAT | LINUX_O_TRUNC | LINUX_O_APPEND)) != 0) {
            return -21;
        }
    } else if ((flags & LINUX_O_DIRECTORY) != 0) {
        return -20;
    }
    if (exists && metadata.read_only && access != LINUX_O_RDONLY) {
        return -30;
    }
    if ((!exists || (flags & LINUX_O_TRUNC) != 0)
        && !vfs_create_file(path, path_length,
                            (flags & LINUX_O_TRUNC) != 0)) {
        return -5;
    }
    if (!vfs_stat(path, path_length, &metadata)) {
        return -5;
    }

    vfs_object_handle object;
    if (!vfs_open_object(path, path_length, &object, &metadata)) {
        return -24;
    }

    struct user_file_descriptor *file = &open_files[file_slot];
    *file = (struct user_file_descriptor) {
        .used = true,
        .references = 1,
        .kind = USER_OPEN_FILE_VFS,
        .directory = metadata.type == VFS_NODE_DIRECTORY,
        .readable = access != LINUX_O_WRONLY,
        .writable = metadata.type == VFS_NODE_FILE && access != LINUX_O_RDONLY,
        .append = (flags & LINUX_O_APPEND) != 0,
        .offset = (flags & LINUX_O_APPEND) != 0 ? metadata.size : 0,
        .object = object,
    };
    process->descriptors[descriptor_slot] = (struct user_descriptor) {
        .used = true,
        .close_on_exec = (flags & LINUX_O_CLOEXEC) != 0,
        .file_slot = (uint8_t)file_slot,
    };
    return (int64_t)descriptor_slot;
}

static int64_t user_linux_pipe(struct user_process *process,
                               uintptr_t descriptor_address,
                               uint64_t flags) {
    if (process == NULL || (flags & ~((uint64_t)LINUX_O_CLOEXEC)) != 0) {
        return -22;
    }
    if (!paging_user_range_valid(descriptor_address,
                                 2 * sizeof(uint32_t), true, false)) {
        return -14;
    }
    size_t descriptor_slots[2] = {
        USER_DESCRIPTOR_CAPACITY, USER_DESCRIPTOR_CAPACITY
    };
    size_t descriptor_count = 0;
    for (size_t i = USER_FIRST_FILE_DESCRIPTOR;
         i < USER_DESCRIPTOR_CAPACITY && descriptor_count < 2; i++) {
        if (!process->descriptors[i].used) {
            descriptor_slots[descriptor_count++] = i;
        }
    }
    size_t file_slots[2] = {
        USER_OPEN_FILE_CAPACITY, USER_OPEN_FILE_CAPACITY
    };
    size_t file_count = 0;
    for (size_t i = 0;
         i < USER_OPEN_FILE_CAPACITY && file_count < 2; i++) {
        if (!open_files[i].used) {
            file_slots[file_count++] = i;
        }
    }
    size_t pipe_slot = USER_PIPE_CAPACITY;
    for (size_t i = 0; i < USER_PIPE_CAPACITY; i++) {
        if (!pipes[i].used) {
            pipe_slot = i;
            break;
        }
    }
    if (descriptor_count != 2 || file_count != 2
        || pipe_slot == USER_PIPE_CAPACITY) {
        return -24;
    }

    pipes[pipe_slot] = (struct user_pipe) {
        .used = true,
        .readers = 1,
        .writers = 1,
    };
    open_files[file_slots[0]] = (struct user_file_descriptor) {
        .used = true,
        .references = 1,
        .kind = USER_OPEN_FILE_PIPE_READ,
        .readable = true,
        .pipe_slot = (uint8_t)pipe_slot,
    };
    open_files[file_slots[1]] = (struct user_file_descriptor) {
        .used = true,
        .references = 1,
        .kind = USER_OPEN_FILE_PIPE_WRITE,
        .writable = true,
        .pipe_slot = (uint8_t)pipe_slot,
    };
    bool close_on_exec = (flags & LINUX_O_CLOEXEC) != 0;
    process->descriptors[descriptor_slots[0]] = (struct user_descriptor) {
        .used = true,
        .close_on_exec = close_on_exec,
        .file_slot = (uint8_t)file_slots[0],
    };
    process->descriptors[descriptor_slots[1]] = (struct user_descriptor) {
        .used = true,
        .close_on_exec = close_on_exec,
        .file_slot = (uint8_t)file_slots[1],
    };
    uint32_t *descriptors = (uint32_t *)descriptor_address;
    descriptors[0] = (uint32_t)descriptor_slots[0];
    descriptors[1] = (uint32_t)descriptor_slots[1];
    return 0;
}

static int64_t user_close_descriptor(struct user_process *process,
                                     uint64_t descriptor) {
    if (process == NULL || descriptor >= USER_DESCRIPTOR_CAPACITY) {
        return -9;
    }
    struct user_descriptor *entry = &process->descriptors[descriptor];
    if (!entry->used) {
        if (descriptor < USER_FIRST_FILE_DESCRIPTOR && !entry->closed) {
            entry->closed = true;
            return 0;
        }
        return -9;
    }
    struct user_file_descriptor *file = user_file_descriptor(process,
                                                              descriptor);
    if (file == NULL || file->references == 0) {
        return -9;
    }
    if (file->references == 1 && !user_close_open_file(file)) {
        return -5;
    }
    file->references--;
    *entry = (struct user_descriptor){0};
    if (descriptor < USER_FIRST_FILE_DESCRIPTOR) {
        entry->closed = true;
    }
    if (file->references == 0) {
        *file = (struct user_file_descriptor){0};
    }
    return 0;
}

static int64_t user_duplicate_descriptor(struct user_process *process,
                                         uint64_t old_descriptor,
                                         uint64_t new_descriptor,
                                         bool exact, bool close_on_exec,
                                         bool reject_same) {
    struct user_file_descriptor *file = user_file_descriptor(
        process, old_descriptor);
    if (file == NULL) {
        return -9;
    }
    if (exact) {
        if (new_descriptor >= USER_DESCRIPTOR_CAPACITY) {
            return -9;
        }
        if (old_descriptor == new_descriptor) {
            return reject_same ? -22 : (int64_t)new_descriptor;
        }
    } else {
        new_descriptor = USER_FIRST_FILE_DESCRIPTOR;
        while (new_descriptor < USER_DESCRIPTOR_CAPACITY
               && process->descriptors[new_descriptor].used) {
            new_descriptor++;
        }
        if (new_descriptor == USER_DESCRIPTOR_CAPACITY) {
            return -24;
        }
    }
    if (file->references == UINT32_MAX) {
        return -24;
    }
    if (process->descriptors[new_descriptor].used
        || (new_descriptor < USER_FIRST_FILE_DESCRIPTOR
            && !process->descriptors[new_descriptor].closed)) {
        int64_t closed = user_close_descriptor(process, new_descriptor);
        if (closed != 0) {
            return closed;
        }
    }
    size_t file_slot = (size_t)(file - open_files);
    file->references++;
    process->descriptors[new_descriptor] = (struct user_descriptor) {
        .used = true,
        .close_on_exec = close_on_exec,
        .file_slot = (uint8_t)file_slot,
    };
    return (int64_t)new_descriptor;
}

static void user_close_all_files(struct user_process *process) {
    if (process == NULL) {
        return;
    }
    for (size_t i = 0; i < USER_DESCRIPTOR_CAPACITY; i++) {
        if (process->descriptors[i].used) {
            (void)user_close_descriptor(process, i);
        }
    }
}

static void user_linux_fill_stat(struct linux_stat *stat,
                                 const struct vfs_metadata *metadata) {
    for (size_t i = 0; i < sizeof(*stat); i++) {
        ((uint8_t *)stat)[i] = 0;
    }
    stat->device = metadata->device;
    stat->inode = metadata->inode;
    stat->link_count = metadata->type == VFS_NODE_DIRECTORY ? 2 : 1;
    stat->mode = metadata->type == VFS_NODE_DIRECTORY
        ? LINUX_MODE_DIRECTORY | 0777 : LINUX_MODE_REGULAR | 0666;
    stat->size = (int64_t)metadata->size;
    stat->block_size = 512;
    stat->block_count = (int64_t)((metadata->size + 511) / 512);
}

static int64_t user_linux_stat_path(struct user_process *process,
                                    int64_t directory_descriptor,
                                    uintptr_t path_address,
                                    uintptr_t stat_address) {
    if (!paging_user_range_valid(stat_address, sizeof(struct linux_stat),
                                 true, false)) {
        return -14;
    }
    char path[USER_FILE_PATH_CAPACITY];
    size_t path_length;
    int64_t copied = user_resolve_file_path(process, directory_descriptor,
                                            path_address, path, &path_length);
    if (copied != 0) {
        return copied;
    }
    if (!user_path_allowed(process, path, path_length, PERMISSION_READ)) {
        return -13;
    }
    struct vfs_metadata metadata;
    if (!vfs_stat(path, path_length, &metadata)) {
        return -2;
    }
    user_linux_fill_stat((struct linux_stat *)stat_address, &metadata);
    return 0;
}

static int64_t user_linux_change_directory(struct user_process *process,
                                           int64_t directory_descriptor,
                                           uintptr_t path_address) {
    char path[USER_FILE_PATH_CAPACITY];
    size_t path_length;
    int64_t resolved = user_resolve_file_path(process, directory_descriptor,
                                              path_address, path, &path_length);
    if (resolved != 0) {
        return resolved;
    }
    if (!user_path_allowed(process, path, path_length, PERMISSION_READ)) {
        return -13;
    }
    struct vfs_metadata metadata;
    if (!vfs_stat(path, path_length, &metadata)) {
        return -2;
    }
    if (metadata.type != VFS_NODE_DIRECTORY) {
        return -20;
    }
    process->cwd_length = path_length;
    for (size_t i = 0; i < path_length; i++) {
        process->cwd[i] = path[i];
    }
    return 0;
}

static int64_t user_linux_get_current_directory(struct user_process *process,
                                                uintptr_t buffer_address,
                                                size_t buffer_size) {
    if (process == NULL || buffer_size <= process->cwd_length) {
        return -34;
    }
    if (!paging_user_range_valid(buffer_address, process->cwd_length + 1,
                                 true, false)) {
        return -14;
    }
    char *buffer = (char *)buffer_address;
    for (size_t i = 0; i < process->cwd_length; i++) {
        buffer[i] = process->cwd[i];
    }
    buffer[process->cwd_length] = '\0';
    return (int64_t)(process->cwd_length + 1);
}

static int64_t user_linux_make_directory(struct user_process *process,
                                         int64_t directory_descriptor,
                                         uintptr_t path_address) {
    char path[USER_FILE_PATH_CAPACITY];
    size_t path_length;
    int64_t resolved = user_resolve_file_path(process, directory_descriptor,
                                              path_address, path, &path_length);
    if (resolved != 0) {
        return resolved;
    }
    if (!user_path_allowed(process, path, path_length, PERMISSION_WRITE)) {
        return -13;
    }
    struct vfs_metadata metadata;
    if (vfs_stat(path, path_length, &metadata)) {
        return -17;
    }
    if (vfs_path_read_only(path, path_length)) {
        return -30;
    }
    return vfs_create_directory(path, path_length) ? 0 : -5;
}

static int64_t user_linux_remove(struct user_process *process,
                                 int64_t directory_descriptor,
                                 uintptr_t path_address, bool directory) {
    char path[USER_FILE_PATH_CAPACITY];
    size_t path_length;
    int64_t resolved = user_resolve_file_path(process, directory_descriptor,
                                              path_address, path, &path_length);
    if (resolved != 0) {
        return resolved;
    }
    if (!user_path_allowed(process, path, path_length, PERMISSION_REMOVE)) {
        return -13;
    }
    struct vfs_metadata metadata;
    if (!vfs_stat(path, path_length, &metadata)) {
        return -2;
    }
    if (metadata.read_only) {
        return -30;
    }
    if (directory && metadata.type != VFS_NODE_DIRECTORY) {
        return -20;
    }
    if (!directory && metadata.type == VFS_NODE_DIRECTORY) {
        return -21;
    }
    bool not_empty;
    if (!vfs_remove(path, path_length, directory, &not_empty)) {
        return not_empty ? -39 : -5;
    }
    return 0;
}

static bool user_path_prefix_equal(const char *path, size_t path_length,
                                   const char *prefix,
                                   size_t prefix_length) {
    if (path_length < prefix_length
        || (path_length != prefix_length && path[prefix_length] != '/')) {
        return false;
    }
    for (size_t i = 0; i < prefix_length; i++) {
        char left = path[i];
        char right = prefix[i];
        if (left >= 'A' && left <= 'Z') left += 'a' - 'A';
        if (right >= 'A' && right <= 'Z') right += 'a' - 'A';
        if (left != right) {
            return false;
        }
    }
    return true;
}

static void user_rewrite_path(char path[], size_t *path_length,
                              const char *old_path, size_t old_length,
                              const char *new_path, size_t new_length) {
    if (!user_path_prefix_equal(path, *path_length, old_path, old_length)) {
        return;
    }
    size_t suffix_length = *path_length - old_length;
    if (new_length + suffix_length >= USER_FILE_PATH_CAPACITY) {
        return;
    }
    if (new_length > old_length) {
        for (size_t i = suffix_length; i > 0; i--) {
            path[new_length + i - 1] = path[old_length + i - 1];
        }
    } else if (new_length < old_length) {
        for (size_t i = 0; i < suffix_length; i++) {
            path[new_length + i] = path[old_length + i];
        }
    }
    for (size_t i = 0; i < new_length; i++) {
        path[i] = new_path[i];
    }
    *path_length = new_length + suffix_length;
}

static bool user_renamed_cwds_fit(const char *old_path, size_t old_length,
                                  size_t new_length) {
    for (size_t slot = 0; slot < SCHEDULER_TASK_CAPACITY; slot++) {
        struct user_process *candidate = &process_slots[slot];
        if (candidate->state != USER_PROCESS_UNUSED
            && user_path_prefix_equal(candidate->cwd, candidate->cwd_length,
                                      old_path, old_length)
            && new_length + candidate->cwd_length - old_length
                >= USER_FILE_PATH_CAPACITY) {
            return false;
        }
    }
    return true;
}

static void user_rewrite_renamed_cwds(const char *old_path,
                                      size_t old_length,
                                      const char *new_path,
                                      size_t new_length) {
    for (size_t slot = 0; slot < SCHEDULER_TASK_CAPACITY; slot++) {
        struct user_process *candidate = &process_slots[slot];
        if (candidate->state == USER_PROCESS_UNUSED) {
            continue;
        }
        user_rewrite_path(candidate->cwd, &candidate->cwd_length,
                          old_path, old_length, new_path, new_length);
    }
}

static int64_t user_linux_rename(struct user_process *process,
                                 int64_t old_directory_descriptor,
                                 uintptr_t old_path_address,
                                 int64_t new_directory_descriptor,
                                 uintptr_t new_path_address) {
    char old_path[USER_FILE_PATH_CAPACITY];
    char new_path[USER_FILE_PATH_CAPACITY];
    size_t old_path_length;
    size_t new_path_length;
    int64_t resolved = user_resolve_file_path(
        process, old_directory_descriptor, old_path_address,
        old_path, &old_path_length);
    if (resolved != 0) {
        return resolved;
    }
    resolved = user_resolve_file_path(
        process, new_directory_descriptor, new_path_address,
        new_path, &new_path_length);
    if (resolved != 0) {
        return resolved;
    }
    if (!user_path_allowed(process, old_path, old_path_length,
                           PERMISSION_REMOVE)
        || !user_path_allowed(process, new_path, new_path_length,
                              PERMISSION_REMOVE)) {
        return -13;
    }
    if (!user_renamed_cwds_fit(old_path, old_path_length,
                               new_path_length)) {
        return -36;
    }
    switch (vfs_rename(old_path, old_path_length, new_path, new_path_length)) {
        case VFS_RENAME_OK:
            user_rewrite_renamed_cwds(old_path, old_path_length,
                                      new_path, new_path_length);
            return 0;
        case VFS_RENAME_NOT_FOUND: return -2;
        case VFS_RENAME_NOT_DIRECTORY: return -20;
        case VFS_RENAME_IS_DIRECTORY: return -21;
        case VFS_RENAME_NOT_EMPTY: return -39;
        case VFS_RENAME_INVALID: return -22;
        case VFS_RENAME_NAME_TOO_LONG: return -36;
        case VFS_RENAME_READ_ONLY: return -30;
        case VFS_RENAME_CROSS_VOLUME: return -18;
        default: return -5;
    }
}

static int64_t user_linux_truncate_path(struct user_process *process,
                                        uintptr_t path_address,
                                        uint64_t new_size) {
    if (new_size > UINT32_MAX) {
        return -27;
    }
    char path[USER_FILE_PATH_CAPACITY];
    size_t path_length;
    int64_t resolved = user_resolve_file_path(process, LINUX_AT_FDCWD,
                                              path_address, path, &path_length);
    if (resolved != 0) {
        return resolved;
    }
    if (!user_path_allowed(process, path, path_length, PERMISSION_WRITE)) {
        return -13;
    }
    struct vfs_metadata metadata;
    if (!vfs_stat(path, path_length, &metadata)) {
        return -2;
    }
    if (metadata.type == VFS_NODE_DIRECTORY) {
        return -21;
    }
    if (metadata.read_only) {
        return -30;
    }
    return vfs_resize_file(path, path_length, (uint32_t)new_size) ? 0 : -5;
}

static int64_t user_linux_truncate_descriptor(struct user_process *process,
                                              uint64_t descriptor,
                                              uint64_t new_size) {
    if (new_size > UINT32_MAX) {
        return -27;
    }
    struct user_file_descriptor *file =
        user_file_descriptor(process, descriptor);
    if (file == NULL) {
        return -9;
    }
    if (file->kind != USER_OPEN_FILE_VFS) {
        return -22;
    }
    if (file->directory) {
        return -21;
    }
    if (!file->writable) {
        return -22;
    }
    return vfs_object_resize(file->object, (uint32_t)new_size) ? 0 : -5;
}

static int64_t user_linux_fstat(struct user_process *process,
                                uint64_t descriptor, uintptr_t stat_address) {
    if (!paging_user_range_valid(stat_address, sizeof(struct linux_stat),
                                 true, false)) {
        return -14;
    }
    struct linux_stat *stat = (struct linux_stat *)stat_address;
    if (descriptor < USER_FIRST_FILE_DESCRIPTOR
        && !process->descriptors[descriptor].used) {
        if (process->descriptors[descriptor].closed) {
            return -9;
        }
        for (size_t i = 0; i < sizeof(*stat); i++) {
            ((uint8_t *)stat)[i] = 0;
        }
        stat->device = 1;
        stat->inode = 100 + descriptor;
        stat->link_count = 1;
        stat->mode = LINUX_MODE_CHARACTER | 0666;
        stat->block_size = 1;
        return 0;
    }
    struct user_file_descriptor *file =
        user_file_descriptor(process, descriptor);
    if (file == NULL) {
        return -9;
    }
    if (file->kind == USER_OPEN_FILE_PIPE_READ
        || file->kind == USER_OPEN_FILE_PIPE_WRITE) {
        for (size_t i = 0; i < sizeof(*stat); i++) {
            ((uint8_t *)stat)[i] = 0;
        }
        stat->device = 2;
        stat->inode = 200 + file->pipe_slot;
        stat->link_count = 1;
        stat->mode = LINUX_MODE_FIFO | 0666;
        stat->block_size = USER_PIPE_BUFFER_CAPACITY;
        return 0;
    }
    if (file->kind == USER_OPEN_FILE_SOCKET) {
        for (size_t i = 0; i < sizeof(*stat); i++) {
            ((uint8_t *)stat)[i] = 0;
        }
        stat->device = 3;
        stat->inode = 300 + (uint64_t)(file - open_files);
        stat->link_count = 1;
        stat->mode = 0140000 | 0777; /* S_IFSOCK */
        stat->block_size = 4096;
        return 0;
    }
    struct vfs_metadata metadata;
    if (!vfs_object_metadata(file->object, &metadata)) {
        return -5;
    }
    user_linux_fill_stat(stat, &metadata);
    return 0;
}

static int64_t user_linux_getdents64(struct user_process *process,
                                     uint64_t descriptor,
                                     uintptr_t buffer_address,
                                     size_t buffer_length) {
    struct user_file_descriptor *file =
        user_file_descriptor(process, descriptor);
    if (file == NULL) {
        return -9;
    }
    if (!file->directory) {
        return -20;
    }
    if (buffer_length == 0 || buffer_length > USER_FILE_IO_MAX) {
        return -22;
    }
    if (!paging_user_range_valid(buffer_address, buffer_length, true, false)) {
        return -14;
    }
    size_t entry_count;
    if (!vfs_object_directory_count(file->object, &entry_count)) {
        return -5;
    }

    size_t written = 0;
    while (file->offset < entry_count + 2) {
        char name[256];
        size_t name_length;
        uint64_t inode;
        uint8_t type;
        if (file->offset == 0) {
            name[0] = '.';
            name[1] = '\0';
            name_length = 1;
            inode = 1;
            type = LINUX_DIRECTORY_TYPE;
        } else if (file->offset == 1) {
            name[0] = '.';
            name[1] = '.';
            name[2] = '\0';
            name_length = 2;
            inode = 1;
            type = LINUX_DIRECTORY_TYPE;
        } else {
            struct vfs_directory_entry entry;
            if (!vfs_object_directory_entry_at(file->object, file->offset - 2,
                                               &entry)) {
                return -5;
            }
            for (size_t i = 0; i <= entry.name_length; i++) {
                name[i] = entry.name[i];
            }
            name_length = entry.name_length;
            inode = entry.inode;
            type = entry.type == VFS_NODE_DIRECTORY
                ? LINUX_DIRECTORY_TYPE : LINUX_REGULAR_TYPE;
        }
        size_t record_length = (offsetof(struct linux_dirent64, name)
            + name_length + 1 + 7) & ~(size_t)7;
        if (record_length > buffer_length - written) {
            if (written == 0) {
                return -22;
            }
            break;
        }
        uint8_t *destination = (uint8_t *)buffer_address + written;
        for (size_t i = 0; i < record_length; i++) {
            destination[i] = 0;
        }
        struct linux_dirent64 *record = (struct linux_dirent64 *)destination;
        record->inode = inode;
        record->next_offset = (int64_t)(file->offset + 1);
        record->record_length = (uint16_t)record_length;
        record->type = type;
        for (size_t i = 0; i <= name_length; i++) {
            record->name[i] = name[i];
        }
        written += record_length;
        file->offset++;
    }
    return (int64_t)written;
}

static int64_t user_copy_exec_string(uintptr_t address,
                                     struct user_exec_vectors *vectors,
                                     uint16_t *offset) {
    if (address == 0 || vectors == NULL || offset == NULL
        || vectors->string_size >= USER_EXEC_STRING_MAX) {
        return -14;
    }
    *offset = (uint16_t)vectors->string_size;
    for (;;) {
        if (vectors->string_size == USER_EXEC_STRING_MAX
            || !paging_user_range_valid(address, 1, false, false)) {
            return -14;
        }
        char character = *(const char *)address;
        vectors->strings[vectors->string_size++] = character;
        if (character == '\0') {
            return 0;
        }
        if (address == UINTPTR_MAX) {
            return -14;
        }
        address++;
    }
}

static int64_t user_copy_exec_vector(uintptr_t vector_address,
                                     bool environment,
                                     struct user_exec_vectors *vectors) {
    if (vectors == NULL || (!environment && vector_address == 0)) {
        return -14;
    }
    if (vector_address == 0) {
        return 0;
    }
    for (size_t i = 0; i < USER_EXEC_VECTOR_MAX; i++) {
        if (vector_address > UINTPTR_MAX - i * sizeof(uintptr_t)
            || !paging_user_range_valid(vector_address
                    + i * sizeof(uintptr_t), sizeof(uintptr_t), false, false)) {
            return -14;
        }
        uintptr_t string_address =
            ((const uintptr_t *)vector_address)[i];
        if (string_address == 0) {
            if (!environment) {
                vectors->argument_count = i;
                return i == 0 ? -22 : 0;
            }
            vectors->environment_count = i;
            return 0;
        }
        uint16_t *offset = environment
            ? &vectors->environment_offsets[i] : &vectors->argument_offsets[i];
        int64_t copied = user_copy_exec_string(string_address, vectors, offset);
        if (copied != 0) {
            return copied;
        }
    }
    return -7;
}

static uintptr_t user_prepare_exec_stack(
    uint8_t *stack, const struct elf_load_plan *plan,
    const struct user_exec_vectors *vectors, uintptr_t interpreter_base,
    const char *exec_path, size_t exec_path_length) {
    /* exec_path NULL: AT_EXECFN points at argv[0] (a program started by
       the kernel, whose argv[0] is its path). */
    if (stack == NULL || plan == NULL || vectors == NULL
        || (exec_path != NULL && exec_path_length == 0)
        || exec_path_length >= USER_FILE_PATH_CAPACITY
        || vectors->argument_count == 0
        || vectors->argument_count > USER_EXEC_VECTOR_MAX
        || vectors->environment_count > USER_EXEC_VECTOR_MAX
        || vectors->string_size > USER_EXEC_STRING_MAX) {
        return 0;
    }
    size_t string_offset = PAGE_SIZE - vectors->string_size;
    for (size_t i = 0; i < vectors->string_size; i++) {
        stack[string_offset + i] = (uint8_t)vectors->strings[i];
    }
    size_t execfn_offset = string_offset + vectors->argument_offsets[0];
    size_t below_strings = string_offset;
    if (exec_path != NULL) {
        if (exec_path_length + 1 > string_offset) {
            return 0;
        }
        execfn_offset = string_offset - exec_path_length - 1;
        for (size_t i = 0; i < exec_path_length; i++) {
            stack[execfn_offset + i] = (uint8_t)exec_path[i];
        }
        stack[execfn_offset + exec_path_length] = '\0';
        below_strings = execfn_offset;
    }
    static const char platform[] = "x86_64";
    if (below_strings < sizeof(platform)) {
        return 0;
    }
    size_t platform_offset = below_strings - sizeof(platform);
    for (size_t i = 0; i < sizeof(platform); i++) {
        stack[platform_offset + i] = (uint8_t)platform[i];
    }
    if (platform_offset < 16) {
        return 0;
    }
    size_t random_offset = (platform_offset - 16) & ~(size_t)15;
    user_fill_random(stack + random_offset);
    size_t word_count = 1 + vectors->argument_count + 1
        + vectors->environment_count + 1 + 36
        + (plan->has_interpreter ? 2 : 0);
    if (word_count > random_offset / sizeof(uint64_t)) {
        return 0;
    }
    size_t stack_offset = (random_offset - word_count * sizeof(uint64_t))
        & ~(size_t)15;
    uint64_t *words = (uint64_t *)(stack + stack_offset);
    size_t word = 0;
    words[word++] = vectors->argument_count;
    for (size_t i = 0; i < vectors->argument_count; i++) {
        words[word++] = USER_STACK_ADDRESS + string_offset
            + vectors->argument_offsets[i];
    }
    words[word++] = 0;
    for (size_t i = 0; i < vectors->environment_count; i++) {
        words[word++] = USER_STACK_ADDRESS + string_offset
            + vectors->environment_offsets[i];
    }
    words[word++] = 0;
    words[word++] = USER_AT_PHDR;
    words[word++] = plan->program_header_address;
    words[word++] = USER_AT_PHENT;
    words[word++] = plan->program_header_size;
    words[word++] = USER_AT_PHNUM;
    words[word++] = plan->program_header_count;
    words[word++] = USER_AT_PAGESZ;
    words[word++] = PAGE_SIZE;
    if (plan->has_interpreter) {
        words[word++] = USER_AT_BASE;
        words[word++] = interpreter_base;
    }
    words[word++] = USER_AT_ENTRY;
    words[word++] = plan->entry_point;
    words[word++] = USER_AT_UID;
    words[word++] = 0;
    words[word++] = USER_AT_EUID;
    words[word++] = 0;
    words[word++] = USER_AT_GID;
    words[word++] = 0;
    words[word++] = USER_AT_EGID;
    words[word++] = 0;
    words[word++] = USER_AT_SECURE;
    words[word++] = 0;
    words[word++] = USER_AT_RANDOM;
    words[word++] = USER_STACK_ADDRESS + random_offset;
    words[word++] = USER_AT_EXECFN;
    words[word++] = USER_STACK_ADDRESS + execfn_offset;
    words[word++] = USER_AT_PLATFORM;
    words[word++] = USER_STACK_ADDRESS + platform_offset;
    words[word++] = USER_AT_HWCAP;
    words[word++] = user_cpu_hwcap();
    words[word++] = USER_AT_HWCAP2;
    words[word++] = 0;
    words[word++] = USER_AT_CLKTCK;
    words[word++] = USER_CLOCK_TICKS_PER_SECOND;
    words[word++] = USER_AT_FLAGS;
    words[word++] = 0;
    words[word++] = USER_AT_NULL;
    words[word] = 0;
    return USER_STACK_ADDRESS + stack_offset;
}

static bool user_free_mmap_physical(struct user_process *process) {
    bool released = true;
    for (size_t i = 0; i < USER_MMAP_MAX_PAGES; i++) {
        if (process->mmap_pages[i] != 0) {
            if (!physical_page_free(process->mmap_pages[i])) {
                released = false;
            }
            process->mmap_pages[i] = 0;
            process->mmap_flags[i] = 0;
        }
    }
    return released;
}

static bool user_clone_page_array(const uint64_t source[], size_t count,
                                  uint64_t destination[]) {
    for (size_t i = 0; i < count; i++) {
        if (source[i] == 0) {
            return false;
        }
        destination[i] = physical_page_alloc();
        uint8_t *source_page = physical_page_to_hhdm(source[i]);
        uint8_t *destination_page = physical_page_to_hhdm(destination[i]);
        if (destination[i] == 0 || source_page == NULL
            || destination_page == NULL) {
            for (size_t j = 0; j <= i; j++) {
                if (destination[j] != 0) {
                    (void)physical_page_free(destination[j]);
                    destination[j] = 0;
                }
            }
            return false;
        }
        for (size_t j = 0; j < PAGE_SIZE; j++) {
            destination_page[j] = source_page[j];
        }
    }
    return true;
}

static void user_discard_cloned_process(struct user_process *process) {
    if (process == NULL) {
        return;
    }
    if (process->address_space != 0) {
        (void)paging_address_space_destroy(process->address_space);
        process->address_space = 0;
    }
    (void)release_user_pages(0, process->image_pages,
                             process->image_page_count, 0);
    process->image_page_count = 0;
    process->image_segment_count = 0;
    (void)release_user_pages(0, process->stack_pages, USER_STACK_PAGES, 0);
    (void)release_user_pages(0, process->heap_pages,
                             process->heap_page_count, 0);
    process->heap_page_count = 0;
    (void)user_free_mmap_physical(process);
}

static bool user_map_cloned_process(struct user_process *process) {
    if (process == NULL) return false;
    for (size_t i = 0; i < process->image_segment_count; i++) {
        const struct user_image_segment *segment = &process->image_segments[i];
        if (segment->page_index > process->image_page_count
            || segment->page_count
                > process->image_page_count - segment->page_index
            || map_user_pages(segment->base,
                              process->image_pages + segment->page_index,
                              segment->page_count,
                              user_segment_paging_flags(segment->flags))
                != segment->page_count) return false;
    }
    if (map_user_pages(USER_STACK_BASE, process->stack_pages,
                       USER_STACK_PAGES,
                       PAGING_USER | PAGING_WRITABLE | PAGING_NO_EXECUTE)
            != USER_STACK_PAGES
        || map_user_pages(process->heap_base, process->heap_pages,
                          process->heap_page_count,
                          PAGING_USER | PAGING_WRITABLE | PAGING_NO_EXECUTE)
            != process->heap_page_count) {
        return false;
    }
    for (size_t i = 0; i < USER_MMAP_MAX_PAGES; i++) {
        if (process->mmap_pages[i] != 0
            && process->mmap_flags[i] != USER_MMAP_RESERVED
            && (process->mmap_flags[i] == 0
                || !paging_map_page(USER_MMAP_BASE + i * PAGE_SIZE,
                                    process->mmap_pages[i],
                                    process->mmap_flags[i]))) {
            return false;
        }
    }
    return true;
}

static struct user_thread *user_current_thread(struct user_process *process) {
    size_t slot = scheduler_current_slot();
    if (slot >= SCHEDULER_TASK_CAPACITY || !user_threads[slot].active
        || user_threads[slot].process != process) {
        return NULL;
    }
    return &user_threads[slot];
}

static struct user_futex_queue *user_futex_queue(
    struct user_process *process, uintptr_t address, bool create) {
    struct user_futex_queue *unused = NULL;
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY; i++) {
        if (futex_queues[i].used && futex_queues[i].process == process
            && futex_queues[i].address == address) {
            return &futex_queues[i];
        }
        if (!futex_queues[i].used && unused == NULL) {
            unused = &futex_queues[i];
        }
    }
    if (!create || unused == NULL) {
        return NULL;
    }
    *unused = (struct user_futex_queue) {
        .used = true,
        .process = process,
        .address = address,
    };
    return unused;
}

static size_t user_futex_wake_locked(struct user_process *process,
                                     uintptr_t address, size_t maximum) {
    struct user_futex_queue *queue = user_futex_queue(process, address, false);
    return queue == NULL ? 0
        : scheduler_wake_event_count(queue, maximum);
}

static int64_t user_linux_futex(struct user_process *process,
                                const struct linux_syscall_frame *frame) {
    uintptr_t address = (uintptr_t)frame->rdi;
    uint32_t operation = (uint32_t)frame->rsi;
    uint32_t command = operation & ~(uint32_t)LINUX_FUTEX_PRIVATE_FLAG;
    if (process == NULL || (address & (sizeof(uint32_t) - 1)) != 0
        || (operation & ~(uint32_t)(LINUX_FUTEX_PRIVATE_FLAG | 0x7f)) != 0
        || (command != LINUX_FUTEX_WAIT && command != LINUX_FUTEX_WAKE)) {
        return -22;
    }
    if (!paging_user_range_valid(address, sizeof(uint32_t),
                                 command == LINUX_FUTEX_WAIT, false)) {
        return -14;
    }
    if (command == LINUX_FUTEX_WAKE) {
        uint64_t flags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
        size_t maximum = frame->rdx > SIZE_MAX ? SIZE_MAX : (size_t)frame->rdx;
        size_t woken = user_futex_wake_locked(process, address, maximum);
        if ((flags & (1u << 9)) != 0) {
            __asm__ volatile ("sti" : : : "memory");
        }
        return (int64_t)woken;
    }
    if (frame->r10 != 0) {
        return -38;
    }

    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    if (*(volatile uint32_t *)address != (uint32_t)frame->rdx) {
        if ((flags & (1u << 9)) != 0) {
            __asm__ volatile ("sti" : : : "memory");
        }
        return -11;
    }
    struct user_futex_queue *queue = user_futex_queue(process, address, true);
    if (queue == NULL) {
        if ((flags & (1u << 9)) != 0) {
            __asm__ volatile ("sti" : : : "memory");
        }
        return -11;
    }
    queue->waiters++;
    bool blocked = scheduler_block_current_locked(queue);
    queue->waiters--;
    if (queue->waiters == 0) {
        *queue = (struct user_futex_queue) {0};
    }
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
    return blocked ? 0 : -11;
}

static int64_t user_linux_clone_thread(struct user_process *process,
                                       const struct linux_syscall_frame *frame) {
    const uint64_t required = LINUX_CLONE_VM | LINUX_CLONE_FS
        | LINUX_CLONE_FILES | LINUX_CLONE_SIGHAND | LINUX_CLONE_THREAD
        | LINUX_CLONE_SYSVSEM;
    const uint64_t allowed = required | LINUX_CLONE_SETTLS
        | LINUX_CLONE_PARENT_SETTID | LINUX_CLONE_CHILD_CLEARTID
        | LINUX_CLONE_CHILD_SETTID | LINUX_CLONE_DETACHED;
    struct user_thread *parent = user_current_thread(process);
    uintptr_t stack = (uintptr_t)frame->rsi;
    uintptr_t parent_tid = (uintptr_t)frame->rdx;
    uintptr_t child_tid = (uintptr_t)frame->r10;
    uintptr_t tls = (uintptr_t)frame->r8;
    if (parent == NULL || (frame->rdi & required) != required
        || (frame->rdi & ~allowed) != 0 || stack < sizeof(uint64_t)
        || !paging_user_range_valid(stack - sizeof(uint64_t),
                                    sizeof(uint64_t), true, false)
        || ((frame->rdi & LINUX_CLONE_PARENT_SETTID) != 0
            && !paging_user_range_valid(parent_tid, sizeof(uint32_t),
                                        true, false))
        || ((frame->rdi & (LINUX_CLONE_CHILD_CLEARTID
                           | LINUX_CLONE_CHILD_SETTID)) != 0
            && !paging_user_range_valid(child_tid, sizeof(uint32_t),
                                        true, false))
        || ((frame->rdi & LINUX_CLONE_SETTLS) != 0
            && !paging_user_range_valid(tls, 1, false, false))) {
        return -22;
    }
    struct user_thread_job *job = NULL;
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY - 1; i++) {
        if (!thread_jobs[i].active) {
            job = &thread_jobs[i];
            break;
        }
    }
    if (job == NULL || next_process_id == 0) {
        return -11;
    }
    uint64_t thread_id = __atomic_fetch_add(&next_process_id, 1,
                                             __ATOMIC_RELAXED);
    *job = (struct user_thread_job) {
        .active = true,
        .thread_id = thread_id,
        .process = process,
        .stack_pointer = stack,
        .clear_child_tid = (frame->rdi & LINUX_CLONE_CHILD_CLEARTID) != 0
            ? child_tid : 0,
        .signal_mask = parent->signal_mask,
        .frame = *frame,
    };
    job->frame.rax = 0;
    if ((frame->rdi & LINUX_CLONE_SETTLS) != 0) {
        job->frame.fs_base = tls;
    }
    if (!scheduler_spawn(user_thread_worker, job, &job->task_id)) {
        *job = (struct user_thread_job) {0};
        return -11;
    }
    if ((frame->rdi & LINUX_CLONE_PARENT_SETTID) != 0) {
        *(uint32_t *)parent_tid = (uint32_t)thread_id;
    }
    if ((frame->rdi & LINUX_CLONE_CHILD_SETTID) != 0) {
        *(uint32_t *)child_tid = (uint32_t)thread_id;
    }
    process->active_threads++;
    return (int64_t)thread_id;
}

static int64_t user_linux_fork(struct user_process *parent,
                               const struct linux_syscall_frame *frame) {
    if (parent == NULL || frame == NULL || next_process_id == 0) {
        return -11;
    }
    /* The stack may be the initial page or mmap memory (the SDK's crt0
       moves it there); both are copied for the child. */
    uintptr_t user_stack = scheduler_linux_syscall_user_stack();
    if (user_stack < USER_APP_MIN_ADDRESS + sizeof(uint64_t)
        || user_stack >= USER_ADDRESS_LIMIT
        || !paging_user_range_valid(user_stack - sizeof(uint64_t),
                                    sizeof(uint64_t), true, false)) {
        return -14;
    }
    struct user_child_job *job = NULL;
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY - 1; i++) {
        if (child_jobs[i].state == USER_CHILD_UNUSED) {
            job = &child_jobs[i];
            break;
        }
    }
    if (job == NULL) {
        return -11;
    }
    for (size_t i = 0; i < USER_DESCRIPTOR_CAPACITY; i++) {
        if (!parent->descriptors[i].used) {
            continue;
        }
        uint8_t file_slot = parent->descriptors[i].file_slot;
        if (file_slot >= USER_OPEN_FILE_CAPACITY
            || !open_files[file_slot].used
            || open_files[file_slot].references == UINT32_MAX) {
            return -24;
        }
    }

    /* Too large for the kernel stack. fork does not block, so one copy
       is enough. */
    static struct user_process fork_child;
    struct user_process *child = &fork_child;
    *child = (struct user_process) {0};
    child->fault_status = parent->fault_status;
    child->faulted = false;
    child->elf_image = parent->elf_image;
    child->initial_break = parent->initial_break;
    child->current_break = parent->current_break;
    child->heap_base = parent->heap_base;
    child->heap_page_count = parent->heap_page_count;
    child->image_segment_count = parent->image_segment_count;
    child->image_page_count = parent->image_page_count;
    for (size_t i = 0; i < LINUX_SIGNAL_COUNT; i++) {
        child->signal_actions[i] = parent->signal_actions[i];
    }
    for (size_t i = 0; i < parent->image_segment_count; i++) {
        child->image_segments[i] = parent->image_segments[i];
    }
    child->identity = parent->identity;
    for (size_t i = 0; i < sizeof(child->name); i++) {
        child->name[i] = parent->name[i];
    }
    child->cwd_length = parent->cwd_length;
    for (size_t i = 0; i < parent->cwd_length; i++) {
        child->cwd[i] = parent->cwd[i];
    }
    for (size_t i = 0; i < USER_MMAP_MAX_PAGES; i++) {
        child->mmap_flags[i] = parent->mmap_flags[i];
    }
    bool cloned = user_clone_page_array(parent->image_pages,
                                         parent->image_page_count,
                                         child->image_pages)
        && user_clone_page_array(parent->stack_pages, USER_STACK_PAGES,
                                 child->stack_pages)
        && user_clone_page_array(parent->heap_pages,
                                 parent->heap_page_count,
                                 child->heap_pages);
    for (size_t i = 0; cloned && i < USER_MMAP_MAX_PAGES; i++) {
        if (parent->mmap_pages[i] != 0) {
            cloned = user_clone_page_array(&parent->mmap_pages[i], 1,
                                           &child->mmap_pages[i]);
        }
    }
    if (!cloned) {
        user_discard_cloned_process(child);
        return -12;
    }

    uint64_t parent_address_space = parent->address_space;
    bool kernel_active = scheduler_activate_current_address_space(
        paging_kernel_address_space());
    bool address_space_created = kernel_active
        && paging_address_space_create(&child->address_space);
    bool child_active = address_space_created
        && scheduler_activate_current_address_space(child->address_space);
    bool mapped = child_active && user_map_cloned_process(child);
    bool parent_restored = kernel_active
        && scheduler_activate_current_address_space(parent_address_space);
    if (!mapped || !parent_restored) {
        if (!parent_restored) {
            console_write_string("OPOS: fork could not restore parent address space\n");
            __asm__ volatile ("cli");
            for (;;) {
                __asm__ volatile ("hlt");
            }
        }
        user_discard_cloned_process(child);
        return -12;
    }

    *job = (struct user_child_job) {
        .parent_process_id = parent->id,
        .forked = true,
        .fork_stack_pointer = user_stack,
        .fork_frame = *frame,
        .fork_signal_mask = user_current_thread(parent)->signal_mask,
        .state = USER_CHILD_RUNNING,
        .started = true,
    };
    job->fork_frame.rax = 0;
    if (!scheduler_spawn(user_child_worker, job, &job->task_id)) {
        job->state = USER_CHILD_UNUSED;
        user_discard_cloned_process(child);
        return -11;
    }
    size_t child_slot;
    if (!scheduler_task_slot(job->task_id, &child_slot)
        || child_slot >= SCHEDULER_TASK_CAPACITY
        || !process_resource_state_clean(&process_slots[child_slot])) {
        console_write_string("OPOS: fork scheduler slot ownership failed\n");
        __asm__ volatile ("cli");
        for (;;) {
            __asm__ volatile ("hlt");
        }
    }
    child->id = __atomic_fetch_add(&next_process_id, 1, __ATOMIC_RELAXED);
    child->state = USER_PROCESS_RUNNING;
    child->active_threads = 1;
    for (size_t i = 0; i < USER_DESCRIPTOR_CAPACITY; i++) {
        child->descriptors[i] = parent->descriptors[i];
        if (parent->descriptors[i].used) {
            open_files[child->descriptors[i].file_slot].references++;
        }
    }
    process_slots[child_slot] = *child;
    job->process_id = child->id;
    return (int64_t)child->id;
}

static int64_t user_linux_execve(struct user_process *process,
                                 struct linux_syscall_frame *frame) {
    if (process->active_threads != 1) {
        return -16;
    }
    char path[USER_FILE_PATH_CAPACITY];
    size_t path_length;
    int64_t resolved = user_resolve_file_path(
        process, LINUX_AT_FDCWD, (uintptr_t)frame->rdi, path, &path_length);
    if (resolved != 0) {
        return resolved;
    }
    if (!user_path_allowed(process, path, path_length, PERMISSION_READ)) {
        return -13;
    }
    struct user_exec_vectors vectors = {0};
    int64_t copied = user_copy_exec_vector((uintptr_t)frame->rsi, false,
                                           &vectors);
    if (copied == 0) {
        copied = user_copy_exec_vector((uintptr_t)frame->rdx, true, &vectors);
    }
    if (copied != 0) {
        return copied;
    }

    size_t task_slot = scheduler_current_slot();
    if (task_slot >= SCHEDULER_TASK_CAPACITY) {
        return -5;
    }
    uint8_t *exec_image = user_exec_image_buffer(task_slot);
    if (exec_image == NULL) {
        return -12;
    }
    size_t image_size;
    if (!vfs_read_file(path, path_length, exec_image, USER_EXEC_IMAGE_MAX,
                       &image_size)) {
        return image_size > USER_EXEC_IMAGE_MAX ? -7 : -2;
    }
    struct elf_load_plan plan = {0};
    if (!elf_inspect_user_image(exec_image, image_size,
                                PAGE_SIZE, USER_PIE_BASE,
                                USER_APP_MIN_ADDRESS,
                                USER_MMAP_BASE, &plan)) {
        return -8;
    }
    struct elf_load_plan interpreter_plan = {0};
    size_t interpreter_size = 0;
    if (plan.has_interpreter) {
        if (plan.interpreter_path_length == 0
            || plan.interpreter_path[0] != '/'
            || !user_path_allowed(process, plan.interpreter_path,
                                  plan.interpreter_path_length,
                                  PERMISSION_READ)) {
            return -13;
        }
        if (!vfs_read_file(plan.interpreter_path,
                           plan.interpreter_path_length,
                           user_interpreter_images[task_slot],
                           USER_INTERPRETER_IMAGE_MAX, &interpreter_size)) {
            return -2;
        }
        if (!elf_inspect_user_interpreter(
                user_interpreter_images[task_slot], interpreter_size,
                PAGE_SIZE, USER_MMAP_BASE, USER_MMAP_BASE,
                USER_MMAP_BASE + USER_MMAP_MAX_PAGES * PAGE_SIZE,
                &interpreter_plan)) {
            return -8;
        }
    }

    /* Page lists for up to ELF_MAX_SEGMENT_PAGES pages are too big for
       the kernel stack; each task has its own. */
    struct user_load_pages *load = &user_load_scratch[task_slot];
    *load = (struct user_load_pages) {0};
    uint64_t *image_physical = load->image_physical;
    uint8_t **image_buffers = load->image_buffers;
    uint64_t *interpreter_physical = load->interpreter_physical;
    uint8_t **interpreter_buffers = load->interpreter_buffers;
    uint64_t stack_physical[USER_STACK_PAGES] = {0};
    uint8_t *stack_buffers[USER_STACK_PAGES] = {0};
    if (!allocate_user_pages(plan.total_page_count, image_physical,
                             image_buffers)
        || !allocate_user_pages(interpreter_plan.total_page_count,
                                interpreter_physical, interpreter_buffers)
        || !allocate_user_pages(USER_STACK_PAGES, stack_physical,
                                stack_buffers)) {
        release_user_pages(0, image_physical, plan.total_page_count, 0);
        release_user_pages(0, interpreter_physical,
                           interpreter_plan.total_page_count, 0);
        release_user_pages(0, stack_physical, USER_STACK_PAGES, 0);
        return -12;
    }
    zero_user_pages(plan.total_page_count, image_buffers);
    zero_user_pages(interpreter_plan.total_page_count, interpreter_buffers);
    zero_user_pages(USER_STACK_PAGES, stack_buffers);
    if (!elf_load_user_image(exec_image, image_size, &plan,
                             image_buffers, plan.total_page_count, PAGE_SIZE)
        || (plan.has_interpreter
            && !elf_load_user_image(
                user_interpreter_images[task_slot], interpreter_size,
                &interpreter_plan, interpreter_buffers,
                interpreter_plan.total_page_count, PAGE_SIZE))) {
        release_user_pages(0, image_physical, plan.total_page_count, 0);
        release_user_pages(0, interpreter_physical,
                           interpreter_plan.total_page_count, 0);
        release_user_pages(0, stack_physical, USER_STACK_PAGES, 0);
        return -8;
    }
    uintptr_t initial_stack = user_prepare_exec_stack(stack_buffers[USER_STACK_ARGUMENT_PAGE], &plan,
                                                       &vectors,
                                                       plan.has_interpreter
                                                           ? USER_MMAP_BASE : 0,
                                                       path, path_length);
    if (initial_stack == 0) {
        release_user_pages(0, image_physical, plan.total_page_count, 0);
        release_user_pages(0, interpreter_physical,
                           interpreter_plan.total_page_count, 0);
        release_user_pages(0, stack_physical, USER_STACK_PAGES, 0);
        return -7;
    }

    uint64_t old_address_space = process->address_space;
    uint64_t new_address_space = 0;
    bool kernel_space_active = scheduler_activate_current_address_space(
        paging_kernel_address_space());
    bool new_space_created = kernel_space_active
        && paging_address_space_create(&new_address_space);
    if (!new_space_created
        || !scheduler_activate_current_address_space(new_address_space)) {
        if (new_address_space != 0) {
            (void)paging_address_space_destroy(new_address_space);
        }
        if (kernel_space_active) {
            (void)scheduler_activate_current_address_space(old_address_space);
        }
        release_user_pages(0, image_physical, plan.total_page_count, 0);
        release_user_pages(0, interpreter_physical,
                           interpreter_plan.total_page_count, 0);
        release_user_pages(0, stack_physical, USER_STACK_PAGES, 0);
        return -12;
    }
    size_t image_mapped[ELF_MAX_LOAD_SEGMENTS] = {0};
    size_t interpreter_mapped[ELF_MAX_LOAD_SEGMENTS] = {0};
    bool image_mapped_all = map_elf_plan(&plan, image_physical, image_mapped);
    bool interpreter_mapped_all = image_mapped_all
        && (!plan.has_interpreter
            || map_elf_plan(&interpreter_plan, interpreter_physical,
                            interpreter_mapped));
    bool stack_mapped = interpreter_mapped_all
        && map_user_pages(USER_STACK_BASE, stack_physical, USER_STACK_PAGES,
                          PAGING_USER | PAGING_WRITABLE | PAGING_NO_EXECUTE)
               == USER_STACK_PAGES;
    bool image_valid = stack_mapped
        && elf_plan_permissions_valid(&plan)
        && (!plan.has_interpreter
            || elf_plan_permissions_valid(&interpreter_plan))
        && paging_user_range_valid(USER_STACK_BASE,
                                   USER_STACK_PAGES * PAGE_SIZE, true, false)
        && !paging_user_range_valid(USER_STACK_BASE,
                                    USER_STACK_PAGES * PAGE_SIZE, false, true);
    if (!image_valid) {
        (void)scheduler_activate_current_address_space(old_address_space);
        (void)paging_address_space_destroy(new_address_space);
        release_user_pages(0, image_physical, plan.total_page_count, 0);
        release_user_pages(0, interpreter_physical,
                           interpreter_plan.total_page_count, 0);
        release_user_pages(0, stack_physical, USER_STACK_PAGES, 0);
        return -12;
    }

    if (!paging_address_space_destroy(old_address_space)) {
        console_write_string("OPOS: execve could not release old page tables\n");
    }
    bool old_image_freed = release_user_pages(
        0, process->image_pages, process->image_page_count, 0);
    bool old_stack_freed = release_user_pages(0, process->stack_pages,
                                              USER_STACK_PAGES, 0);
    bool old_heap_freed = release_user_pages(
        0, process->heap_pages, process->heap_page_count, 0);
    bool old_mmap_freed = user_free_mmap_physical(process);
    if (!old_image_freed || !old_stack_freed
        || !old_heap_freed || !old_mmap_freed) {
        console_write_string("OPOS: execve leaked old image pages\n");
    }

    process->address_space = new_address_space;
    process->elf_image = true;
    process->faulted = false;
    process->fault_status = 0;
    const struct elf_load_segment *writable =
        &plan.segments[plan.writable_segment];
    process->initial_break = writable->address + writable->memory_size;
    process->current_break = process->initial_break;
    process->heap_base = (process->initial_break + PAGE_SIZE - 1)
        & ~(uintptr_t)(PAGE_SIZE - 1);
    process->heap_page_count = 0;
    user_set_process_name(process, path, path_length);
    process->image_segment_count = plan.segment_count;
    process->image_page_count = plan.total_page_count;
    for (size_t i = 0; i < plan.segment_count; i++) {
        process->image_segments[i] = (struct user_image_segment) {
            .base = plan.segments[i].mapping_base,
            .page_index = plan.segments[i].page_index,
            .page_count = plan.segments[i].page_count,
            .flags = plan.segments[i].flags,
        };
    }
    for (size_t i = 0; i < plan.total_page_count; i++) {
        process->image_pages[i] = image_physical[i];
    }
    if (plan.has_interpreter) {
        for (size_t i = 0; i < interpreter_plan.segment_count; i++) {
            const struct elf_load_segment *segment =
                &interpreter_plan.segments[i];
            size_t mmap_index =
                (segment->mapping_base - USER_MMAP_BASE) / PAGE_SIZE;
            for (size_t page = 0; page < segment->page_count; page++) {
                process->mmap_pages[mmap_index + page] =
                    interpreter_physical[segment->page_index + page];
                process->mmap_flags[mmap_index + page] =
                    user_segment_paging_flags(segment->flags);
            }
        }
    }
    for (size_t i = 0; i < USER_STACK_PAGES; i++) {
        process->stack_pages[i] = stack_physical[i];
    }
    for (size_t i = 0; i < USER_DESCRIPTOR_CAPACITY; i++) {
        if (process->descriptors[i].used
            && process->descriptors[i].close_on_exec) {
            (void)user_close_descriptor(process, i);
        }
    }
    for (size_t i = 0; i < LINUX_SIGNAL_COUNT; i++) {
        if (process->signal_actions[i].handler != LINUX_SIG_IGN) {
            process->signal_actions[i] = (struct linux_signal_action) {0};
        }
    }

    *frame = (struct linux_syscall_frame) {
        .instruction_pointer = plan.has_interpreter
            ? interpreter_plan.entry_point : plan.entry_point,
        .flags = 0x202,
        .fs_base = 0,
    };
    scheduler_reset_fpu_state();
    scheduler_linux_syscall_set_user_stack(initial_stack);
    return 0;
}

/*
 * OPOS native calls. They use the SYSCALL instruction like the Linux ABI,
 * with numbers 0x4f500000 and up ("OP"), which Linux never uses. The SDK
 * (sdk/include/opos.h) documents them; numbers and structures are only
 * ever added to, never changed, so old programs keep working.
 */
#define OPOS_SYSCALL_BASE 0x4f500000ULL
#define OPOS_SYSCALL_ABI_VERSION (OPOS_SYSCALL_BASE + 0)
#define OPOS_SYSCALL_USER_INFO (OPOS_SYSCALL_BASE + 1)
#define OPOS_ABI_VERSION 1
#define OPOS_USER_ADMIN 1u
#define OPOS_USER_ACCOUNT 2u

struct opos_user_info {
    uint32_t size;
    uint32_t flags;
    char name[32];
    char home[128];
};

static int64_t user_opos_user_info(const struct user_process *process,
                                   uintptr_t address) {
    if (!paging_user_range_valid(address, sizeof(uint32_t), false, false)) {
        return -14;
    }
    /* The caller states how much room it has, so a newer kernel can grow
       the structure without writing past an older program's copy. */
    uint32_t size = *(const uint32_t *)address;
    if (size < sizeof(struct opos_user_info)) {
        return -22;
    }
    if (!paging_user_range_valid(address, sizeof(struct opos_user_info),
                                 true, false)) {
        return -14;
    }
    struct opos_user_info info = {.size = sizeof(info)};
    if (process->identity.admin) {
        info.flags |= OPOS_USER_ADMIN;
    }
    size_t account = process->identity.account;
    if (account != PERMISSION_NO_ACCOUNT && account < accounts_count()) {
        info.flags |= OPOS_USER_ACCOUNT;
        const char *name = accounts_name(account);
        size_t length = 0;
        while (name[length] != '\0' && length + 1 < sizeof(info.name)) {
            info.name[length] = name[length];
            length++;
        }
        static const char root[] = ACCOUNT_HOME_ROOT "/";
        size_t used = 0;
        for (size_t i = 0; root[i] != '\0'; i++) {
            info.home[used++] = root[i];
        }
        for (size_t i = 0; i < length && used + 1 < sizeof(info.home); i++) {
            info.home[used++] = name[i];
        }
    }
    *(struct opos_user_info *)address = info;
    return 0;
}


/*
 * Windows (ABI 1): one window per program, shown by the desktop. See
 * desktop_app_window_* in desktop.c and sdk/include/opos.h.
 */
#define OPOS_SYSCALL_WINDOW_CREATE (OPOS_SYSCALL_BASE + 0x10)
#define OPOS_SYSCALL_WINDOW_PRESENT (OPOS_SYSCALL_BASE + 0x11)
#define OPOS_SYSCALL_WINDOW_EVENT (OPOS_SYSCALL_BASE + 0x12)
#define OPOS_SYSCALL_WINDOW_SIZE (OPOS_SYSCALL_BASE + 0x13)
#define OPOS_WINDOW_TITLE_MAX 64
#define OPOS_WINDOW_PIXELS_MAX 4096

/* Returns true when the process must end (frame->rax holds the status). */
static bool user_opos_window(const struct user_process *process,
                             struct linux_syscall_frame *frame) {
    uint64_t number = frame->rax;
    int64_t result = -22;
    if (number == OPOS_SYSCALL_WINDOW_CREATE) {
        uintptr_t title_address = (uintptr_t)frame->rdi;
        size_t title_length = (size_t)frame->rsi;
        char title[OPOS_WINDOW_TITLE_MAX];
        if (title_length > sizeof(title)) {
            title_length = sizeof(title);
        }
        if (!paging_user_range_valid(title_address, title_length, false,
                                     false)) {
            result = -14;
        } else if (frame->rdx > 0xffffffffULL || frame->r10 > 0xffffffffULL) {
            result = -22;
        } else {
            for (size_t i = 0; i < title_length; i++) {
                title[i] = ((const char *)title_address)[i];
            }
            result = desktop_app_window_create(
                process->id, title, title_length, (uint32_t)frame->rdx,
                (uint32_t)frame->r10);
        }
    } else if (number == OPOS_SYSCALL_WINDOW_PRESENT) {
        uintptr_t pixels = (uintptr_t)frame->rdi;
        uint64_t width = frame->rsi;
        uint64_t height = frame->rdx;
        uint64_t stride = frame->r10;
        if (width == 0 || height == 0 || width > OPOS_WINDOW_PIXELS_MAX
            || height > OPOS_WINDOW_PIXELS_MAX || stride < width
            || stride > OPOS_WINDOW_PIXELS_MAX) {
            result = -22;
        } else if ((pixels & 3) != 0
                   || !paging_user_range_valid(
                          pixels, ((height - 1) * stride + width) * 4,
                          false, false)) {
            result = -14;
        } else {
            /* Interrupts stay off: nothing can unmap the pixels while they
               are copied. */
            result = desktop_app_window_present(
                process->id, (const uint32_t *)pixels, (uint32_t)width,
                (uint32_t)height, (uint32_t)stride);
        }
    } else if (number == OPOS_SYSCALL_WINDOW_EVENT) {
        uintptr_t address = (uintptr_t)frame->rdi;
        struct desktop_app_event event = {0};
        bool terminate = false;
        if (!paging_user_range_valid(address, sizeof(event), true, false)) {
            result = -14;
        } else {
            result = desktop_app_window_event(process->id, &event,
                                              (int64_t)frame->rsi,
                                              &terminate);
            if (terminate) {
                frame->rax = 128 + 9; /* as if killed */
                return true;
            }
            /* The desktop ran meanwhile; the memory may be gone. */
            if (result > 0) {
                if (paging_user_range_valid(address, sizeof(event), true,
                                            false)) {
                    *(struct desktop_app_event *)address = event;
                } else {
                    result = -14;
                }
            }
        }
    } else if (number == OPOS_SYSCALL_WINDOW_SIZE) {
        uint64_t size = desktop_app_window_size(process->id);
        result = size == 0 ? -9 : (int64_t)size;
    } else {
        result = -38;
    }
    frame->rax = (uint64_t)result;
    return false;
}

/* ------------------------------------------------------------- sockets */

/*
 * Linux socket calls for AF_INET stream (TCP) and datagram (UDP) sockets
 * on top of net.c. Kernel code is not preempted, so the net.c calls here
 * cannot interleave with the desktop's net_poll. Blocking calls poll the
 * network themselves and sleep a tick at a time; a kill (scheduler
 * interrupt) ends them with EINTR.
 */
#define LINUX_AF_UNSPEC 0
#define LINUX_AF_INET 2
#define LINUX_SOCK_STREAM 1
#define LINUX_SOCK_DGRAM 2
#define LINUX_SOCK_NONBLOCK 0x800
#define LINUX_SOCK_CLOEXEC 0x80000
#define LINUX_MSG_PEEK 0x2
#define LINUX_MSG_DONTWAIT 0x40
#define LINUX_SOL_SOCKET 1
#define LINUX_SO_TYPE 3
#define LINUX_SO_ERROR 4
#define LINUX_POLLIN 0x001
#define LINUX_POLLPRI 0x002
#define LINUX_POLLOUT 0x004
#define LINUX_POLLERR 0x008
#define LINUX_POLLHUP 0x010
#define LINUX_POLLNVAL 0x020
#define USER_POLL_MAX 64
#define USER_UDP_MAX 1472

enum user_socket_state {
    USER_SOCKET_NEW,
    USER_SOCKET_CONNECTING,
    USER_SOCKET_CONNECTED,
    USER_SOCKET_LISTENING,
};

/* One tick of waiting; false when the process is being killed. */
static uint8_t user_socket_event;

static bool user_socket_wait(void) {
    net_poll();
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    bool woke = scheduler_block_current_until_locked(&user_socket_event,
                                                     timer_ticks() + 1);
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
    return woke;
}

static int64_t user_read_sockaddr(uintptr_t address, uint64_t length,
                                  uint8_t ip[4], uint16_t *port,
                                  uint16_t *family) {
    if (address == 0) {
        return -14;
    }
    if (length < 2 || !paging_user_range_valid(address, 2, false, false)) {
        return length < 2 ? -22 : -14;
    }
    const uint8_t *bytes = (const uint8_t *)address;
    *family = (uint16_t)(bytes[0] | bytes[1] << 8);
    if (*family == LINUX_AF_UNSPEC) {
        return 0;
    }
    if (*family != LINUX_AF_INET) {
        return -97;
    }
    if (length < 16) {
        return -22;
    }
    if (!paging_user_range_valid(address, 16, false, false)) {
        return -14;
    }
    *port = (uint16_t)(bytes[2] << 8 | bytes[3]);
    for (int i = 0; i < 4; i++) {
        ip[i] = bytes[4 + i];
    }
    return 0;
}

/* Fills a sockaddr_in and its length (both may be 0 = not wanted). */
static int64_t user_write_sockaddr(uintptr_t address, uintptr_t length_address,
                                   const uint8_t ip[4], uint16_t port) {
    if (address == 0 || length_address == 0) {
        return 0;
    }
    if (!paging_user_range_valid(length_address, 4, true, false)) {
        return -14;
    }
    uint32_t capacity = *(uint32_t *)length_address;
    uint8_t bytes[16] = {LINUX_AF_INET, 0, (uint8_t)(port >> 8),
                         (uint8_t)port, ip[0], ip[1], ip[2], ip[3]};
    uint32_t length = capacity < 16 ? capacity : 16;
    if (length != 0 && !paging_user_range_valid(address, length, true, false)) {
        return -14;
    }
    for (uint32_t i = 0; i < length; i++) {
        ((uint8_t *)address)[i] = bytes[i];
    }
    *(uint32_t *)length_address = 16;
    return 0;
}

static struct user_file_descriptor *user_socket_file(
    struct user_process *process, uint64_t descriptor, int64_t *error) {
    struct user_file_descriptor *file = user_file_descriptor(process,
                                                             descriptor);
    if (file == NULL) {
        *error = -9;
        return NULL;
    }
    if (file->kind != USER_OPEN_FILE_SOCKET) {
        *error = -88; /* ENOTSOCK */
        return NULL;
    }
    return file;
}

/* Gives a new open file a descriptor; -24 when either table is full. */
static int64_t user_install_file(struct user_process *process,
                                 const struct user_file_descriptor *file,
                                 bool close_on_exec) {
    size_t descriptor = USER_FIRST_FILE_DESCRIPTOR;
    while (descriptor < USER_DESCRIPTOR_CAPACITY
           && process->descriptors[descriptor].used) {
        descriptor++;
    }
    size_t slot = 0;
    while (slot < USER_OPEN_FILE_CAPACITY && open_files[slot].used) {
        slot++;
    }
    if (descriptor == USER_DESCRIPTOR_CAPACITY
        || slot == USER_OPEN_FILE_CAPACITY) {
        return -24;
    }
    open_files[slot] = *file;
    open_files[slot].used = true;
    open_files[slot].references = 1;
    process->descriptors[descriptor] = (struct user_descriptor){
        .used = true,
        .close_on_exec = close_on_exec,
        .file_slot = (uint8_t)slot,
    };
    return (int64_t)descriptor;
}

static void user_socket_release(struct user_file_descriptor *file) {
    if (file->socket_handle < 0) {
        return;
    }
    if (file->socket_type == LINUX_SOCK_DGRAM) {
        net_udp_close(file->socket_handle);
    } else if (file->socket_state == USER_SOCKET_LISTENING) {
        net_tcp_unlisten(file->socket_handle);
    } else {
        net_tcp_close(file->socket_handle);
    }
    file->socket_handle = -1;
}

/* A UDP socket gets its port on first use when it was never bound. */
static int64_t user_udp_ready(struct user_file_descriptor *file) {
    if (file->socket_handle >= 0) {
        return 0;
    }
    int id = net_udp_open(0);
    if (id < 0) {
        return id;
    }
    file->socket_handle = id;
    return 0;
}

/* Settles a pending connect: 0 connected, -115 still going, or the
   error (the socket is then unconnected again). */
static int64_t user_tcp_settle(struct user_file_descriptor *file) {
    if (file->socket_state != USER_SOCKET_CONNECTING) {
        return 0;
    }
    enum net_tcp_status status = net_tcp_status(file->socket_handle);
    if (status == NET_TCP_CONNECTING) {
        return -115;
    }
    if (status == NET_TCP_FAILED) {
        int error = net_tcp_error(file->socket_handle);
        net_tcp_close(file->socket_handle);
        file->socket_handle = -1;
        file->socket_state = USER_SOCKET_NEW;
        file->socket_error = error != 0 ? error : -111;
        return file->socket_error;
    }
    file->socket_state = USER_SOCKET_CONNECTED;
    return 0;
}

static int64_t user_linux_socket(struct user_process *process,
                                 uint64_t domain, uint64_t type,
                                 uint64_t protocol) {
    if (domain != LINUX_AF_INET) {
        return -97; /* EAFNOSUPPORT */
    }
    uint64_t kind = type & 0xff;
    if ((kind != LINUX_SOCK_STREAM && kind != LINUX_SOCK_DGRAM)
        || (type & ~(0xffull | LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC))
               != 0) {
        return -94; /* ESOCKTNOSUPPORT */
    }
    if (protocol != 0
        && protocol != (kind == LINUX_SOCK_STREAM ? 6u : 17u)) {
        return -93; /* EPROTONOSUPPORT */
    }
    struct user_file_descriptor file = {
        .kind = USER_OPEN_FILE_SOCKET,
        .readable = true,
        .writable = true,
        .nonblocking = (type & LINUX_SOCK_NONBLOCK) != 0,
        .socket_type = (uint8_t)kind,
        .socket_state = USER_SOCKET_NEW,
        .socket_handle = -1,
    };
    return user_install_file(process, &file,
                             (type & LINUX_SOCK_CLOEXEC) != 0);
}

static int64_t user_linux_bind(struct user_process *process,
                               uint64_t descriptor, uintptr_t address,
                               uint64_t length) {
    int64_t error = 0;
    struct user_file_descriptor *file = user_socket_file(process, descriptor,
                                                         &error);
    if (file == NULL) {
        return error;
    }
    uint8_t ip[4] = {0, 0, 0, 0};
    uint16_t port = 0;
    uint16_t family = 0;
    error = user_read_sockaddr(address, length, ip, &port, &family);
    if (error != 0) {
        return error;
    }
    if (family != LINUX_AF_INET) {
        return -97;
    }
    if (file->socket_type == LINUX_SOCK_DGRAM) {
        if (file->socket_handle >= 0) {
            return -22;
        }
        int id = net_udp_open(port);
        if (id < 0) {
            return id;
        }
        file->socket_handle = id;
        return 0;
    }
    if (file->socket_state != USER_SOCKET_NEW || file->socket_port != 0) {
        return -22;
    }
    if (port == 0) {
        port = net_tcp_pick_port();
    } else if (!net_tcp_port_free(port)) {
        return -98; /* EADDRINUSE */
    }
    file->socket_port = port;
    return 0;
}

static int64_t user_linux_listen(struct user_process *process,
                                 uint64_t descriptor, uint64_t backlog) {
    int64_t error = 0;
    struct user_file_descriptor *file = user_socket_file(process, descriptor,
                                                         &error);
    if (file == NULL) {
        return error;
    }
    if (file->socket_type != LINUX_SOCK_STREAM) {
        return -95; /* EOPNOTSUPP */
    }
    if (file->socket_state == USER_SOCKET_LISTENING) {
        return 0;
    }
    if (file->socket_state != USER_SOCKET_NEW) {
        return -22;
    }
    if (file->socket_port == 0) {
        file->socket_port = net_tcp_pick_port();
    }
    int listener = net_tcp_listen(file->socket_port,
                                  (int)(backlog > 64 ? 64 : backlog));
    if (listener < 0) {
        return listener;
    }
    file->socket_handle = listener;
    file->socket_state = USER_SOCKET_LISTENING;
    return 0;
}

static int64_t user_linux_accept(struct user_process *process,
                                 uint64_t descriptor, uintptr_t address,
                                 uintptr_t length_address, uint64_t flags) {
    int64_t error = 0;
    struct user_file_descriptor *file = user_socket_file(process, descriptor,
                                                         &error);
    if (file == NULL) {
        return error;
    }
    if (file->socket_state != USER_SOCKET_LISTENING
        || (flags & ~(uint64_t)(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC))
               != 0) {
        return -22;
    }
    int handle;
    for (;;) {
        handle = net_tcp_accept(file->socket_handle);
        if (handle != -11) {
            break;
        }
        if (file->nonblocking) {
            return -11;
        }
        if (!user_socket_wait()) {
            return -4;
        }
        /* The descriptor may have been closed by another thread. */
        file = user_socket_file(process, descriptor, &error);
        if (file == NULL || file->socket_state != USER_SOCKET_LISTENING) {
            return file == NULL ? error : -22;
        }
    }
    if (handle < 0) {
        return handle;
    }
    struct user_file_descriptor accepted = {
        .kind = USER_OPEN_FILE_SOCKET,
        .readable = true,
        .writable = true,
        .nonblocking = (flags & LINUX_SOCK_NONBLOCK) != 0,
        .socket_type = LINUX_SOCK_STREAM,
        .socket_state = USER_SOCKET_CONNECTED,
        .socket_handle = handle,
    };
    int64_t new_descriptor = user_install_file(
        process, &accepted, (flags & LINUX_SOCK_CLOEXEC) != 0);
    if (new_descriptor < 0) {
        net_tcp_close(handle);
        return new_descriptor;
    }
    uint8_t local_ip[4];
    uint8_t remote_ip[4];
    uint16_t local_port;
    uint16_t remote_port;
    net_tcp_endpoints(handle, local_ip, &local_port, remote_ip, &remote_port);
    (void)user_write_sockaddr(address, length_address, remote_ip,
                              remote_port);
    return new_descriptor;
}

static int64_t user_linux_connect(struct user_process *process,
                                  uint64_t descriptor, uintptr_t address,
                                  uint64_t length) {
    int64_t error = 0;
    struct user_file_descriptor *file = user_socket_file(process, descriptor,
                                                         &error);
    if (file == NULL) {
        return error;
    }
    uint8_t ip[4] = {0, 0, 0, 0};
    uint16_t port = 0;
    uint16_t family = 0;
    error = user_read_sockaddr(address, length, ip, &port, &family);
    if (error != 0) {
        return error;
    }
    if (file->socket_type == LINUX_SOCK_DGRAM) {
        if (family == LINUX_AF_UNSPEC) {
            file->socket_state = USER_SOCKET_NEW;
            return 0;
        }
        error = user_udp_ready(file);
        if (error != 0) {
            return error;
        }
        for (int i = 0; i < 4; i++) {
            file->socket_peer_ip[i] = ip[i];
        }
        file->socket_peer_port = port;
        file->socket_state = USER_SOCKET_CONNECTED;
        return 0;
    }
    if (family != LINUX_AF_INET) {
        return -97;
    }
    if (file->socket_state == USER_SOCKET_CONNECTED) {
        return -106; /* EISCONN */
    }
    if (file->socket_state == USER_SOCKET_LISTENING) {
        return -22;
    }
    if (file->socket_state == USER_SOCKET_NEW) {
        if (ip[0] == 127) {
            return -111; /* nothing listens on loopback */
        }
        if (port == 0) {
            return -22;
        }
        int handle = net_tcp_connect_from(ip, port, file->socket_port);
        if (handle < 0) {
            uint8_t local[4];
            net_local_address(local);
            return local[0] == 0 && local[1] == 0 ? -101 : -105;
        }
        file->socket_handle = handle;
        file->socket_state = USER_SOCKET_CONNECTING;
        file->socket_error = 0;
        if (file->nonblocking) {
            return -115; /* EINPROGRESS */
        }
    } else if (file->nonblocking) {
        error = user_tcp_settle(file);
        return error == -115 ? -114 : error; /* EALREADY */
    }
    for (;;) {
        error = user_tcp_settle(file);
        if (error != -115) {
            if (error != 0) {
                file->socket_error = 0; /* reported here instead */
            }
            return error;
        }
        if (!user_socket_wait()) {
            return -4;
        }
        file = user_socket_file(process, descriptor, &error);
        if (file == NULL) {
            return error;
        }
    }
}

/* Sends from user memory (or kernel memory when kernel_buffer). */
static int64_t user_socket_send(struct user_process *process,
                                uint64_t descriptor, uintptr_t buffer,
                                size_t length, uint64_t flags,
                                const uint8_t *to_ip, uint16_t to_port,
                                bool kernel_buffer) {
    int64_t error = 0;
    struct user_file_descriptor *file = user_socket_file(process, descriptor,
                                                         &error);
    if (file == NULL) {
        return error;
    }
    if (!kernel_buffer && length != 0
        && !paging_user_range_valid(buffer, length, false, false)) {
        return -14;
    }
    bool nonblocking = file->nonblocking || (flags & LINUX_MSG_DONTWAIT);
    if (file->socket_type == LINUX_SOCK_DGRAM) {
        uint8_t ip[4];
        uint16_t port;
        if (to_ip != NULL) {
            for (int i = 0; i < 4; i++) {
                ip[i] = to_ip[i];
            }
            port = to_port;
        } else if (file->socket_state == USER_SOCKET_CONNECTED) {
            for (int i = 0; i < 4; i++) {
                ip[i] = file->socket_peer_ip[i];
            }
            port = file->socket_peer_port;
        } else {
            return -89; /* EDESTADDRREQ */
        }
        error = user_udp_ready(file);
        if (error != 0) {
            return error;
        }
        return net_udp_send(file->socket_handle, ip, port,
                            (const void *)buffer, length);
    }
    if (file->socket_state == USER_SOCKET_CONNECTING) {
        while ((error = user_tcp_settle(file)) == -115) {
            if (nonblocking) {
                return -11;
            }
            if (!user_socket_wait()) {
                return -4;
            }
            file = user_socket_file(process, descriptor, &error);
            if (file == NULL) {
                return error;
            }
        }
        if (error != 0) {
            return error;
        }
    }
    if (file->socket_state != USER_SOCKET_CONNECTED) {
        return -107; /* ENOTCONN */
    }
    size_t sent = 0;
    while (sent < length || length == 0) {
        if (file->socket_write_shut || !net_tcp_can_send(file->socket_handle)) {
            int tcp_error = net_tcp_error(file->socket_handle);
            error = tcp_error != 0 ? tcp_error : -32; /* EPIPE */
            return sent != 0 ? (int64_t)sent : error;
        }
        if (length == 0) {
            return 0;
        }
        size_t queued = net_tcp_send(file->socket_handle,
                                     (const uint8_t *)buffer + sent,
                                     length - sent);
        sent += queued;
        if (sent == length) {
            break;
        }
        if (queued == 0) {
            if (nonblocking) {
                return sent != 0 ? (int64_t)sent : -11;
            }
            if (!user_socket_wait()) {
                return sent != 0 ? (int64_t)sent : -4;
            }
            file = user_socket_file(process, descriptor, &error);
            if (file == NULL) {
                return sent != 0 ? (int64_t)sent : error;
            }
            if (!kernel_buffer
                && !paging_user_range_valid(buffer + sent, length - sent,
                                            false, false)) {
                return sent != 0 ? (int64_t)sent : -14;
            }
        }
    }
    return (int64_t)sent;
}

/* Receives into user memory (or kernel memory when kernel_buffer). */
static int64_t user_socket_receive(struct user_process *process,
                                   uint64_t descriptor, uintptr_t buffer,
                                   size_t length, uint64_t flags,
                                   uint8_t from_ip[4], uint16_t *from_port,
                                   bool kernel_buffer) {
    int64_t error = 0;
    struct user_file_descriptor *file = user_socket_file(process, descriptor,
                                                         &error);
    if (file == NULL) {
        return error;
    }
    if ((flags & LINUX_MSG_PEEK) != 0) {
        return -95;
    }
    bool nonblocking = file->nonblocking || (flags & LINUX_MSG_DONTWAIT);
    for (;;) {
        if (!kernel_buffer && length != 0
            && !paging_user_range_valid(buffer, length, true, false)) {
            return -14;
        }
        if (file->socket_type == LINUX_SOCK_DGRAM) {
            error = user_udp_ready(file);
            if (error != 0) {
                return error;
            }
            uint8_t ip[4];
            uint16_t port;
            long got = net_udp_receive(file->socket_handle, (void *)buffer,
                                       length, ip, &port);
            if (got != -11) {
                if (got >= 0 && from_ip != NULL) {
                    for (int i = 0; i < 4; i++) {
                        from_ip[i] = ip[i];
                    }
                    *from_port = port;
                }
                return got;
            }
        } else {
            if (file->socket_state == USER_SOCKET_CONNECTING) {
                error = user_tcp_settle(file);
                if (error != 0 && error != -115) {
                    return error;
                }
            }
            if (file->socket_state == USER_SOCKET_CONNECTED) {
                size_t got = net_tcp_receive(file->socket_handle,
                                             (void *)buffer, length);
                if (got != 0 || length == 0) {
                    if (from_ip != NULL) {
                        uint8_t local[4];
                        uint16_t local_port;
                        net_tcp_endpoints(file->socket_handle, local,
                                          &local_port, from_ip, from_port);
                    }
                    return (int64_t)got;
                }
                enum net_tcp_status status =
                    net_tcp_status(file->socket_handle);
                if (status == NET_TCP_FINISHED || file->socket_read_shut) {
                    return 0;
                }
                if (status == NET_TCP_FAILED) {
                    int tcp_error = net_tcp_error(file->socket_handle);
                    return tcp_error != 0 ? tcp_error : 0;
                }
            } else if (file->socket_state != USER_SOCKET_CONNECTING) {
                return -107;
            }
        }
        if (nonblocking) {
            return -11;
        }
        if (!user_socket_wait()) {
            return -4;
        }
        file = user_socket_file(process, descriptor, &error);
        if (file == NULL) {
            return error;
        }
    }
}

static int64_t user_linux_sendto(struct user_process *process,
                                 struct linux_syscall_frame *frame) {
    uint8_t ip[4];
    uint16_t port = 0;
    uint16_t family = 0;
    const uint8_t *to = NULL;
    if (frame->r8 != 0) {
        int64_t error = user_read_sockaddr((uintptr_t)frame->r8, frame->r9,
                                           ip, &port, &family);
        if (error != 0) {
            return error;
        }
        if (family != LINUX_AF_INET) {
            return -97;
        }
        to = ip;
    }
    size_t length = frame->rdx > USER_SOCKET_IO_MAX ? USER_SOCKET_IO_MAX
                                                    : (size_t)frame->rdx;
    return user_socket_send(process, frame->rdi, (uintptr_t)frame->rsi,
                            length, frame->r10, to, port, false);
}

static int64_t user_linux_recvfrom(struct user_process *process,
                                   struct linux_syscall_frame *frame) {
    uint8_t ip[4] = {0, 0, 0, 0};
    uint16_t port = 0;
    size_t length = frame->rdx > USER_SOCKET_IO_MAX ? USER_SOCKET_IO_MAX
                                                    : (size_t)frame->rdx;
    int64_t result = user_socket_receive(
        process, frame->rdi, (uintptr_t)frame->rsi, length, frame->r10, ip,
        &port, false);
    if (result >= 0 && frame->r8 != 0) {
        int64_t error = user_write_sockaddr((uintptr_t)frame->r8,
                                            (uintptr_t)frame->r9, ip, port);
        if (error != 0) {
            return error;
        }
    }
    return result;
}

/* struct msghdr: name 0, namelen 8, iov 16, iovlen 24, control 32,
   controllen 40, flags 48. */
static int64_t user_linux_message(struct user_process *process,
                                  struct linux_syscall_frame *frame,
                                  bool send) {
    uintptr_t message = (uintptr_t)frame->rsi;
    if (!paging_user_range_valid(message, 56, !send, false)) {
        return -14;
    }
    uintptr_t name = *(uint64_t *)message;
    uint32_t name_length = *(uint32_t *)(message + 8);
    uintptr_t iovecs = *(uint64_t *)(message + 16);
    uint64_t count = *(uint64_t *)(message + 24);
    if (count > USER_IOV_MAX
        || (count != 0
            && !paging_user_range_valid(
                iovecs, count * sizeof(struct linux_iovec), false, false))) {
        return count > USER_IOV_MAX ? -22 : -14;
    }
    const struct linux_iovec *vector = (const struct linux_iovec *)iovecs;
    int64_t error = 0;
    struct user_file_descriptor *file = user_socket_file(process, frame->rdi,
                                                         &error);
    if (file == NULL) {
        return error;
    }
    bool datagram = file->socket_type == LINUX_SOCK_DGRAM;
    uint8_t ip[4] = {0, 0, 0, 0};
    uint16_t port = 0;
    if (send) {
        const uint8_t *to = NULL;
        if (name != 0) {
            uint16_t family = 0;
            error = user_read_sockaddr(name, name_length, ip, &port, &family);
            if (error != 0) {
                return error;
            }
            to = ip;
        }
        if (datagram) {
            /* One datagram: gather the pieces first. */
            uint8_t gathered[USER_UDP_MAX];
            size_t total = 0;
            for (uint64_t i = 0; i < count; i++) {
                if (vector[i].length > sizeof(gathered) - total) {
                    return -90; /* EMSGSIZE */
                }
                if (vector[i].length != 0
                    && !paging_user_range_valid(vector[i].base,
                                                vector[i].length, false,
                                                false)) {
                    return -14;
                }
                for (uint64_t j = 0; j < vector[i].length; j++) {
                    gathered[total++] = ((const uint8_t *)vector[i].base)[j];
                }
            }
            return user_socket_send(process, frame->rdi,
                                    (uintptr_t)gathered, total, frame->rdx,
                                    to, port, true);
        }
        int64_t total = 0;
        for (uint64_t i = 0; i < count; i++) {
            if (vector[i].length == 0) {
                continue;
            }
            int64_t sent = user_socket_send(process, frame->rdi,
                                            vector[i].base, vector[i].length,
                                            frame->rdx, NULL, 0, false);
            if (sent < 0) {
                return total != 0 ? total : sent;
            }
            total += sent;
            if ((uint64_t)sent < vector[i].length) {
                break;
            }
        }
        return total;
    }
    int64_t result;
    if (datagram) {
        uint8_t received[USER_UDP_MAX];
        result = user_socket_receive(process, frame->rdi,
                                     (uintptr_t)received, sizeof(received),
                                     frame->rdx, ip, &port, true);
        if (result >= 0) {
            size_t offset = 0;
            for (uint64_t i = 0; i < count && offset < (size_t)result; i++) {
                size_t piece = vector[i].length;
                if (piece > (size_t)result - offset) {
                    piece = (size_t)result - offset;
                }
                if (piece != 0
                    && !paging_user_range_valid(vector[i].base, piece, true,
                                                false)) {
                    return -14;
                }
                for (size_t j = 0; j < piece; j++) {
                    ((uint8_t *)vector[i].base)[j] = received[offset + j];
                }
                offset += piece;
            }
            result = (int64_t)offset;
        }
    } else {
        /* A stream read fills the first non-empty piece (short reads are
           allowed). */
        uint64_t i = 0;
        while (i < count && vector[i].length == 0) {
            i++;
        }
        result = user_socket_receive(
            process, frame->rdi, i < count ? vector[i].base : 0,
            i < count ? vector[i].length : 0, frame->rdx, ip, &port, false);
    }
    if (result >= 0) {
        if (name != 0) {
            error = user_write_sockaddr(name, message + 8, ip, port);
            if (error != 0) {
                return error;
            }
        }
        *(uint64_t *)(message + 40) = 0;          /* no control data */
        *(uint32_t *)(message + 48) = 0;          /* flags */
    }
    return result;
}

static int64_t user_linux_shutdown(struct user_process *process,
                                   uint64_t descriptor, uint64_t how) {
    int64_t error = 0;
    struct user_file_descriptor *file = user_socket_file(process, descriptor,
                                                         &error);
    if (file == NULL) {
        return error;
    }
    if (how > 2) {
        return -22;
    }
    if (file->socket_state != USER_SOCKET_CONNECTED) {
        return -107;
    }
    if (how != 1) {
        file->socket_read_shut = true;
    }
    if (how != 0 && file->socket_type == LINUX_SOCK_STREAM) {
        file->socket_write_shut = true;
        net_tcp_shutdown(file->socket_handle);
    }
    return 0;
}

static int64_t user_linux_socket_name(struct user_process *process,
                                      uint64_t descriptor, uintptr_t address,
                                      uintptr_t length_address, bool peer) {
    int64_t error = 0;
    struct user_file_descriptor *file = user_socket_file(process, descriptor,
                                                         &error);
    if (file == NULL) {
        return error;
    }
    uint8_t ip[4] = {0, 0, 0, 0};
    uint16_t port = 0;
    if (file->socket_type == LINUX_SOCK_DGRAM) {
        if (peer) {
            if (file->socket_state != USER_SOCKET_CONNECTED) {
                return -107;
            }
            for (int i = 0; i < 4; i++) {
                ip[i] = file->socket_peer_ip[i];
            }
            port = file->socket_peer_port;
        } else {
            net_local_address(ip);
            port = file->socket_handle >= 0
                       ? net_udp_port(file->socket_handle) : 0;
        }
    } else if (file->socket_state == USER_SOCKET_CONNECTED
               || file->socket_state == USER_SOCKET_CONNECTING) {
        uint8_t local_ip[4];
        uint8_t remote_ip[4];
        uint16_t local_port;
        uint16_t remote_port;
        net_tcp_endpoints(file->socket_handle, local_ip, &local_port,
                          remote_ip, &remote_port);
        for (int i = 0; i < 4; i++) {
            ip[i] = peer ? remote_ip[i] : local_ip[i];
        }
        port = peer ? remote_port : local_port;
    } else if (peer) {
        return -107;
    } else {
        port = file->socket_port;
    }
    return user_write_sockaddr(address, length_address, ip, port);
}

static int64_t user_linux_getsockopt(struct user_process *process,
                                     struct linux_syscall_frame *frame) {
    int64_t error = 0;
    struct user_file_descriptor *file = user_socket_file(process, frame->rdi,
                                                         &error);
    if (file == NULL) {
        return error;
    }
    uintptr_t value = (uintptr_t)frame->r10;
    uintptr_t length = (uintptr_t)frame->r8;
    if (!paging_user_range_valid(length, 4, true, false)
        || *(uint32_t *)length < 4
        || !paging_user_range_valid(value, 4, true, false)) {
        return -14;
    }
    int32_t result = 0;
    if (frame->rsi == LINUX_SOL_SOCKET && frame->rdx == LINUX_SO_ERROR) {
        if (file->socket_type == LINUX_SOCK_STREAM) {
            (void)user_tcp_settle(file);
        }
        result = -file->socket_error;
        file->socket_error = 0;
    } else if (frame->rsi == LINUX_SOL_SOCKET
               && frame->rdx == LINUX_SO_TYPE) {
        result = file->socket_type;
    }
    *(int32_t *)value = result;
    *(uint32_t *)length = 4;
    return 0;
}

/* Ready events of one descriptor, for poll and select. */
static uint32_t user_descriptor_events(struct user_process *process,
                                       uint64_t descriptor) {
    if (descriptor < USER_FIRST_FILE_DESCRIPTOR
        && descriptor < USER_DESCRIPTOR_CAPACITY
        && !process->descriptors[descriptor].used) {
        if (process->descriptors[descriptor].closed) {
            return LINUX_POLLNVAL;
        }
        /* The console: output always fits; input readiness is unknown,
           so stdin only reports when polled with a timeout of 0 too. */
        return descriptor == 0 ? 0 : LINUX_POLLOUT;
    }
    struct user_file_descriptor *file = user_file_descriptor(process,
                                                             descriptor);
    if (file == NULL) {
        return LINUX_POLLNVAL;
    }
    if (file->kind == USER_OPEN_FILE_PIPE_READ
        || file->kind == USER_OPEN_FILE_PIPE_WRITE) {
        struct user_pipe *pipe = user_file_pipe(file);
        if (pipe == NULL) {
            return LINUX_POLLNVAL;
        }
        if (file->kind == USER_OPEN_FILE_PIPE_READ) {
            return (pipe->size != 0 ? LINUX_POLLIN : 0)
                 | (pipe->writers == 0 ? LINUX_POLLHUP : 0);
        }
        return pipe->readers == 0 ? LINUX_POLLERR
             : pipe->size < USER_PIPE_BUFFER_CAPACITY ? LINUX_POLLOUT : 0;
    }
    if (file->kind != USER_OPEN_FILE_SOCKET) {
        return LINUX_POLLIN | LINUX_POLLOUT;
    }
    if (file->socket_type == LINUX_SOCK_DGRAM) {
        return LINUX_POLLOUT
             | (file->socket_handle >= 0
                    && net_udp_readable(file->socket_handle)
                ? LINUX_POLLIN : 0);
    }
    switch (file->socket_state) {
    case USER_SOCKET_LISTENING:
        return net_tcp_accept_ready(file->socket_handle) ? LINUX_POLLIN : 0;
    case USER_SOCKET_CONNECTING: {
        int64_t error = user_tcp_settle(file);
        if (error == -115) {
            return 0;
        }
        if (error != 0) {
            return LINUX_POLLOUT | LINUX_POLLERR | LINUX_POLLHUP;
        }
        break;
    }
    case USER_SOCKET_CONNECTED:
        break;
    default:
        return LINUX_POLLOUT | LINUX_POLLHUP;
    }
    uint32_t events = 0;
    if (net_tcp_readable(file->socket_handle) || file->socket_read_shut) {
        events |= LINUX_POLLIN;
    }
    if (net_tcp_status(file->socket_handle) == NET_TCP_FAILED) {
        events |= LINUX_POLLERR | LINUX_POLLHUP;
    } else if (net_tcp_can_send(file->socket_handle)
               && net_tcp_send_space(file->socket_handle) != 0) {
        events |= LINUX_POLLOUT;
    }
    return events;
}

/* Milliseconds to an absolute tick; UINT64_MAX means forever. */
static uint64_t user_poll_deadline(int64_t milliseconds) {
    if (milliseconds < 0) {
        return UINT64_MAX;
    }
    return timer_ticks() + ((uint64_t)milliseconds + 9) / 10;
}

static int64_t user_poll(struct user_process *process, uintptr_t address,
                         uint64_t count, uint64_t deadline) {
    if (count > USER_POLL_MAX) {
        return -22;
    }
    if (count != 0
        && !paging_user_range_valid(address, count * 8, true, false)) {
        return -14;
    }
    for (;;) {
        net_poll();
        if (count != 0
            && !paging_user_range_valid(address, count * 8, true, false)) {
            return -14;
        }
        int64_t ready = 0;
        for (uint64_t i = 0; i < count; i++) {
            uint8_t *entry = (uint8_t *)address + i * 8;
            int32_t descriptor = *(int32_t *)entry;
            uint16_t wanted = *(uint16_t *)(entry + 4);
            uint16_t events = 0;
            if (descriptor >= 0) {
                events = (uint16_t)user_descriptor_events(process,
                                                          (uint64_t)descriptor);
                events &= wanted | LINUX_POLLERR | LINUX_POLLHUP
                        | LINUX_POLLNVAL;
            }
            *(uint16_t *)(entry + 6) = events;
            ready += events != 0 ? 1 : 0;
        }
        if (ready != 0 || timer_ticks() >= deadline) {
            return ready;
        }
        if (!user_socket_wait()) {
            return -4;
        }
    }
}

/* select/pselect6: fd_set bitmaps of 64-bit words. */
static int64_t user_select(struct user_process *process, uint64_t count,
                           uintptr_t read_set, uintptr_t write_set,
                           uintptr_t except_set, uint64_t deadline) {
    if (count > 1024) {
        return -22;
    }
    size_t bytes = (size_t)((count + 63) / 64) * 8;
    uint64_t wanted_read[16] = {0};
    uint64_t wanted_write[16] = {0};
    uintptr_t sets[3] = {read_set, write_set, except_set};
    for (int s = 0; s < 3; s++) {
        if (sets[s] != 0 && bytes != 0
            && !paging_user_range_valid(sets[s], bytes, true, false)) {
            return -14;
        }
    }
    for (size_t w = 0; w < bytes / 8; w++) {
        wanted_read[w] = read_set != 0 ? ((uint64_t *)read_set)[w] : 0;
        wanted_write[w] = write_set != 0 ? ((uint64_t *)write_set)[w] : 0;
    }
    for (;;) {
        net_poll();
        int64_t ready = 0;
        uint64_t out_read[16] = {0};
        uint64_t out_write[16] = {0};
        for (uint64_t fd = 0; fd < count; fd++) {
            uint64_t bit = 1ull << (fd % 64);
            bool want_read = (wanted_read[fd / 64] & bit) != 0;
            bool want_write = (wanted_write[fd / 64] & bit) != 0;
            if (!want_read && !want_write) {
                continue;
            }
            uint32_t events = user_descriptor_events(process, fd);
            if ((events & LINUX_POLLNVAL) != 0) {
                return -9;
            }
            if (want_read
                && (events & (LINUX_POLLIN | LINUX_POLLHUP | LINUX_POLLERR))) {
                out_read[fd / 64] |= bit;
                ready++;
            }
            if (want_write && (events & (LINUX_POLLOUT | LINUX_POLLERR))) {
                out_write[fd / 64] |= bit;
                ready++;
            }
        }
        if (ready != 0 || timer_ticks() >= deadline) {
            for (size_t w = 0; w < bytes / 8; w++) {
                if (read_set != 0) {
                    ((uint64_t *)read_set)[w] = out_read[w];
                }
                if (write_set != 0) {
                    ((uint64_t *)write_set)[w] = out_write[w];
                }
                if (except_set != 0) {
                    ((uint64_t *)except_set)[w] = 0;
                }
            }
            return ready;
        }
        if (!user_socket_wait()) {
            return -4;
        }
    }
}

/* Reads a struct timespec (or timeval when micro) into a deadline. */
static int64_t user_time_deadline(uintptr_t address, bool micro,
                                  uint64_t *deadline) {
    if (address == 0) {
        *deadline = UINT64_MAX;
        return 0;
    }
    if (!paging_user_range_valid(address, 16, false, false)) {
        return -14;
    }
    int64_t seconds = *(int64_t *)address;
    int64_t fraction = *(int64_t *)(address + 8);
    if (seconds < 0 || fraction < 0
        || fraction >= (micro ? 1000000 : 1000000000)) {
        return -22;
    }
    uint64_t milliseconds = (uint64_t)seconds * 1000
                          + (uint64_t)fraction / (micro ? 1000 : 1000000);
    if (seconds > 100000000) {
        *deadline = UINT64_MAX;
        return 0;
    }
    *deadline = user_poll_deadline((int64_t)milliseconds);
    return 0;
}

static int64_t user_linux_fcntl(struct user_process *process,
                                uint64_t descriptor, uint64_t command,
                                uint64_t argument) {
    if (descriptor >= USER_DESCRIPTOR_CAPACITY) {
        return -9;
    }
    struct user_descriptor *entry = &process->descriptors[descriptor];
    struct user_file_descriptor *file = user_file_descriptor(process,
                                                             descriptor);
    bool console = descriptor < USER_FIRST_FILE_DESCRIPTOR && !entry->used
                && !entry->closed;
    if (file == NULL && !console) {
        return -9;
    }
    switch (command) {
    case 0:    /* F_DUPFD */
    case 1030: /* F_DUPFD_CLOEXEC */
        if (file == NULL) {
            return -22;
        }
        return user_duplicate_descriptor(process, descriptor, argument, false,
                                         command == 1030, false);
    case 1: /* F_GETFD */
        return entry->close_on_exec ? 1 : 0;
    case 2: /* F_SETFD */
        if (file != NULL) {
            entry->close_on_exec = (argument & 1) != 0;
        }
        return 0;
    case 3: { /* F_GETFL */
        if (file == NULL) {
            return descriptor == 0 ? 0 : 1;
        }
        int64_t flags = file->readable && file->writable ? 2
                      : file->writable ? 1 : 0;
        flags |= file->nonblocking ? LINUX_O_NONBLOCK : 0;
        flags |= file->append ? LINUX_O_APPEND : 0;
        return flags;
    }
    case 4: /* F_SETFL */
        if (file != NULL) {
            file->nonblocking = (argument & LINUX_O_NONBLOCK) != 0;
            if (file->kind == USER_OPEN_FILE_VFS) {
                file->append = (argument & LINUX_O_APPEND) != 0;
            }
        }
        return 0;
    default:
        return -22;
    }
}

/* Socket, poll, select and fcntl calls. Returns false when number is not
   one of them. */
static bool user_linux_socket_syscall(struct user_process *process,
                                      struct linux_syscall_frame *frame,
                                      uint64_t number) {
    int64_t result;
    switch (number) {
    case 41:
        result = user_linux_socket(process, frame->rdi, frame->rsi,
                                   frame->rdx);
        break;
    case 42:
        result = user_linux_connect(process, frame->rdi,
                                    (uintptr_t)frame->rsi, frame->rdx);
        break;
    case 43:
        result = user_linux_accept(process, frame->rdi, (uintptr_t)frame->rsi,
                                   (uintptr_t)frame->rdx, 0);
        break;
    case 288:
        result = user_linux_accept(process, frame->rdi, (uintptr_t)frame->rsi,
                                   (uintptr_t)frame->rdx, frame->r10);
        break;
    case 44:
        result = user_linux_sendto(process, frame);
        break;
    case 45:
        result = user_linux_recvfrom(process, frame);
        break;
    case 46:
        result = user_linux_message(process, frame, true);
        break;
    case 47:
        result = user_linux_message(process, frame, false);
        break;
    case 48:
        result = user_linux_shutdown(process, frame->rdi, frame->rsi);
        break;
    case 49:
        result = user_linux_bind(process, frame->rdi, (uintptr_t)frame->rsi,
                                 frame->rdx);
        break;
    case 50:
        result = user_linux_listen(process, frame->rdi, frame->rsi);
        break;
    case 51:
    case 52:
        result = user_linux_socket_name(process, frame->rdi,
                                        (uintptr_t)frame->rsi,
                                        (uintptr_t)frame->rdx, number == 52);
        break;
    case 53: /* socketpair: AF_UNIX only */
        result = -97;
        break;
    case 54: { /* setsockopt: accepted and ignored */
        int64_t error = 0;
        result = user_socket_file(process, frame->rdi, &error) != NULL
                     ? 0 : error;
        break;
    }
    case 55:
        result = user_linux_getsockopt(process, frame);
        break;
    case 7:
        result = user_poll(process, (uintptr_t)frame->rdi, frame->rsi,
                           user_poll_deadline((int32_t)frame->rdx));
        break;
    case 271: {
        uint64_t deadline;
        result = user_time_deadline((uintptr_t)frame->rdx, false, &deadline);
        if (result == 0) {
            result = user_poll(process, (uintptr_t)frame->rdi, frame->rsi,
                               deadline);
        }
        break;
    }
    case 23:
    case 270: {
        uint64_t deadline;
        result = user_time_deadline((uintptr_t)frame->r8, number == 23,
                                    &deadline);
        if (result == 0) {
            result = user_select(process, frame->rdi, (uintptr_t)frame->rsi,
                                 (uintptr_t)frame->rdx,
                                 (uintptr_t)frame->r10, deadline);
        }
        break;
    }
    case 72:
        result = user_linux_fcntl(process, frame->rdi, frame->rsi,
                                  frame->rdx);
        break;
    default:
        return false;
    }
    frame->rax = (uint64_t)result;
    return true;
}

/*
 * Wall-clock time: the real-time clock read once (taken as UTC), then
 * advanced with the monotonic timer.
 */
static bool user_wall_clock_known;
static int64_t user_wall_clock_offset; /* seconds: wall minus monotonic */

static int64_t user_days_from_civil(int64_t year, unsigned month,
                                    unsigned day) {
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    unsigned year_of_era = (unsigned)(year - era * 400);
    unsigned day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5
                         + day - 1;
    unsigned day_of_era = year_of_era * 365 + year_of_era / 4
                        - year_of_era / 100 + day_of_year;
    return era * 146097 + (int64_t)day_of_era - 719468;
}

static void user_wall_time(uint64_t *seconds, uint64_t *nanoseconds) {
    uint64_t monotonic_seconds;
    timer_monotonic_time(&monotonic_seconds, nanoseconds);
    if (!user_wall_clock_known) {
        struct rtc_time now;
        if (rtc_read(&now) && now.month >= 1 && now.month <= 12) {
            int64_t wall = user_days_from_civil(now.year, now.month, now.day)
                * 86400 + now.hour * 3600 + now.minute * 60 + now.second;
            user_wall_clock_offset = wall - (int64_t)monotonic_seconds;
            user_wall_clock_known = true;
        }
    }
    *seconds = (uint64_t)((int64_t)monotonic_seconds + user_wall_clock_offset);
}

static int64_t user_linux_getrandom(uintptr_t buffer, size_t length) {
    if (length > 4096) {
        length = 4096;
    }
    if (length != 0 && !paging_user_range_valid(buffer, length, true, false)) {
        return -14;
    }
    bool hardware_random = user_rdrand_supported();
    for (size_t i = 0; i < length; i += 8) {
        uint64_t word = user_random_word(hardware_random);
        for (size_t j = 0; j < 8 && i + j < length; j++) {
            ((uint8_t *)buffer)[i + j] = (uint8_t)(word >> (j * 8));
        }
    }
    return (int64_t)length;
}

bool user_linux_syscall_dispatch(void *saved_registers) {
    struct linux_syscall_frame *frame = saved_registers;
    struct user_process *process = scheduler_current_context();
    if (process == NULL || frame == NULL) {
        return false;
    }

    uint64_t number = frame->rax;
    if (number == OPOS_SYSCALL_ABI_VERSION) {
        frame->rax = OPOS_ABI_VERSION;
        return false;
    }
    if (number == OPOS_SYSCALL_USER_INFO) {
        frame->rax = (uint64_t)user_opos_user_info(process,
                                                   (uintptr_t)frame->rdi);
        return false;
    }
    if (number >= OPOS_SYSCALL_WINDOW_CREATE
        && number <= OPOS_SYSCALL_WINDOW_SIZE) {
        return user_opos_window(process, frame);
    }
    if (user_linux_socket_syscall(process, frame, number)) {
        return false;
    }
    uint64_t file_descriptor = frame->rdi;
    uintptr_t buffer_address = (uintptr_t)frame->rsi;
    size_t length = (size_t)frame->rdx;

    if (number == 1) {
        if ((file_descriptor == 1 || file_descriptor == 2)
            && !process->descriptors[file_descriptor].used) {
            if (process->descriptors[file_descriptor].closed) {
                frame->rax = (uint64_t)(int64_t)-9;
                return false;
            }
            if (length > USER_WRITE_MAX) {
                frame->rax = (uint64_t)(int64_t)-22;
            } else if (length != 0
                       && !paging_user_range_valid(buffer_address, length,
                                                   false, false)) {
                frame->rax = (uint64_t)(int64_t)-14;
            } else {
                user_console_write(file_descriptor,
                                   (const char *)buffer_address, length);
                frame->rax = length;
            }
            return false;
        }
        struct user_file_descriptor *file =
            user_file_descriptor(process, file_descriptor);
        if (file != NULL && file->kind == USER_OPEN_FILE_SOCKET
            && length > USER_SOCKET_IO_MAX) {
            length = USER_SOCKET_IO_MAX;
        }
        if (file == NULL || !file->writable) {
            frame->rax = (uint64_t)(int64_t)(file != NULL && file->directory
                                             ? -21 : -9);
        } else if (length > USER_FILE_IO_MAX
                   && file->kind != USER_OPEN_FILE_SOCKET) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else if (length != 0
                   && !paging_user_range_valid(buffer_address, length, false, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
        } else if (file->kind == USER_OPEN_FILE_PIPE_WRITE) {
            frame->rax = (uint64_t)user_pipe_write(
                file, (const uint8_t *)buffer_address, length, true);
        } else if (file->kind == USER_OPEN_FILE_SOCKET) {
            frame->rax = (uint64_t)user_socket_send(
                process, file_descriptor, buffer_address, length, 0, NULL, 0,
                false);
        } else {
            if (file->append) {
                struct vfs_metadata metadata;
                if (!vfs_object_metadata(file->object, &metadata)) {
                    frame->rax = (uint64_t)(int64_t)-5;
                    return false;
                }
                file->offset = metadata.size;
            }
            size_t bytes_written = 0;
            if (!vfs_object_write_at(file->object, file->offset,
                                     (const uint8_t *)buffer_address, length,
                                     &bytes_written)) {
                frame->rax = (uint64_t)(int64_t)-5;
            } else {
                file->offset += bytes_written;
                frame->rax = bytes_written;
            }
        }
        return false;
    }

    if (number == 0) {
        if (file_descriptor == 0 && !process->descriptors[0].used) {
            if (process->descriptors[0].closed) {
                frame->rax = (uint64_t)(int64_t)-9;
                return false;
            }
            if (length > USER_READ_MAX) {
                frame->rax = (uint64_t)(int64_t)-22;
                return false;
            }
            if (length != 0
                && !paging_user_range_valid(buffer_address, length, true,
                                            false)) {
                frame->rax = (uint64_t)(int64_t)-14;
                return false;
            }
            char *buffer = (char *)buffer_address;
            size_t bytes_read = 0;
            while (bytes_read < length) {
                char character;
                if (!(console_input != NULL
                          ? console_input(&character)
                          : keyboard_wait_character(&character))) {
                    frame->rax = (uint64_t)(int64_t)-5;
                    return false;
                }
                if (character == '\b') {
                    if (bytes_read != 0) {
                        bytes_read--;
                        user_console_write(1, &character, 1);
                    }
                    continue;
                }
                if (character != '\n' && (character < ' ' || character > '~')) {
                    continue;
                }
                /* Another thread may have unmapped it while we waited. */
                if (!paging_user_range_valid(
                        (uintptr_t)(buffer + bytes_read), 1, true, false)) {
                    frame->rax = (uint64_t)(int64_t)-14;
                    return false;
                }
                buffer[bytes_read++] = character;
                user_console_write(1, &character, 1);
                if (character == '\n') {
                    break;
                }
            }
            frame->rax = bytes_read;
            return false;
        }
        struct user_file_descriptor *file =
            user_file_descriptor(process, file_descriptor);
        if (file != NULL && file->kind == USER_OPEN_FILE_SOCKET
            && length > USER_SOCKET_IO_MAX) {
            length = USER_SOCKET_IO_MAX;
        }
        if (file == NULL || !file->readable || file->directory) {
            frame->rax = (uint64_t)(int64_t)(file != NULL && file->directory
                                             ? -21 : -9);
        } else if (length > USER_FILE_IO_MAX
                   && file->kind != USER_OPEN_FILE_SOCKET) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else if (length != 0
                   && !paging_user_range_valid(buffer_address, length, true, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
        } else if (file->kind == USER_OPEN_FILE_PIPE_READ) {
            frame->rax = (uint64_t)user_pipe_read(
                file, (uint8_t *)buffer_address, length);
        } else if (file->kind == USER_OPEN_FILE_SOCKET) {
            frame->rax = (uint64_t)user_socket_receive(
                process, file_descriptor, buffer_address, length, 0, NULL,
                NULL, false);
        } else {
            size_t bytes_read = 0;
            if (!vfs_object_read_at(file->object, file->offset,
                                    (uint8_t *)buffer_address, length,
                                    &bytes_read)) {
                frame->rax = (uint64_t)(int64_t)-5;
            } else {
                file->offset += bytes_read;
                frame->rax = bytes_read;
            }
        }
        return false;
    }

    if (number == 2) {
        frame->rax = (uint64_t)user_linux_open_file(
            process, LINUX_AT_FDCWD, (uintptr_t)frame->rdi, frame->rsi);
        return false;
    }

    if (number == 22) {
        frame->rax = (uint64_t)user_linux_pipe(
            process, (uintptr_t)frame->rdi, 0);
        return false;
    }

    if (number == 293) {
        frame->rax = (uint64_t)user_linux_pipe(
            process, (uintptr_t)frame->rdi, frame->rsi);
        return false;
    }

    if (number == 59) {
        int64_t executed = user_linux_execve(process, frame);
        if (executed != 0) {
            frame->rax = (uint64_t)executed;
        }
        return false;
    }

    if (number == 57) {
        frame->rax = (uint64_t)user_linux_fork(process, frame);
        return false;
    }

    if (number == 56) {
        frame->rax = (uint64_t)user_linux_clone_thread(process, frame);
        return false;
    }

    if (number == 202) {
        frame->rax = (uint64_t)user_linux_futex(process, frame);
        return false;
    }

    if (number == 13) {
        uintptr_t action_address = (uintptr_t)frame->rsi;
        uintptr_t old_action_address = (uintptr_t)frame->rdx;
        uint64_t signal_number = frame->rdi;
        if (signal_number == 0 || signal_number > LINUX_SIGNAL_COUNT
            || frame->r10 != LINUX_SIGNAL_SET_SIZE) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else if ((action_address != 0
                    && !paging_user_range_valid(action_address,
                        sizeof(struct linux_signal_action), false, false))
                   || (old_action_address != 0
                       && !paging_user_range_valid(old_action_address,
                           sizeof(struct linux_signal_action), true, false))) {
            frame->rax = (uint64_t)(int64_t)-14;
        } else if (action_address != 0
                   && (signal_number == LINUX_SIGKILL
                       || signal_number == LINUX_SIGSTOP)) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else {
            struct linux_signal_action *slot =
                &process->signal_actions[signal_number - 1];
            if (old_action_address != 0) {
                *(struct linux_signal_action *)old_action_address = *slot;
            }
            if (action_address != 0) {
                const struct linux_signal_action *requested =
                    (const struct linux_signal_action *)action_address;
                const uint64_t supported = LINUX_SA_RESTORER
                    | LINUX_SA_RESTART | LINUX_SA_NODEFER
                    | LINUX_SA_RESETHAND;
                if ((requested->flags & ~supported) != 0
                    || (requested->handler > LINUX_SIG_IGN
                        && (!paging_user_range_valid(requested->handler, 1,
                                                     false, true)
                            || (requested->flags & LINUX_SA_RESTORER) == 0
                            || !paging_user_range_valid(requested->restorer, 1,
                                                        false, true)))) {
                    frame->rax = (uint64_t)(int64_t)-22;
                    return false;
                }
                *slot = *requested;
                slot->mask &= ~((1ULL << (LINUX_SIGKILL - 1))
                                | (1ULL << (LINUX_SIGSTOP - 1)));
            }
            frame->rax = 0;
        }
        return false;
    }

    if (number == 15) {
        struct user_thread *thread = user_current_thread(process);
        uintptr_t signal_frame_address = scheduler_linux_syscall_user_stack();
        if (thread == NULL
            || !paging_user_range_valid(signal_frame_address,
                    sizeof(struct user_signal_frame), false, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
            return true;
        }
        const struct user_signal_frame *signal_frame =
            (const struct user_signal_frame *)signal_frame_address;
        if (signal_frame->magic != USER_SIGNAL_FRAME_MAGIC
            || !paging_user_range_valid(
                (uintptr_t)signal_frame->registers.instruction_pointer,
                1, false, true)
            || signal_frame->stack_pointer < USER_APP_MIN_ADDRESS) {
            frame->rax = (uint64_t)(int64_t)-22;
            return true;
        }
        struct user_signal_frame restored = *signal_frame;
        *frame = restored.registers;
        thread->signal_mask = restored.signal_mask;
        scheduler_linux_syscall_set_user_stack(restored.stack_pointer);
        scheduler_restore_current_fpu(restored.fpu_state);
        return false;
    }

    if (number == 200) {
        uint64_t thread_id = frame->rdi;
        uint64_t signal_number = frame->rsi;
        struct user_thread *target = NULL;
        for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY; i++) {
            if (user_threads[i].active && user_threads[i].process == process
                && user_threads[i].id == thread_id) {
                target = &user_threads[i];
                break;
            }
        }
        if (signal_number > LINUX_SIGNAL_COUNT) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else if (target == NULL) {
            frame->rax = (uint64_t)(int64_t)-3;
        } else {
            if (signal_number != 0) {
                __atomic_fetch_or(&target->pending_signals,
                                  1ULL << (signal_number - 1),
                                  __ATOMIC_RELAXED);
            }
            frame->rax = 0;
        }
        return false;
    }

    if (number == 14) {
        struct user_thread *thread = user_current_thread(process);
        uintptr_t set_address = (uintptr_t)frame->rsi;
        uintptr_t old_address = (uintptr_t)frame->rdx;
        if (thread == NULL || frame->r10 != LINUX_SIGNAL_SET_SIZE
            || frame->rdi > LINUX_SIG_SETMASK) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else if ((set_address != 0
                    && !paging_user_range_valid(set_address,
                                                LINUX_SIGNAL_SET_SIZE,
                                                false, false))
                   || (old_address != 0
                       && !paging_user_range_valid(old_address,
                                                   LINUX_SIGNAL_SET_SIZE,
                                                   true, false))) {
            frame->rax = (uint64_t)(int64_t)-14;
        } else {
            uint64_t previous = thread->signal_mask;
            if (old_address != 0) {
                *(uint64_t *)old_address = previous;
            }
            if (set_address != 0) {
                uint64_t requested = *(const uint64_t *)set_address;
                if (frame->rdi == LINUX_SIG_BLOCK) {
                    thread->signal_mask |= requested;
                } else if (frame->rdi == LINUX_SIG_UNBLOCK) {
                    thread->signal_mask &= ~requested;
                } else {
                    thread->signal_mask = requested;
                }
                thread->signal_mask &= ~((1ULL << (9 - 1))
                                         | (1ULL << (19 - 1)));
            }
            frame->rax = 0;
        }
        return false;
    }

    if (number == 218) {
        struct user_thread *thread = user_current_thread(process);
        uintptr_t address = (uintptr_t)frame->rdi;
        if (thread == NULL || (address != 0
            && !paging_user_range_valid(address, sizeof(uint32_t),
                                        true, false))) {
            frame->rax = (uint64_t)(int64_t)-14;
        } else {
            thread->clear_child_tid = address;
            frame->rax = thread->id;
        }
        return false;
    }

    if (number == 273) {
        struct user_thread *thread = user_current_thread(process);
        if (thread == NULL || frame->rsi != 3 * sizeof(uintptr_t)) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else if (frame->rdi != 0
                   && !paging_user_range_valid((uintptr_t)frame->rdi,
                                               (size_t)frame->rsi,
                                               false, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
        } else {
            thread->robust_list = (uintptr_t)frame->rdi;
            frame->rax = 0;
        }
        return false;
    }

    if (number == 274) {
        struct user_thread *thread = user_current_thread(process);
        if (thread == NULL || (frame->rdi != 0 && frame->rdi != thread->id)) {
            frame->rax = (uint64_t)(int64_t)-3;
        } else if (!paging_user_range_valid((uintptr_t)frame->rsi,
                                            sizeof(uintptr_t), true, false)
                   || !paging_user_range_valid((uintptr_t)frame->rdx,
                                               sizeof(size_t), true, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
        } else {
            *(uintptr_t *)(uintptr_t)frame->rsi = thread->robust_list;
            *(size_t *)(uintptr_t)frame->rdx = 3 * sizeof(uintptr_t);
            frame->rax = 0;
        }
        return false;
    }

    if (number == 61) {
        if (frame->rdx != 0 || frame->r10 != 0) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else if (frame->rsi != 0
                   && !paging_user_range_valid((uintptr_t)frame->rsi,
                                               sizeof(uint32_t), true, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
        } else {
            frame->rax = (uint64_t)user_wait_linux_child(
                process, (int64_t)frame->rdi,
                (uint32_t *)(uintptr_t)frame->rsi);
        }
        return false;
    }

    if (number == 257) {
        frame->rax = (uint64_t)user_linux_open_file(
            process, (int64_t)frame->rdi, (uintptr_t)frame->rsi, frame->rdx);
        return false;
    }

    if (number == 3) {
        frame->rax = (uint64_t)user_close_descriptor(process,
                                                     file_descriptor);
        return false;
    }

    if (number == 32) {
        frame->rax = (uint64_t)user_duplicate_descriptor(
            process, frame->rdi, 0, false, false, false);
        return false;
    }

    if (number == 33) {
        frame->rax = (uint64_t)user_duplicate_descriptor(
            process, frame->rdi, frame->rsi, true, false, false);
        return false;
    }

    if (number == 292) {
        if ((frame->rdx & ~((uint64_t)LINUX_O_CLOEXEC)) != 0) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else {
            frame->rax = (uint64_t)user_duplicate_descriptor(
                process, frame->rdi, frame->rsi, true,
                (frame->rdx & LINUX_O_CLOEXEC) != 0, true);
        }
        return false;
    }

    if (number == 4 || number == 6) {
        frame->rax = (uint64_t)user_linux_stat_path(
            process, LINUX_AT_FDCWD, (uintptr_t)frame->rdi,
            (uintptr_t)frame->rsi);
        return false;
    }

    if (number == 5) {
        frame->rax = (uint64_t)user_linux_fstat(
            process, frame->rdi, (uintptr_t)frame->rsi);
        return false;
    }

    if (number == 8) {
        struct user_file_descriptor *file =
            user_file_descriptor(process, file_descriptor);
        if (file == NULL || frame->rdx > 2) {
            frame->rax = (uint64_t)(int64_t)(file == NULL ? -9 : -22);
            return false;
        }
        if (file->kind != USER_OPEN_FILE_VFS) {
            frame->rax = (uint64_t)(int64_t)-29;
            return false;
        }
        if (file->directory && frame->rdx == 2) {
            frame->rax = (uint64_t)(int64_t)-22;
            return false;
        }
        size_t base = 0;
        if (frame->rdx == 1) {
            base = file->offset;
        } else if (frame->rdx == 2) {
            struct vfs_metadata metadata;
            if (!vfs_object_metadata(file->object, &metadata)) {
                frame->rax = (uint64_t)(int64_t)-5;
                return false;
            }
            base = metadata.size;
        }
        int64_t displacement = (int64_t)frame->rsi;
        size_t new_offset;
        if (displacement < 0) {
            uint64_t magnitude = (uint64_t)(-(displacement + 1)) + 1;
            if (magnitude > base) {
                frame->rax = (uint64_t)(int64_t)-22;
                return false;
            }
            new_offset = base - (size_t)magnitude;
        } else {
            if ((uint64_t)displacement > UINT32_MAX - base) {
                frame->rax = (uint64_t)(int64_t)-22;
                return false;
            }
            new_offset = base + (size_t)displacement;
        }
        file->offset = new_offset;
        frame->rax = new_offset;
        return false;
    }

    if (number == 20) {
        user_linux_writev(process, frame);
        return false;
    }

    if (number == 217) {
        frame->rax = (uint64_t)user_linux_getdents64(
            process, frame->rdi, (uintptr_t)frame->rsi, (size_t)frame->rdx);
        return false;
    }

    if (number == 262) {
        if (frame->r10 != 0) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else {
            frame->rax = (uint64_t)user_linux_stat_path(
                process, (int64_t)frame->rdi, (uintptr_t)frame->rsi,
                (uintptr_t)frame->rdx);
        }
        return false;
    }

    if (number == 76) {
        frame->rax = (uint64_t)user_linux_truncate_path(
            process, (uintptr_t)frame->rdi, frame->rsi);
        return false;
    }

    if (number == 77) {
        frame->rax = (uint64_t)user_linux_truncate_descriptor(
            process, frame->rdi, frame->rsi);
        return false;
    }

    if (number == 79) {
        frame->rax = (uint64_t)user_linux_get_current_directory(
            process, (uintptr_t)frame->rdi, (size_t)frame->rsi);
        return false;
    }

    if (number == 80) {
        frame->rax = (uint64_t)user_linux_change_directory(
            process, LINUX_AT_FDCWD, (uintptr_t)frame->rdi);
        return false;
    }

    if (number == 82) {
        frame->rax = (uint64_t)user_linux_rename(
            process, LINUX_AT_FDCWD, (uintptr_t)frame->rdi,
            LINUX_AT_FDCWD, (uintptr_t)frame->rsi);
        return false;
    }

    if (number == 83) {
        frame->rax = (uint64_t)user_linux_make_directory(
            process, LINUX_AT_FDCWD, (uintptr_t)frame->rdi);
        return false;
    }

    if (number == 84) {
        frame->rax = (uint64_t)user_linux_remove(
            process, LINUX_AT_FDCWD, (uintptr_t)frame->rdi, true);
        return false;
    }

    if (number == 87) {
        frame->rax = (uint64_t)user_linux_remove(
            process, LINUX_AT_FDCWD, (uintptr_t)frame->rdi, false);
        return false;
    }

    if (number == 258) {
        frame->rax = (uint64_t)user_linux_make_directory(
            process, (int64_t)frame->rdi, (uintptr_t)frame->rsi);
        return false;
    }

    if (number == 263) {
        if (frame->rdx != 0 && frame->rdx != LINUX_AT_REMOVEDIR) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else {
            frame->rax = (uint64_t)user_linux_remove(
                process, (int64_t)frame->rdi, (uintptr_t)frame->rsi,
                frame->rdx == LINUX_AT_REMOVEDIR);
        }
        return false;
    }

    if (number == 264) {
        frame->rax = (uint64_t)user_linux_rename(
            process, (int64_t)frame->rdi, (uintptr_t)frame->rsi,
            (int64_t)frame->rdx, (uintptr_t)frame->r10);
        return false;
    }

    if (number == 316) {
        if (frame->r8 != 0) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else {
            frame->rax = (uint64_t)user_linux_rename(
                process, (int64_t)frame->rdi, (uintptr_t)frame->rsi,
                (int64_t)frame->rdx, (uintptr_t)frame->r10);
        }
        return false;
    }

    if (number == 39) {
        frame->rax = process->id;
        return false;
    }

    if (number == 186) {
        struct user_thread *thread = user_current_thread(process);
        frame->rax = thread != NULL ? thread->id
                                    : (uint64_t)(int64_t)-3;
        return false;
    }

    if (number == 102 || number == 104 || number == 107 || number == 108) {
        frame->rax = 0;
        return false;
    }

    if (number == 63) {
        if (!paging_user_range_valid((uintptr_t)frame->rdi,
                                     sizeof(struct linux_utsname), true, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
        } else {
            struct linux_utsname *name =
                (struct linux_utsname *)(uintptr_t)frame->rdi;
            copy_utsname_field(name->sysname, "Linux");
            copy_utsname_field(name->nodename, "opos");
            copy_utsname_field(name->release, "0.1.0-opos");
            copy_utsname_field(name->version, "#1 OPOS");
            copy_utsname_field(name->machine, "x86_64");
            copy_utsname_field(name->domainname, "localdomain");
            frame->rax = 0;
        }
        return false;
    }

    if (number == 96) {
        if ((frame->rdi != 0
             && !paging_user_range_valid((uintptr_t)frame->rdi,
                                         sizeof(struct linux_timeval), true, false))
            || (frame->rsi != 0
                && !paging_user_range_valid((uintptr_t)frame->rsi,
                                            sizeof(struct linux_timezone),
                                            true, false))) {
            frame->rax = (uint64_t)(int64_t)-14;
        } else {
            if (frame->rdi != 0) {
                struct linux_timeval *time =
                    (struct linux_timeval *)(uintptr_t)frame->rdi;
                uint64_t seconds;
                uint64_t nanoseconds;
                user_wall_time(&seconds, &nanoseconds);
                time->seconds = (int64_t)seconds;
                time->microseconds = (int64_t)(nanoseconds / 1000);
            }
            if (frame->rsi != 0) {
                struct linux_timezone *timezone =
                    (struct linux_timezone *)(uintptr_t)frame->rsi;
                timezone->minutes_west = 0;
                timezone->dst_time = 0;
            }
            frame->rax = 0;
        }
        return false;
    }

    if (number == 35) {
        if (!paging_user_range_valid((uintptr_t)frame->rdi,
                                     sizeof(struct linux_timespec), false, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
            return false;
        }

        const struct linux_timespec *request =
            (const struct linux_timespec *)(uintptr_t)frame->rdi;
        if (request->seconds < 0 || request->nanoseconds < 0
            || request->nanoseconds >= 1000000000) {
            frame->rax = (uint64_t)(int64_t)-22;
            return false;
        }

        if (!timer_sleep_duration((uint64_t)request->seconds,
                                  (uint64_t)request->nanoseconds)) {
            frame->rax = (uint64_t)(int64_t)-22;
            return false;
        }
        frame->rax = 0;
        return false;
    }

    if (number == 318) {
        frame->rax = (uint64_t)user_linux_getrandom((uintptr_t)frame->rdi,
                                                    (size_t)frame->rsi);
        return false;
    }

    if (number == 228) {
        if (frame->rdi != LINUX_CLOCK_MONOTONIC
            && frame->rdi != LINUX_CLOCK_REALTIME) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else if (!paging_user_range_valid((uintptr_t)frame->rsi,
                                            sizeof(struct linux_timespec),
                                            true, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
        } else {
            struct linux_timespec *time =
                (struct linux_timespec *)(uintptr_t)frame->rsi;
            uint64_t seconds;
            uint64_t nanoseconds;
            if (frame->rdi == LINUX_CLOCK_REALTIME) {
                user_wall_time(&seconds, &nanoseconds);
            } else {
                timer_monotonic_time(&seconds, &nanoseconds);
            }
            time->seconds = (int64_t)seconds;
            time->nanoseconds = (int64_t)nanoseconds;
            frame->rax = 0;
        }
        return false;
    }

    if (number == 229) {
        if (frame->rdi != LINUX_CLOCK_MONOTONIC
            && frame->rdi != LINUX_CLOCK_REALTIME) {
            frame->rax = (uint64_t)(int64_t)-22;
        } else if (frame->rsi != 0
                   && !paging_user_range_valid((uintptr_t)frame->rsi,
                                               sizeof(struct linux_timespec),
                                               true, false)) {
            frame->rax = (uint64_t)(int64_t)-14;
        } else {
            if (frame->rsi != 0) {
                struct linux_timespec *resolution =
                    (struct linux_timespec *)(uintptr_t)frame->rsi;
                resolution->seconds = 0;
                resolution->nanoseconds =
                    (int64_t)timer_monotonic_resolution_nanoseconds();
            }
            frame->rax = 0;
        }
        return false;
    }

    if (number == 158) {
        if (frame->rdi == LINUX_ARCH_SET_FS) {
            if (frame->rsi != 0
                && !paging_user_range_valid((uintptr_t)frame->rsi, 1, false, false)) {
                frame->rax = (uint64_t)(int64_t)-14;
                return false;
            }
            frame->fs_base = frame->rsi;
            frame->rax = 0;
        } else if (frame->rdi == LINUX_ARCH_GET_FS) {
            if (!paging_user_range_valid((uintptr_t)frame->rsi,
                                         sizeof(frame->fs_base), true, false)) {
                frame->rax = (uint64_t)(int64_t)-14;
                return false;
            }
            *(uint64_t *)(uintptr_t)frame->rsi = frame->fs_base;
            frame->rax = 0;
        } else {
            frame->rax = (uint64_t)(int64_t)-22;
        }
        return false;
    }

    if (number == 12) {
        if (!process->elf_image) {
            frame->rax = (uint64_t)(int64_t)-38;
            return false;
        }

        uintptr_t requested_break = (uintptr_t)frame->rdi;
        uintptr_t maximum_break = process->heap_base
            + USER_HEAP_MAX_PAGES * PAGE_SIZE;
        if (maximum_break > USER_MMAP_BASE) {
            maximum_break = USER_MMAP_BASE;
        }
        if (requested_break == 0) {
            frame->rax = process->current_break;
            return false;
        }
        if (requested_break < process->initial_break
            || requested_break > maximum_break) {
            frame->rax = process->current_break;
            return false;
        }

        size_t requested_pages = requested_break <= process->heap_base ? 0
            : (requested_break - process->heap_base - 1) / PAGE_SIZE + 1;
        if (requested_pages > process->heap_page_count) {
            size_t original_page_count = process->heap_page_count;
            while (process->heap_page_count < requested_pages) {
                size_t page_index = process->heap_page_count;
                uint64_t physical_page = physical_page_alloc();
                if (physical_page == 0) {
                    break;
                }

                uint8_t *page = physical_page_to_hhdm(physical_page);
                if (page == NULL) {
                    physical_page_free(physical_page);
                    break;
                }
                for (size_t i = 0; i < PAGE_SIZE; i++) {
                    page[i] = 0;
                }

                uintptr_t address = process->heap_base + page_index * PAGE_SIZE;
                if (!paging_map_page(address, physical_page,
                                     PAGING_USER | PAGING_WRITABLE
                                         | PAGING_NO_EXECUTE)) {
                    physical_page_free(physical_page);
                    break;
                }
                process->heap_pages[page_index] = physical_page;
                process->heap_page_count++;
            }

            if (process->heap_page_count != requested_pages) {
                while (process->heap_page_count > original_page_count) {
                    size_t page_index = process->heap_page_count - 1;
                    uintptr_t address = process->heap_base + page_index * PAGE_SIZE;
                    if (!paging_unmap_page(address, NULL)
                        || !physical_page_free(process->heap_pages[page_index])) {
                        frame->rax = process->current_break;
                        return false;
                    }
                    process->heap_pages[page_index] = 0;
                    process->heap_page_count--;
                }
                frame->rax = process->current_break;
                return false;
            }
        } else {
            while (process->heap_page_count > requested_pages) {
                size_t page_index = process->heap_page_count - 1;
                uintptr_t address = process->heap_base + page_index * PAGE_SIZE;
                if (!paging_unmap_page(address, NULL)
                    || !physical_page_free(process->heap_pages[page_index])) {
                    frame->rax = process->current_break;
                    return false;
                }
                process->heap_pages[page_index] = 0;
                process->heap_page_count--;
            }
        }

        process->current_break = requested_break;
        frame->rax = process->current_break;
        return false;
    }

    if (number == 9) {
        if (!process->elf_image) {
            frame->rax = (uint64_t)(int64_t)-38;
            return false;
        }
        bool anonymous = (frame->r10 & USER_MMAP_ANONYMOUS) != 0;
        if ((frame->rdx & ~(uint64_t)(USER_PROT_READ | USER_PROT_WRITE
                                     | USER_PROT_EXEC)) != 0
            || (frame->rdx & (USER_PROT_WRITE | USER_PROT_EXEC))
                == (USER_PROT_WRITE | USER_PROT_EXEC)
            || (frame->r10 & ~(uint64_t)(USER_MMAP_PRIVATE
                                         | USER_MMAP_ANONYMOUS)) != 0
            || (frame->r10 & USER_MMAP_PRIVATE) == 0
            || (anonymous && ((frame->rdx & USER_PROT_EXEC) != 0
                              || frame->r8 != UINT64_MAX || frame->r9 != 0))
            || (!anonymous && (frame->r9 & (PAGE_SIZE - 1)) != 0)) {
            frame->rax = (uint64_t)(int64_t)-22;
            return false;
        }
        if (frame->rsi == 0
            || frame->rsi > USER_MMAP_MAX_PAGES * PAGE_SIZE) {
            frame->rax = (uint64_t)(int64_t)-12;
            return false;
        }

        struct user_file_descriptor *mapped_file = NULL;
        struct vfs_metadata mapped_metadata = {0};
        if (!anonymous) {
            mapped_file = user_file_descriptor(process, frame->r8);
            if (mapped_file == NULL) {
                frame->rax = (uint64_t)(int64_t)-9;
                return false;
            }
            if (mapped_file->kind != USER_OPEN_FILE_VFS
                || mapped_file->directory) {
                frame->rax = (uint64_t)(int64_t)-19;
                return false;
            }
            if (!mapped_file->readable) {
                frame->rax = (uint64_t)(int64_t)-13;
                return false;
            }
            if (frame->r9 > SIZE_MAX
                || !vfs_object_metadata(mapped_file->object,
                                        &mapped_metadata)) {
                frame->rax = (uint64_t)(int64_t)-5;
                return false;
            }
        }

        size_t page_count = (size_t)(frame->rsi / PAGE_SIZE
            + (frame->rsi % PAGE_SIZE != 0));
        size_t first_page = USER_MMAP_MAX_PAGES;
        size_t consecutive_pages = 0;
        for (size_t i = 0; i < USER_MMAP_MAX_PAGES; i++) {
            if (process->mmap_pages[i] == 0) {
                if (consecutive_pages == 0) {
                    first_page = i;
                }
                consecutive_pages++;
                if (consecutive_pages == page_count) {
                    break;
                }
            } else {
                consecutive_pages = 0;
                first_page = USER_MMAP_MAX_PAGES;
            }
        }
        if (consecutive_pages != page_count) {
            frame->rax = (uint64_t)(int64_t)-12;
            return false;
        }

        uint64_t mapping_flags = USER_MMAP_RESERVED;
        if ((frame->rdx & USER_PROT_WRITE) != 0) {
            mapping_flags = PAGING_USER;
            mapping_flags |= PAGING_WRITABLE | PAGING_NO_EXECUTE;
        } else if ((frame->rdx & USER_PROT_EXEC) != 0) {
            mapping_flags = PAGING_USER;
        } else if ((frame->rdx & USER_PROT_READ) != 0) {
            mapping_flags = PAGING_USER | PAGING_NO_EXECUTE;
        }
        size_t mapped_pages = 0;
        for (; mapped_pages < page_count; mapped_pages++) {
            uint64_t physical_page = physical_page_alloc();
            if (physical_page == 0) {
                break;
            }
            uint8_t *page = physical_page_to_hhdm(physical_page);
            if (page == NULL) {
                physical_page_free(physical_page);
                break;
            }
            for (size_t i = 0; i < PAGE_SIZE; i++) {
                page[i] = 0;
            }

            if (mapped_file != NULL) {
                size_t mapping_offset = mapped_pages * PAGE_SIZE;
                size_t file_offset = (size_t)frame->r9 + mapping_offset;
                size_t bytes_to_read = 0;
                if (file_offset >= (size_t)frame->r9
                    && file_offset < mapped_metadata.size
                    && mapping_offset < frame->rsi) {
                    bytes_to_read = mapped_metadata.size - file_offset;
                    if (bytes_to_read > PAGE_SIZE) {
                        bytes_to_read = PAGE_SIZE;
                    }
                    size_t mapping_remaining = (size_t)frame->rsi
                        - mapping_offset;
                    if (bytes_to_read > mapping_remaining) {
                        bytes_to_read = mapping_remaining;
                    }
                }
                size_t bytes_read = 0;
                if (bytes_to_read != 0
                    && (!vfs_object_read_at(mapped_file->object, file_offset,
                                            page, bytes_to_read, &bytes_read)
                        || bytes_read != bytes_to_read)) {
                    physical_page_free(physical_page);
                    break;
                }
            }

            uintptr_t virtual_address =
                USER_MMAP_BASE + (first_page + mapped_pages) * PAGE_SIZE;
            if (mapping_flags != USER_MMAP_RESERVED
                && !paging_map_page(virtual_address, physical_page,
                                    mapping_flags)) {
                physical_page_free(physical_page);
                break;
            }
            process->mmap_pages[first_page + mapped_pages] = physical_page;
            process->mmap_flags[first_page + mapped_pages] = mapping_flags;
        }
        if (mapped_pages != page_count) {
            bool rolled_back = release_mmap_pages(process, first_page,
                                                  mapped_pages);
            frame->rax = rolled_back ? (uint64_t)(int64_t)-12
                                     : (uint64_t)(int64_t)-5;
            return false;
        }

        frame->rax = USER_MMAP_BASE + first_page * PAGE_SIZE;
        return false;
    }

    if (number == 11) {
        if (!process->elf_image) {
            frame->rax = (uint64_t)(int64_t)-38;
            return false;
        }
        if (frame->rdi < USER_MMAP_BASE
            || frame->rdi >= USER_MMAP_BASE + USER_MMAP_MAX_PAGES * PAGE_SIZE
            || (frame->rdi & (PAGE_SIZE - 1)) != 0 || frame->rsi == 0
            || frame->rsi > USER_MMAP_MAX_PAGES * PAGE_SIZE) {
            frame->rax = (uint64_t)(int64_t)-22;
            return false;
        }

        size_t first_page = (frame->rdi - USER_MMAP_BASE) / PAGE_SIZE;
        size_t page_count = (size_t)(frame->rsi / PAGE_SIZE
            + (frame->rsi % PAGE_SIZE != 0));
        if (page_count > USER_MMAP_MAX_PAGES - first_page) {
            frame->rax = (uint64_t)(int64_t)-22;
            return false;
        }
        for (size_t i = first_page; i < first_page + page_count; i++) {
            if (process->mmap_pages[i] == 0) {
                frame->rax = (uint64_t)(int64_t)-22;
                return false;
            }
        }
        frame->rax = release_mmap_pages(process, first_page, page_count)
            ? 0 : (uint64_t)(int64_t)-5;
        return false;
    }

    if (number == 10) {
        if (!process->elf_image) {
            frame->rax = (uint64_t)(int64_t)-38;
            return false;
        }
        if ((frame->rdi & (PAGE_SIZE - 1)) != 0 || frame->rsi == 0
            || frame->rsi > USER_MMAP_MAX_PAGES * PAGE_SIZE
            || (frame->rdx & ~(uint64_t)(USER_PROT_READ | USER_PROT_WRITE
                                         | USER_PROT_EXEC)) != 0
            || (frame->rdx & (USER_PROT_WRITE | USER_PROT_EXEC))
                == (USER_PROT_WRITE | USER_PROT_EXEC)
            ) {
            frame->rax = (uint64_t)(int64_t)-22;
            return false;
        }
        if (frame->rdi < USER_MMAP_BASE
            || frame->rdi >= USER_MMAP_BASE + USER_MMAP_MAX_PAGES * PAGE_SIZE
            || frame->rsi > UINTPTR_MAX - frame->rdi) {
            frame->rax = (uint64_t)(int64_t)-12;
            return false;
        }

        size_t page_count = (size_t)(frame->rsi / PAGE_SIZE
            + (frame->rsi % PAGE_SIZE != 0));
        size_t first_page = (frame->rdi - USER_MMAP_BASE) / PAGE_SIZE;
        if (page_count > USER_MMAP_MAX_PAGES - first_page
            || frame->rdi + frame->rsi
                > USER_MMAP_BASE + USER_MMAP_MAX_PAGES * PAGE_SIZE) {
            frame->rax = (uint64_t)(int64_t)-12;
            return false;
        }
        for (size_t i = first_page; i < first_page + page_count; i++) {
            if (process->mmap_pages[i] == 0) {
                frame->rax = (uint64_t)(int64_t)-12;
                return false;
            }
        }

        uint64_t flags = USER_MMAP_RESERVED;
        if ((frame->rdx & USER_PROT_WRITE) != 0) {
            flags = PAGING_USER;
            flags |= PAGING_WRITABLE | PAGING_NO_EXECUTE;
        } else if ((frame->rdx & USER_PROT_EXEC) != 0) {
            flags = PAGING_USER;
        } else if ((frame->rdx & USER_PROT_READ) != 0) {
            flags = PAGING_USER | PAGING_NO_EXECUTE;
        }
        for (size_t i = first_page; i < first_page + page_count; i++) {
            uintptr_t address = USER_MMAP_BASE + i * PAGE_SIZE;
            bool changed;
            if (flags == USER_MMAP_RESERVED) {
                changed = process->mmap_flags[i] == USER_MMAP_RESERVED
                    || paging_unmap_page(address, NULL);
            } else if (process->mmap_flags[i] == USER_MMAP_RESERVED) {
                changed = paging_map_page(address, process->mmap_pages[i], flags);
            } else {
                changed = paging_protect_range(address, 1, flags);
            }
            if (!changed) {
                frame->rax = (uint64_t)(int64_t)-12;
                return false;
            }
            process->mmap_flags[i] = flags;
        }
        frame->rax = 0;
        return false;
    }

    if (number == 60 || number == 231) {
        if (number == 231 && process->active_threads > 1) {
            /* The other threads end too, with the same status. */
            process->group_status = file_descriptor;
            process->group_exit = true;
            user_process_interrupt_threads(process);
        }
        frame->rax = file_descriptor;
        return true;
    }

    frame->rax = (uint64_t)(int64_t)-38;
    return false;
}

/*
 * Last check before SYSRET. rt_sigreturn, signal delivery, execve and
 * arch_prctl can all put values from user memory into the saved frame:
 * - RFLAGS: SYSRET keeps IOPL, so IOPL 3 would give ring 3 port I/O
 *   (direct disk access). Only arithmetic flags, DF, AC and ID may pass.
 * - RIP: a non-canonical RIP makes Intel CPUs fault in ring 0 after the
 *   user stack is loaded. Linux stops one page below the user top too.
 * - FS base: WRMSR with a non-canonical value faults in ring 0.
 * A bad value ends the process (SIGSEGV) instead of crashing the kernel.
 */
#define USER_RFLAGS_ALLOWED 0x00240cd5ULL
#define USER_RFLAGS_FORCED 0x00000202ULL

bool user_linux_return_check(void *saved_registers, bool exiting) {
    struct linux_syscall_frame *frame = saved_registers;
    if (exiting || frame == NULL) {
        return exiting;
    }
    frame->flags = (frame->flags & USER_RFLAGS_ALLOWED) | USER_RFLAGS_FORCED;
    if (frame->instruction_pointer >= USER_ADDRESS_LIMIT
        || frame->fs_base >= USER_ADDRESS_LIMIT
        || scheduler_linux_syscall_user_stack() >= USER_ADDRESS_LIMIT) {
        frame->rax = 128 + 11;
        return true;
    }
    return false;
}

static bool user_signal_default_ignored(uint64_t signal_number) {
    return signal_number == LINUX_SIGCHLD || signal_number == LINUX_SIGCONT
        || signal_number == LINUX_SIGURG || signal_number == LINUX_SIGWINCH;
}

bool user_linux_signal_deliver(void *saved_registers) {
    struct linux_syscall_frame *frame = saved_registers;
    struct user_process *process = scheduler_current_context();
    struct user_thread *thread = user_current_thread(process);
    if (frame == NULL || process == NULL || thread == NULL) {
        return false;
    }
    if (process->kill_requested) {
        frame->rax = 128 + LINUX_SIGKILL;
        return true;
    }
    if (process->group_exit) {
        frame->rax = process->group_status;
        return true;
    }

    for (;;) {
        uint64_t deliverable = __atomic_load_n(&thread->pending_signals,
                                               __ATOMIC_RELAXED)
            & ~thread->signal_mask;
        if (deliverable == 0) {
            return false;
        }
        uint64_t signal_index = (uint64_t)__builtin_ctzll(deliverable);
        uint64_t signal_bit = 1ULL << signal_index;
        __atomic_fetch_and(&thread->pending_signals, ~signal_bit,
                           __ATOMIC_RELAXED);
        struct linux_signal_action action =
            process->signal_actions[signal_index];
        uint64_t signal_number = signal_index + 1;
        if (action.handler == LINUX_SIG_IGN
            || (action.handler == 0
                && user_signal_default_ignored(signal_number))) {
            continue;
        }
        if (action.handler == 0) {
            frame->rax = 128 + signal_number;
            return true;
        }

        uintptr_t previous_stack = scheduler_linux_syscall_user_stack();
        if (previous_stack < LINUX_SIGNAL_RED_ZONE_SIZE
                + sizeof(struct user_signal_frame) + sizeof(uint64_t)) {
            frame->rax = 128 + signal_number;
            return true;
        }
        uintptr_t signal_frame_address =
            (previous_stack - LINUX_SIGNAL_RED_ZONE_SIZE
             - sizeof(struct user_signal_frame))
            & ~(uintptr_t)15;
        uintptr_t handler_stack = signal_frame_address - sizeof(uint64_t);
        if (!paging_user_range_valid(handler_stack,
                sizeof(struct user_signal_frame) + sizeof(uint64_t),
                true, false)) {
            frame->rax = 128 + signal_number;
            return true;
        }

        struct user_signal_frame *signal_frame =
            (struct user_signal_frame *)signal_frame_address;
        *signal_frame = (struct user_signal_frame) {
            .magic = USER_SIGNAL_FRAME_MAGIC,
            .registers = *frame,
            .stack_pointer = previous_stack,
            .signal_mask = thread->signal_mask,
        };
        scheduler_save_current_fpu(signal_frame->fpu_state);
        *(uintptr_t *)handler_stack = action.restorer;
        thread->signal_mask |= action.mask;
        if ((action.flags & LINUX_SA_NODEFER) == 0) {
            thread->signal_mask |= signal_bit;
        }
        if ((action.flags & LINUX_SA_RESETHAND) != 0) {
            process->signal_actions[signal_index] =
                (struct linux_signal_action) {0};
        }
        frame->rdi = signal_number;
        frame->rsi = 0;
        frame->rdx = 0;
        frame->rax = 0;
        frame->instruction_pointer = action.handler;
        scheduler_linux_syscall_set_user_stack(handler_stack);
        return false;
    }
}

static bool user_finish_process(struct user_process *process, uint64_t status,
                                uint64_t kernel_address_space,
                                uint64_t *process_id, uint64_t *exit_status) {
    if (process == NULL || process_id == NULL || exit_status == NULL) {
        return false;
    }
    while (process->active_threads > 1) {
        uint64_t flags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
        /* A kill interrupts this thread too; here it must still wait for
           the others to end. */
        scheduler_clear_interrupt();
        if (process->active_threads > 1) {
            (void)scheduler_block_current_locked(process);
        }
        if ((flags & (1u << 9)) != 0) {
            __asm__ volatile ("sti" : : : "memory");
        }
    }
    size_t thread_slot = scheduler_current_slot();
    if (thread_slot < SCHEDULER_TASK_CAPACITY) {
        user_threads[thread_slot] = (struct user_thread) {0};
    }
    process->active_threads = 0;
    user_close_all_files(process);
    scheduler_set_current_context(NULL);
    process->state = USER_PROCESS_EXITED;
    process->kill_requested = false;
    process->group_exit = false;
    scheduler_clear_interrupt();
    desktop_app_process_exited(process->id);
    user_orphan_children(process->id);
    if (process->faulted) {
        status = process->fault_status;
    }
    bool image_freed = true;
    for (size_t i = 0; i < process->image_segment_count; i++) {
        const struct user_image_segment *segment = &process->image_segments[i];
        if (!release_user_pages(segment->base,
                                process->image_pages + segment->page_index,
                                segment->page_count, segment->page_count)) {
            image_freed = false;
        }
    }
    if (image_freed) {
        process->image_segment_count = 0;
        process->image_page_count = 0;
    }
    bool stack_freed = release_user_pages(USER_STACK_BASE,
                                           process->stack_pages,
                                           USER_STACK_PAGES, USER_STACK_PAGES);
    bool heap_freed = release_user_pages(process->heap_base, process->heap_pages,
                                          process->heap_page_count,
                                          process->heap_page_count);
    if (heap_freed) {
        process->heap_page_count = 0;
    }
    bool mmap_freed = release_mmap_pages(process, 0, USER_MMAP_MAX_PAGES);
    bool address_space_freed = restore_kernel_address_space(
        kernel_address_space, process->address_space);
    if (address_space_freed) {
        process->address_space = 0;
    }
    *process_id = process->id;
    *exit_status = status;
    return image_freed && stack_freed && heap_freed && mmap_freed
        && address_space_freed;
}

/*
 * argv for a program started from the desktop: argv[0] is its path, then
 * the typed arguments split at spaces; "double quotes" keep spaces.
 */
static bool user_build_start_vectors(struct user_exec_vectors *vectors,
                                     const char *exec_path,
                                     size_t exec_path_length,
                                     const char *arguments,
                                     size_t arguments_length) {
    *vectors = (struct user_exec_vectors) {0};
    if (exec_path == NULL || exec_path_length + 1 > USER_EXEC_STRING_MAX) {
        return false;
    }
    vectors->argument_offsets[vectors->argument_count++] = 0;
    for (size_t i = 0; i < exec_path_length; i++) {
        vectors->strings[vectors->string_size++] = exec_path[i];
    }
    vectors->strings[vectors->string_size++] = '\0';
    size_t i = 0;
    while (arguments != NULL && i < arguments_length) {
        if (arguments[i] == ' ') {
            i++;
            continue;
        }
        if (vectors->argument_count == USER_EXEC_VECTOR_MAX) {
            return false;
        }
        vectors->argument_offsets[vectors->argument_count++] =
            (uint16_t)vectors->string_size;
        bool quoted = false;
        for (; i < arguments_length; i++) {
            char c = arguments[i];
            if (c == '"') {
                quoted = !quoted;
                continue;
            }
            if (c == ' ' && !quoted) {
                break;
            }
            if (c == '\0') {
                continue;
            }
            if (vectors->string_size + 2 > USER_EXEC_STRING_MAX) {
                return false;
            }
            vectors->strings[vectors->string_size++] = c;
        }
        vectors->strings[vectors->string_size++] = '\0';
    }
    return true;
}

static bool user_process_run_image(enum user_program program,
                                   const uint8_t *image_data,
                                   size_t elf_image_size,
                                   const char *initial_cwd,
                                   size_t initial_cwd_length,
                                   const char *initial_execfn,
                                   size_t initial_execfn_length,
                                   const struct permission_identity *identity,
                                   const char *arguments,
                                   size_t arguments_length,
                                   uint64_t *process_id,
                                   uint64_t *exit_status) {
    size_t task_slot = scheduler_current_slot();
    if (task_slot >= SCHEDULER_TASK_CAPACITY || process_id == NULL
        || exit_status == NULL || scheduler_current_context() != NULL
        || user_threads[task_slot].active
        || next_process_id == 0
        || !process_resource_state_clean(&process_slots[task_slot])
        || ((image_data == NULL) != (elf_image_size == 0))
        || (initial_cwd == NULL && initial_cwd_length != 0)
        || initial_cwd_length >= USER_FILE_PATH_CAPACITY
        || ((initial_execfn == NULL) != (initial_execfn_length == 0))
        || initial_execfn_length >= USER_FILE_PATH_CAPACITY
        || (program != USER_PROGRAM_DEMO && program != USER_PROGRAM_FAULT_TEST
            && program != USER_PROGRAM_NX_TEST && program != USER_PROGRAM_INPUT_TEST
            && program != USER_PROGRAM_SHELL && program != USER_PROGRAM_MODULE_ELF
            && program != USER_PROGRAM_LINUX_READ_TEST
            && program != USER_PROGRAM_FILE_TEST
            && program != USER_PROGRAM_PREEMPT_TEST
            && program != USER_PROGRAM_SPAWN_TEST)) {
        return false;
    }

    process_slots[task_slot] = (struct user_process) {0};
    struct user_process *process = &process_slots[task_slot];
    /* Started by the person at the keyboard, or inherited from the
       parent program. */
    process->identity = identity != NULL ? *identity
                                         : permission_session_identity();
    user_set_process_name(process, initial_execfn, initial_execfn_length);
    static const char default_cwd[] = "/";
    const char *cwd = initial_cwd != NULL ? initial_cwd : default_cwd;
    size_t cwd_length = initial_cwd != NULL
        ? initial_cwd_length : sizeof(default_cwd) - 1;
    process->cwd_length = cwd_length;
    for (size_t i = 0; i < cwd_length; i++) {
        process->cwd[i] = cwd[i];
    }

    const uint8_t *program_start;
    const uint8_t *program_end;
    bool is_elf_image = program == USER_PROGRAM_MODULE_ELF;
    uintptr_t entry_point = USER_APP_MIN_ADDRESS;
    size_t program_size;
    struct elf_load_plan elf_plan = {0};
    struct elf_load_plan interpreter_plan = {0};
    size_t interpreter_size = 0;
    if (is_elf_image) {
        if (image_data == NULL) {
            image_data = user_elf_image;
            elf_image_size = user_elf_image_size;
        }
        if (image_data == NULL || elf_image_size == 0) {
            return false;
        }
        program_start = image_data;
        program_size = elf_image_size;
        if (!elf_inspect_user_image(program_start, program_size, PAGE_SIZE,
                                    USER_PIE_BASE, USER_APP_MIN_ADDRESS,
                                    USER_MMAP_BASE, &elf_plan)) {
            return false;
        }
        entry_point = elf_plan.entry_point;
        if (elf_plan.has_interpreter) {
            /* Dynamically linked images start in their PT_INTERP program,
               exactly as they do after execve. */
            if (!vfs_read_file(elf_plan.interpreter_path,
                               elf_plan.interpreter_path_length,
                               user_interpreter_images[task_slot],
                               USER_INTERPRETER_IMAGE_MAX, &interpreter_size)
                || !elf_inspect_user_interpreter(
                    user_interpreter_images[task_slot], interpreter_size,
                    PAGE_SIZE, USER_MMAP_BASE, USER_MMAP_BASE,
                    USER_MMAP_BASE + USER_MMAP_MAX_PAGES * PAGE_SIZE,
                    &interpreter_plan)) {
                return false;
            }
            entry_point = interpreter_plan.entry_point;
        }
    } else {
        switch (program) {
        case USER_PROGRAM_DEMO:
            program_start = user_program_start;
            program_end = user_program_end;
            break;
        case USER_PROGRAM_FAULT_TEST:
            program_start = user_fault_program_start;
            program_end = user_fault_program_end;
            break;
        case USER_PROGRAM_NX_TEST:
            program_start = user_nx_program_start;
            program_end = user_nx_program_end;
            break;
        case USER_PROGRAM_INPUT_TEST:
            program_start = user_input_program_start;
            program_end = user_input_program_end;
            break;
        case USER_PROGRAM_SHELL:
            program_start = user_shell_program_start;
            program_end = user_shell_program_end;
            break;
        case USER_PROGRAM_LINUX_READ_TEST:
            program_start = user_linux_read_program_start;
            program_end = user_linux_read_program_end;
            break;
        case USER_PROGRAM_FILE_TEST:
            program_start = user_file_program_start;
            program_end = user_file_program_end;
            break;
        case USER_PROGRAM_PREEMPT_TEST:
            program_start = user_preempt_program_start;
            program_end = user_preempt_program_end;
            break;
        case USER_PROGRAM_SPAWN_TEST:
            program_start = user_spawn_program_start;
            program_end = user_spawn_program_end;
            break;
        default:
            return false;
        }
        program_size = (size_t)(program_end - program_start);
        if (program_size == 0 || program_size > PAGE_SIZE) {
            return false;
        }
        elf_plan.segment_count = 1;
        elf_plan.total_page_count = 1;
        elf_plan.executable_segment = 0;
        elf_plan.segments[0] = (struct elf_load_segment) {
            .address = USER_APP_MIN_ADDRESS,
            .mapping_base = USER_APP_MIN_ADDRESS,
            .file_size = program_size,
            .memory_size = program_size,
            .page_count = 1,
            .flags = ELF_SEGMENT_READ | ELF_SEGMENT_EXECUTE,
        };
    }

    /* Page lists for up to ELF_MAX_SEGMENT_PAGES pages are too big for
       the kernel stack; each task has its own. */
    struct user_load_pages *load = &user_load_scratch[task_slot];
    *load = (struct user_load_pages) {0};
    uint64_t *image_physical = load->image_physical;
    uint8_t **image_buffers = load->image_buffers;
    uint64_t *interpreter_physical = load->interpreter_physical;
    uint8_t **interpreter_buffers = load->interpreter_buffers;
    uint64_t stack_physical[USER_STACK_PAGES] = {0};
    uint8_t *stack_buffers[USER_STACK_PAGES] = {0};
    if (!allocate_user_pages(elf_plan.total_page_count,
                             image_physical, image_buffers)
        || !allocate_user_pages(interpreter_plan.total_page_count,
                                interpreter_physical, interpreter_buffers)
        || !allocate_user_pages(USER_STACK_PAGES, stack_physical,
                                stack_buffers)) {
        release_user_pages(0, image_physical,
                           elf_plan.total_page_count, 0);
        release_user_pages(0, interpreter_physical,
                           interpreter_plan.total_page_count, 0);
        release_user_pages(0, stack_physical, USER_STACK_PAGES, 0);
        return false;
    }

    zero_user_pages(elf_plan.total_page_count, image_buffers);
    zero_user_pages(interpreter_plan.total_page_count, interpreter_buffers);
    zero_user_pages(USER_STACK_PAGES, stack_buffers);
    if (is_elf_image) {
        if (!elf_load_user_image(program_start, program_size, &elf_plan,
                                 image_buffers, elf_plan.total_page_count,
                                 PAGE_SIZE)
            || (elf_plan.has_interpreter
                && !elf_load_user_image(
                    user_interpreter_images[task_slot], interpreter_size,
                    &interpreter_plan, interpreter_buffers,
                    interpreter_plan.total_page_count, PAGE_SIZE))) {
            release_user_pages(0, image_physical,
                               elf_plan.total_page_count, 0);
            release_user_pages(0, interpreter_physical,
                               interpreter_plan.total_page_count, 0);
            release_user_pages(0, stack_physical, USER_STACK_PAGES, 0);
            return false;
        }
    } else {
        for (size_t i = 0; i < program_size; i++) {
            image_buffers[0][i] = program_start[i];
        }
    }

    uint64_t kernel_address_space = paging_kernel_address_space();
    if (!paging_address_space_create(&process->address_space)) {
        release_user_pages(0, image_physical,
                           elf_plan.total_page_count, 0);
        release_user_pages(0, interpreter_physical,
                           interpreter_plan.total_page_count, 0);
        release_user_pages(0, stack_physical, USER_STACK_PAGES, 0);
        return false;
    }
    if (!scheduler_activate_current_address_space(process->address_space)) {
        bool address_space_freed =
            paging_address_space_destroy(process->address_space);
        if (address_space_freed) {
            process->address_space = 0;
        }
        bool image_freed = release_user_pages(
            0, image_physical, elf_plan.total_page_count, 0);
        image_freed = release_user_pages(
            0, interpreter_physical, interpreter_plan.total_page_count, 0)
            && image_freed;
        bool stack_freed = release_user_pages(0, stack_physical, USER_STACK_PAGES, 0);
        if (!address_space_freed || !image_freed || !stack_freed) {
            console_write_string("OPOS: failed to clean up process address space\n");
        }
        return false;
    }

    size_t image_mapped[ELF_MAX_LOAD_SEGMENTS] = {0};
    size_t interpreter_mapped[ELF_MAX_LOAD_SEGMENTS] = {0};
    bool all_image_mapped = map_elf_plan(&elf_plan, image_physical,
                                         image_mapped)
        && (!elf_plan.has_interpreter
            || map_elf_plan(&interpreter_plan, interpreter_physical,
                            interpreter_mapped));
    bool stack_mapped = all_image_mapped
        && map_user_pages(USER_STACK_BASE, stack_physical, USER_STACK_PAGES,
                          PAGING_USER | PAGING_WRITABLE | PAGING_NO_EXECUTE)
               == USER_STACK_PAGES;
    bool permissions_valid = stack_mapped
        && elf_plan_permissions_valid(&elf_plan)
        && (!elf_plan.has_interpreter
            || elf_plan_permissions_valid(&interpreter_plan))
        && paging_user_range_valid(USER_STACK_BASE,
                                   USER_STACK_PAGES * PAGE_SIZE, true, false)
        && !paging_user_range_valid(USER_STACK_BASE,
                                    USER_STACK_PAGES * PAGE_SIZE, false, true);
    if (!permissions_valid) {
        bool image_freed = release_elf_plan_pages(
            &elf_plan, image_physical, image_mapped);
        image_freed = release_elf_plan_pages(
            &interpreter_plan, interpreter_physical, interpreter_mapped)
            && image_freed;
        bool stack_freed = release_user_pages(USER_STACK_BASE, stack_physical,
                                              USER_STACK_PAGES,
                                              stack_mapped ? USER_STACK_PAGES
                                                           : 0);
        bool address_space_freed =
            restore_kernel_address_space(kernel_address_space,
                                         process->address_space);
        if (address_space_freed) {
            process->address_space = 0;
        }
        if (!image_freed || !stack_freed || !address_space_freed) {
            console_write_string("OPOS: failed to clean up process address space\n");
        }
        return false;
    }

    uintptr_t initial_stack = USER_STACK_TOP;
    if (is_elf_image) {
        /* argv[0] is the executable path; an interpreter also gets
           AT_BASE. */
        static struct user_exec_vectors start_vectors[SCHEDULER_TASK_CAPACITY];
        struct user_exec_vectors *vectors = &start_vectors[task_slot];
        initial_stack = user_build_start_vectors(
                vectors, initial_execfn, initial_execfn_length, arguments,
                arguments_length)
            ? user_prepare_exec_stack(stack_buffers[USER_STACK_ARGUMENT_PAGE], &elf_plan, vectors,
                                      USER_MMAP_BASE, NULL, 0)
            : 0;
    }
    if (initial_stack == 0) {
        bool image_freed = release_elf_plan_pages(
            &elf_plan, image_physical, image_mapped);
        image_freed = release_elf_plan_pages(
            &interpreter_plan, interpreter_physical, interpreter_mapped)
            && image_freed;
        bool stack_freed = release_user_pages(USER_STACK_BASE, stack_physical,
                                              USER_STACK_PAGES,
                                              USER_STACK_PAGES);
        bool address_space_freed = restore_kernel_address_space(
            kernel_address_space, process->address_space);
        if (address_space_freed) {
            process->address_space = 0;
        }
        if (!image_freed || !stack_freed || !address_space_freed) {
            console_write_string("OPOS: failed to clean up invalid initial stack\n");
        }
        return false;
    }
    process->id = __atomic_fetch_add(&next_process_id, 1, __ATOMIC_RELAXED);
    process->state = USER_PROCESS_RUNNING;
    process->active_threads = 1;
    process->elf_image = is_elf_image;
    const struct elf_load_segment *writable = is_elf_image
        ? &elf_plan.segments[elf_plan.writable_segment] : NULL;
    process->initial_break = writable != NULL
        ? writable->address + writable->memory_size : 0;
    process->current_break = process->initial_break;
    process->heap_base = is_elf_image
        ? (process->initial_break + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1) : 0;
    process->image_segment_count = elf_plan.segment_count;
    process->image_page_count = elf_plan.total_page_count;
    for (size_t i = 0; i < elf_plan.segment_count; i++) {
        process->image_segments[i] = (struct user_image_segment) {
            .base = elf_plan.segments[i].mapping_base,
            .page_index = elf_plan.segments[i].page_index,
            .page_count = elf_plan.segments[i].page_count,
            .flags = elf_plan.segments[i].flags,
        };
    }
    for (size_t i = 0; i < elf_plan.total_page_count; i++) {
        process->image_pages[i] = image_physical[i];
    }
    for (size_t i = 0; i < interpreter_plan.segment_count; i++) {
        const struct elf_load_segment *segment = &interpreter_plan.segments[i];
        size_t mmap_index =
            (segment->mapping_base - USER_MMAP_BASE) / PAGE_SIZE;
        for (size_t page = 0; page < segment->page_count; page++) {
            process->mmap_pages[mmap_index + page] =
                interpreter_physical[segment->page_index + page];
            process->mmap_flags[mmap_index + page] =
                user_segment_paging_flags(segment->flags);
        }
    }
    for (size_t i = 0; i < USER_STACK_PAGES; i++) {
        process->stack_pages[i] = stack_physical[i];
    }
    user_threads[task_slot] = (struct user_thread) {
        .active = true,
        .id = process->id,
        .process = process,
    };
    scheduler_set_current_context(process);
    scheduler_set_user_fs_base(0);
    scheduler_reset_fpu_state();
    uint64_t status = user_enter(entry_point, initial_stack);
    return user_finish_process(process, status, kernel_address_space,
                               process_id, exit_status);
}

bool user_process_run(enum user_program program, uint64_t *process_id,
                      uint64_t *exit_status) {
    static const char default_execfn[] = "opos-app";
    return user_process_run_image(
        program, NULL, 0, NULL, 0,
        program == USER_PROGRAM_MODULE_ELF ? default_execfn : NULL,
        program == USER_PROGRAM_MODULE_ELF ? sizeof(default_execfn) - 1 : 0,
                                  NULL, NULL, 0, process_id, exit_status);
}

bool user_process_run_elf(const uint8_t *image, size_t image_size,
                          uint64_t *process_id, uint64_t *exit_status) {
    static const char default_execfn[] = "opos-app";
    return user_process_run_elf_path(image, image_size, default_execfn,
                                     sizeof(default_execfn) - 1,
                                     process_id, exit_status);
}

bool user_process_run_elf_path(const uint8_t *image, size_t image_size,
                               const char *exec_path, size_t exec_path_length,
                               uint64_t *process_id, uint64_t *exit_status) {
    return user_process_run_elf_args(image, image_size, exec_path,
                                     exec_path_length, NULL, 0, process_id,
                                     exit_status);
}

bool user_process_run_elf_args(const uint8_t *image, size_t image_size,
                               const char *exec_path, size_t exec_path_length,
                               const char *arguments, size_t arguments_length,
                               uint64_t *process_id, uint64_t *exit_status) {
    if (image == NULL || image_size == 0) {
        return false;
    }
    return user_process_run_image(USER_PROGRAM_MODULE_ELF, image, image_size,
                                  NULL, 0, exec_path, exec_path_length,
                                  NULL, arguments, arguments_length,
                                  process_id, exit_status);
}

enum user_run_result user_process_run_file(
    const char *path, size_t path_length, const char *arguments,
    size_t arguments_length, const struct permission_identity *identity,
    uint64_t *process_id, uint64_t *exit_status) {
    uint8_t *image = user_exec_image_buffer(scheduler_current_slot());
    if (path == NULL || path_length == 0
        || path_length >= USER_FILE_PATH_CAPACITY) {
        return USER_RUN_NOT_FOUND;
    }
    if (image == NULL) {
        return USER_RUN_FAILED;
    }
    size_t size = 0;
    if (!vfs_read_file(path, path_length, image, USER_EXEC_IMAGE_MAX,
                       &size)) {
        return size > USER_EXEC_IMAGE_MAX ? USER_RUN_TOO_LARGE
                                          : USER_RUN_NOT_FOUND;
    }
    if (size < 4 || image[0] != 0x7f || image[1] != 'E' || image[2] != 'L'
        || image[3] != 'F') {
        return USER_RUN_NOT_PROGRAM;
    }
    return user_process_run_image(USER_PROGRAM_MODULE_ELF, image, size,
                                  NULL, 0, path, path_length, identity,
                                  arguments, arguments_length, process_id,
                                  exit_status)
        ? USER_RUN_STARTED : USER_RUN_FAILED;
}

bool user_process_kill(uint64_t process_id) {
    if (process_id == 0) {
        return false;
    }
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY; i++) {
        if (process_slots[i].state == USER_PROCESS_RUNNING
            && process_slots[i].id == process_id) {
            process_slots[i].kill_requested = true;
            user_process_interrupt_threads(&process_slots[i]);
            return true;
        }
    }
    return false;
}

size_t user_process_kill_all(void) {
    size_t running = 0;
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY; i++) {
        if (process_slots[i].state == USER_PROCESS_RUNNING) {
            process_slots[i].kill_requested = true;
            user_process_interrupt_threads(&process_slots[i]);
            running++;
        }
    }
    return running;
}

bool user_current_killed(void) {
    const struct user_process *process = scheduler_current_context();
    return process != NULL && process->kill_requested;
}

uint64_t user_current_process_id(void) {
    const struct user_process *process = scheduler_current_context();
    return process != NULL ? process->id : 0;
}

uint64_t user_task_process_id(uint64_t task_id) {
    size_t slot;
    if (!scheduler_task_slot(task_id, &slot)
        || process_slots[slot].state != USER_PROCESS_RUNNING) {
        return 0;
    }
    return process_slots[slot].id;
}

size_t user_process_list(struct user_process_info *list, size_t capacity) {
    size_t count = 0;
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY && count < capacity;
         i++) {
        const struct user_process *process = &process_slots[i];
        if (process->state != USER_PROCESS_RUNNING) {
            continue;
        }
        struct user_process_info *info = &list[count++];
        *info = (struct user_process_info){
            .id = process->id,
            .threads = process->active_threads,
            .memory_pages = process->image_page_count
                + process->heap_page_count + 1,
        };
        for (size_t j = 0; j < sizeof(info->name); j++) {
            info->name[j] = process->name[j];
        }
        for (size_t page = 0; page < USER_MMAP_MAX_PAGES; page++) {
            if (process->mmap_pages[page] != 0
                && process->mmap_pages[page] != USER_MMAP_RESERVED) {
                info->memory_pages++;
            }
        }
        for (size_t slot = 0; slot < SCHEDULER_TASK_CAPACITY; slot++) {
            if (user_threads[slot].active
                && user_threads[slot].process == process) {
                info->cpu_ticks += scheduler_task_run_ticks(slot);
            }
        }
    }
    return count;
}

void *user_preempt_check(void *interrupt_frame) {
    struct user_process *process = scheduler_current_context();
    if (interrupt_frame == NULL || process == NULL
        || (!process->kill_requested && !process->group_exit)) {
        return interrupt_frame;
    }
    /* The timer stub saves 15 registers below the exception frame. */
    struct exception_frame *frame = (struct exception_frame *)
        ((uint8_t *)interrupt_frame + 15 * sizeof(uint64_t));
    if ((frame->code_segment & 3) != 3) {
        return interrupt_frame;
    }
    if (process->kill_requested) {
        process->fault_status = 128 + LINUX_SIGKILL;
        process->faulted = true;
    } else {
        /* RAX, the first register the stub saved, is the exit status. */
        ((uint64_t *)interrupt_frame)[14] = process->group_status;
    }
    frame->instruction_pointer = (uintptr_t)user_exit_trampoline;
    frame->code_segment = KERNEL_CODE_SELECTOR;
    frame->flags = 0x202;
    frame->stack_pointer = (uintptr_t)(frame + 1);
    frame->stack_segment = KERNEL_DATA_SELECTOR;
    return interrupt_frame;
}

static void user_child_worker(void *argument) {
    struct user_child_job *job = argument;
    bool started;
    if (job->forked) {
        size_t task_slot = scheduler_current_slot();
        struct user_process *process = task_slot < SCHEDULER_TASK_CAPACITY
            ? &process_slots[task_slot] : NULL;
        uint64_t kernel_address_space = paging_kernel_address_space();
        if (process == NULL || process->state != USER_PROCESS_RUNNING
            || !scheduler_activate_current_address_space(
                    process->address_space)) {
            if (process != NULL) {
                user_close_all_files(process);
                user_discard_cloned_process(process);
                process->state = USER_PROCESS_EXITED;
            }
            started = false;
        } else {
            user_threads[task_slot] = (struct user_thread) {
                .active = true,
                .id = process->id,
                .process = process,
                .signal_mask = job->fork_signal_mask,
            };
            scheduler_set_current_context(process);
            scheduler_set_user_fs_base(job->fork_frame.fs_base);
            uint64_t status = user_linux_resume(&job->fork_frame,
                                                 job->fork_stack_pointer);
            started = user_finish_process(process, status,
                                           kernel_address_space,
                                           &job->process_id,
                                           &job->exit_status);
        }
    } else {
        started = user_process_run_image(
            job->elf_image ? USER_PROGRAM_MODULE_ELF : job->program,
            job->elf_image ? job->image_data : NULL,
            job->elf_image ? job->image_size : 0,
            job->cwd, job->cwd_length,
            job->elf_image ? "/tests/elf-test" : NULL,
            job->elf_image ? sizeof("/tests/elf-test") - 1 : 0,
            &job->identity, NULL, 0, &job->process_id, &job->exit_status);
    }

    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    job->started = started;
    if (job->parent_process_id == 0) {
        job->state = USER_CHILD_UNUSED;
    } else {
        job->state = USER_CHILD_FINISHED;
        scheduler_wake_event(job);
        scheduler_wake_event(child_jobs);
    }
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

static void user_thread_worker(void *argument) {
    struct user_thread_job *job = argument;
    size_t slot = scheduler_current_slot();
    bool entered = job != NULL && job->active && job->process != NULL
        && slot < SCHEDULER_TASK_CAPACITY
        && scheduler_activate_current_address_space(job->process->address_space);
    if (entered) {
        user_threads[slot] = (struct user_thread) {
            .active = true,
            .id = job->thread_id,
            .process = job->process,
            .clear_child_tid = job->clear_child_tid,
            .robust_list = job->robust_list,
            .signal_mask = job->signal_mask,
        };
        scheduler_set_current_context(job->process);
        scheduler_set_user_fs_base(job->frame.fs_base);
        (void)user_linux_resume(&job->frame, job->stack_pointer);
    }

    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    if (job != NULL && job->process != NULL) {
        uintptr_t clear_child_tid = entered
            ? user_threads[slot].clear_child_tid : job->clear_child_tid;
        if (clear_child_tid != 0
            && paging_user_range_valid(clear_child_tid, sizeof(uint32_t),
                                       true, false)) {
            *(uint32_t *)clear_child_tid = 0;
            (void)user_futex_wake_locked(job->process, clear_child_tid, 1);
        }
        if (job->process->active_threads > 1) {
            job->process->active_threads--;
        }
        scheduler_wake_event(job->process);
    }
    if (slot < SCHEDULER_TASK_CAPACITY) {
        user_threads[slot] = (struct user_thread) {0};
    }
    scheduler_set_current_context(NULL);
    if (job != NULL) {
        *job = (struct user_thread_job) {0};
    }
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

static int64_t user_spawn_child(struct user_process *parent,
                                uint64_t program_selector,
                                const char *path, size_t path_length) {
    if (parent == NULL || program_selector > 1 || next_child_handle == 0) {
        return -22;
    }

    struct initramfs_file file = {0};
    bool elf_image = program_selector == 1;
    if (elf_image) {
        char kernel_path[256];
        size_t source_offset = path[0] == '/' ? 1 : 0;
        if (source_offset == path_length) {
            return -2;
        }
        size_t kernel_path_length = path_length - source_offset;
        for (size_t i = 0; i < kernel_path_length; i++) {
            kernel_path[i] = path[source_offset + i];
        }
        kernel_path[kernel_path_length] = '\0';
        if (!initramfs_find_file(kernel_path, &file)) {
            return -2;
        }
    }

    struct user_child_job *job = NULL;
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY - 1; i++) {
        if (child_jobs[i].state == USER_CHILD_UNUSED) {
            job = &child_jobs[i];
            break;
        }
    }
    if (job == NULL) {
        return -12;
    }

    *job = (struct user_child_job) {
        .handle = __atomic_fetch_add(&next_child_handle, 1, __ATOMIC_RELAXED),
        .parent_process_id = parent->id,
        .program = USER_PROGRAM_PREEMPT_TEST,
        .image_data = file.data,
        .image_size = file.size,
        .elf_image = elf_image,
        .cwd_length = parent->cwd_length,
        .identity = parent->identity,
        .state = USER_CHILD_RUNNING,
    };
    for (size_t i = 0; i < parent->cwd_length; i++) {
        job->cwd[i] = parent->cwd[i];
    }
    if (!scheduler_spawn(user_child_worker, job, &job->task_id)) {
        job->state = USER_CHILD_UNUSED;
        return -11;
    }
    return (int64_t)job->handle;
}

static int64_t user_wait_child(struct user_process *parent, uint64_t handle,
                               uint64_t *process_id) {
    if (parent == NULL || handle == 0 || process_id == NULL) {
        return -22;
    }

    for (;;) {
        uint64_t flags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
        struct user_child_job *job = NULL;
        for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY - 1; i++) {
            if (child_jobs[i].state != USER_CHILD_UNUSED
                && child_jobs[i].handle == handle
                && child_jobs[i].parent_process_id == parent->id) {
                job = &child_jobs[i];
                break;
            }
        }
        if (job == NULL) {
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return -10;
        }
        if (job->state == USER_CHILD_FINISHED) {
            bool started = job->started;
            uint64_t status = job->exit_status;
            *process_id = job->process_id;
            job->state = USER_CHILD_UNUSED;
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return started ? (int64_t)status : -5;
        }
        if (!scheduler_block_current_locked(job)) {
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return -5;
        }
        if ((flags & (1u << 9)) != 0) {
            __asm__ volatile ("sti" : : : "memory");
        }
    }
}

static int64_t user_wait_linux_child(struct user_process *parent,
                                     int64_t requested_process_id,
                                     uint32_t *wait_status) {
    if (parent == NULL
        || (requested_process_id <= 0 && requested_process_id != -1)) {
        return -10;
    }
    for (;;) {
        uint64_t flags;
        __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
        struct user_child_job *finished = NULL;
        bool matching_child = false;
        for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY - 1; i++) {
            struct user_child_job *job = &child_jobs[i];
            if (job->state == USER_CHILD_UNUSED
                || job->parent_process_id != parent->id
                || (requested_process_id != -1
                    && job->process_id != (uint64_t)requested_process_id)) {
                continue;
            }
            matching_child = true;
            if (job->state == USER_CHILD_FINISHED) {
                finished = job;
                break;
            }
        }
        if (finished != NULL) {
            bool started = finished->started;
            uint64_t process_id = finished->process_id;
            uint64_t status = finished->exit_status;
            finished->state = USER_CHILD_UNUSED;
            if (wait_status != NULL
                && paging_user_range_valid((uintptr_t)wait_status,
                                           sizeof(*wait_status), true,
                                           false)) {
                *wait_status = (uint32_t)((status & 0xffu) << 8);
            }
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return started ? (int64_t)process_id : -5;
        }
        if (!matching_child) {
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return -10;
        }
        if (!scheduler_block_current_locked(child_jobs)) {
            if ((flags & (1u << 9)) != 0) {
                __asm__ volatile ("sti" : : : "memory");
            }
            return -5;
        }
        if ((flags & (1u << 9)) != 0) {
            __asm__ volatile ("sti" : : : "memory");
        }
    }
}

static void user_orphan_children(uint64_t parent_process_id) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    for (size_t i = 0; i < SCHEDULER_TASK_CAPACITY - 1; i++) {
        if (child_jobs[i].parent_process_id != parent_process_id) {
            continue;
        }
        if (child_jobs[i].state == USER_CHILD_FINISHED) {
            child_jobs[i].state = USER_CHILD_UNUSED;
        } else if (child_jobs[i].state == USER_CHILD_RUNNING) {
            child_jobs[i].parent_process_id = 0;
        }
    }
    if ((flags & (1u << 9)) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

bool user_process_self_test(void) {
    uint64_t fault_process_id;
    uint64_t fault_status;
    if (!user_process_run(USER_PROGRAM_FAULT_TEST, &fault_process_id,
                          &fault_status)
        || fault_status != 142) {
        return false;
    }

    uint64_t elf_process_id;
    uint64_t elf_status;
    bool started = user_process_run(USER_PROGRAM_MODULE_ELF, &elf_process_id,
                                    &elf_status);
    if (started && elf_status == 0) {
        console_write_string(
            "OPOS: kernel ELF segments; dynamic RELRO/COPY/IFUNC, versions, TLS, and lifecycle passed\n");
    } else if (started) {
        console_write_string("OPOS: process test exit status ");
        console_write_hex(elf_status);
        console_write_string("\n");
    }
    return started && elf_status == 0
        && elf_process_id == fault_process_id + 1;
}

struct concurrent_process_test_result {
    bool started;
    uint64_t process_id;
    uint64_t exit_status;
};

static struct concurrent_process_test_result concurrent_results[2];

static void concurrent_process_test_worker(void *argument) {
    struct concurrent_process_test_result *result = argument;
    result->started = user_process_run(USER_PROGRAM_PREEMPT_TEST,
                                       &result->process_id,
                                       &result->exit_status);
}

bool user_process_concurrent_self_test(void) {
    concurrent_results[0] = (struct concurrent_process_test_result) {0};
    concurrent_results[1] = (struct concurrent_process_test_result) {0};
    uint64_t first_task;
    uint64_t second_task;
    if (!scheduler_spawn(concurrent_process_test_worker, &concurrent_results[0],
                         &first_task)
        || !scheduler_spawn(concurrent_process_test_worker,
                            &concurrent_results[1], &second_task)) {
        return false;
    }

    while (!scheduler_task_finished(first_task)
           || !scheduler_task_finished(second_task)) {
        __asm__ volatile ("hlt" : : : "memory");
    }

    return concurrent_results[0].started && concurrent_results[1].started
        && concurrent_results[0].exit_status == 0
        && concurrent_results[1].exit_status == 0
        && concurrent_results[0].process_id != concurrent_results[1].process_id;
}

static volatile uint64_t input_wait_background_iterations;

static void input_wait_background_worker(void *argument) {
    (void)argument;
    uint64_t start = timer_ticks();
    while (timer_ticks() - start < 20) {
        input_wait_background_iterations++;
    }
}

bool user_process_input_wait_self_test(void) {
    input_wait_background_iterations = 0;
    uint64_t background_task;
    if (!scheduler_spawn(input_wait_background_worker, NULL,
                         &background_task)) {
        return false;
    }

    uint64_t process_id;
    uint64_t exit_status;
    bool process_started = user_process_run(USER_PROGRAM_INPUT_TEST,
                                             &process_id, &exit_status);
    bool background_completed_while_waiting =
        scheduler_task_finished(background_task);
    while (!scheduler_task_finished(background_task)) {
        __asm__ volatile ("hlt" : : : "memory");
    }
    return process_started && process_id != 0 && exit_status == 0
        && background_completed_while_waiting
        && input_wait_background_iterations != 0;
}
