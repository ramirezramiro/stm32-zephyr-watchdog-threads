#pragma once

#include <stdbool.h>
#include <stdint.h>

int watchdog_ctrl_init(uint32_t timeout_ms);
int watchdog_ctrl_feed(void);
void watchdog_ctrl_set_enabled(bool enable);
bool watchdog_ctrl_is_enabled(void);
int watchdog_ctrl_retune(uint32_t timeout_ms);
uint32_t watchdog_ctrl_get_timeout(void);
