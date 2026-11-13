#ifndef OPOS_SHELL_H
#define OPOS_SHELL_H

#include <stdint.h>

void shell_initialize(void);
void shell_set_notice(const char *notice);
void shell_redraw(void);
void shell_handle_character(char character);
void shell_report_dropped_input(uint64_t count);

#endif
