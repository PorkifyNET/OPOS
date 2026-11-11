#ifndef OPOS_USER_H
#define OPOS_USER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum user_program {
    USER_PROGRAM_DEMO,
    USER_PROGRAM_FAULT_TEST,
    USER_PROGRAM_NX_TEST,
    USER_PROGRAM_INPUT_TEST,
    USER_PROGRAM_SHELL,
    USER_PROGRAM_MODULE_ELF,
    USER_PROGRAM_LINUX_READ_TEST,
    USER_PROGRAM_FILE_TEST,
    USER_PROGRAM_PREEMPT_TEST,
    USER_PROGRAM_SPAWN_TEST
};

bool user_set_elf_image(const uint8_t *image, size_t image_size);
bool user_process_run(enum user_program program, uint64_t *process_id,
                      uint64_t *exit_status);
bool user_process_run_elf(const uint8_t *image, size_t image_size,
                          uint64_t *process_id, uint64_t *exit_status);
bool user_process_run_elf_path(const uint8_t *image, size_t image_size,
                               const char *exec_path, size_t exec_path_length,
                               uint64_t *process_id, uint64_t *exit_status);
/* Like user_process_run_elf_path, with arguments for argv[1...]: split at
   spaces, "double quotes" keep spaces. */
bool user_process_run_elf_args(const uint8_t *image, size_t image_size,
                               const char *exec_path, size_t exec_path_length,
                               const char *arguments, size_t arguments_length,
                               uint64_t *process_id, uint64_t *exit_status);
struct permission_identity;

/* Reads an ELF program file (initramfs or disk, at most 1 MiB) into this
   task's buffer and runs it as identity until it ends. */
enum user_run_result {
    USER_RUN_STARTED,
    USER_RUN_NOT_FOUND,
    USER_RUN_TOO_LARGE,
    USER_RUN_NOT_PROGRAM,
    USER_RUN_FAILED,
};
enum user_run_result user_process_run_file(
    const char *path, size_t path_length, const char *arguments,
    size_t arguments_length, const struct permission_identity *identity,
    uint64_t *process_id, uint64_t *exit_status);

/*
 * Ends a running process (as if by SIGKILL, status 137): at its next
 * system call return or timer tick in user mode, and from waits that
 * check user_current_killed. Returns false if no such process runs.
 */
bool user_process_kill(uint64_t process_id);
/* Asks every running process to end; returns how many still run. */
size_t user_process_kill_all(void);
/*
 * Takes capabilities (PERMISSION_CAP_* bits) from every running process
 * of account whose executable has this SHA-256, at once and without
 * asking again. Sockets already open stay open. Returns how many
 * processes changed. Granting never reaches running processes; it
 * applies from the next launch.
 */
size_t user_revoke_capabilities(size_t account, const uint8_t program[32],
                                uint32_t capabilities);
/* Real-time clock as decimal YYYYMMDDhhmmss, or 0 when unreadable. */
uint64_t user_clock_stamp(void);
/* For code running inside a system call of the current process. */
bool user_current_killed(void);
uint64_t user_current_process_id(void);
/* The process a scheduler task runs, or 0. */
uint64_t user_task_process_id(uint64_t task_id);
/* What the Task Manager shows about a running process. */
struct user_process_info {
    uint64_t id;
    char name[32];
    size_t threads;
    size_t memory_pages;
    uint64_t cpu_ticks;
};
size_t user_process_list(struct user_process_info *list, size_t capacity);

/* Timer interrupt return path: sends a killed process to its exit. */
void *user_preempt_check(void *interrupt_frame);

bool user_process_self_test(void);
bool user_process_concurrent_self_test(void);
bool user_process_input_wait_self_test(void);
bool user_exception_dispatch(uint64_t vector, void *exception_frame);
void user_syscall_dispatch(void *saved_registers);
bool user_linux_syscall_dispatch(void *saved_registers);
bool user_linux_signal_deliver(void *saved_registers);
/* Sanitizes the frame before SYSRET; true ends the process instead. */
bool user_linux_return_check(void *saved_registers, bool exiting);

/* Where programs' standard output and error go while set (e.g. the
   desktop Terminal window); NULL means the kernel console. */
typedef void (*user_console_sink)(const char *data, size_t length,
                                  bool error);
void user_set_console_sink(user_console_sink sink);

/* Where programs' standard input comes from while set (the desktop
   Terminal); NULL means the keyboard. Waits for one character; false
   ends the read with an error. Called with interrupts off. */
typedef bool (*user_console_input)(char *character);
void user_set_console_input(user_console_input input);

#endif
