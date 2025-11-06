#pragma once

#include <stdbool.h>
#include <stdint.h>

void supervisor_start(uint32_t steady_timeout_ms, uint32_t retune_delay_ms,
                      bool monitor_led);
void supervisor_notify_led_alive(void);
void supervisor_notify_system_alive(void);
int supervisor_request_watchdog_target(uint32_t timeout_ms, bool apply_immediately);
uint32_t supervisor_get_watchdog_target(void);
void supervisor_request_manual_recovery(void);

#if defined(CONFIG_ZTEST)
void supervisor_test_reset(void);
#endif
