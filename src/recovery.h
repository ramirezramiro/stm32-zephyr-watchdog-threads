#pragma once

#include <zephyr/kernel.h>

enum recovery_reason {
	RECOVERY_REASON_HEALTH_FAULT = 0,
	RECOVERY_REASON_MANUAL_TRIGGER,
	RECOVERY_REASON_SAFE_MODE_TIMEOUT,
	RECOVERY_REASON_WATCHDOG_INIT_FAIL,
	RECOVERY_REASON_COUNT
};

void recovery_start(void);
void recovery_request(enum recovery_reason reason);
void recovery_schedule_safe_mode_reboot(uint32_t delay_ms);
