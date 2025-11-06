#pragma once

#include <stdbool.h>
#include <stdint.h>

int persist_state_init(void);
void persist_state_record_boot(bool watchdog_reset);
void persist_state_clear_watchdog_counter(void);

uint32_t persist_state_get_consecutive_watchdog(void);
uint32_t persist_state_get_total_watchdog(void);
bool persist_state_is_fallback_active(void);

uint32_t persist_state_get_watchdog_override(void);
int persist_state_set_watchdog_override(uint32_t timeout_ms);

#if defined(CONFIG_ZTEST)
void persist_state_test_reset(void);
void persist_state_test_reload(void);
#endif
