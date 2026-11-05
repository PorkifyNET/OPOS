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

#endif
