#ifndef OPOS_POWER_H
#define OPOS_POWER_H

#include <stdbool.h>
#include <stdint.h>

bool power_initialize(const void *rsdp_address);
bool power_shutdown(void);
void power_reboot(void);

#endif
