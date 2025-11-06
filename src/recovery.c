#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>
#include <limits.h>

#include "log_utils.h"
#include "recovery.h"

LOG_MODULE_REGISTER(recovery, LOG_LEVEL_INF);

/* Event used to signal recovery actions */
static struct k_event recovery_event;

K_THREAD_STACK_DEFINE(recovery_stack, 512);
static struct k_thread recovery_tid;

static K_MUTEX_DEFINE(safe_mode_lock);
static const int64_t SAFE_MODE_DEADLINE_INACTIVE = INT64_MIN;
static int64_t safe_mode_deadline = SAFE_MODE_DEADLINE_INACTIVE;
static uint32_t safe_mode_delay_ms;

static const char *recovery_reason_to_str(enum recovery_reason reason)
{
	switch (reason) {
	case RECOVERY_REASON_HEALTH_FAULT:
		return "persistent health fault";
	case RECOVERY_REASON_MANUAL_TRIGGER:
		return "manual recovery request";
	case RECOVERY_REASON_SAFE_MODE_TIMEOUT:
		return "safe-mode timeout";
	case RECOVERY_REASON_WATCHDOG_INIT_FAIL:
		return "watchdog init failure";
	default:
		return "unknown";
	}
}

void recovery_request(enum recovery_reason reason)
{
	if (reason < 0 || reason >= RECOVERY_REASON_COUNT) {
		LOG_WRN("Ignoring recovery request with invalid reason %d", reason);
		return;
	}

	LOG_EVT(WRN, "RECOVERY", "QUEUED", "reason=%d(%s)", reason,
		recovery_reason_to_str(reason));
	k_event_post(&recovery_event, BIT(reason));
}

static void handle_safe_mode_reboot(void)
{
	LOG_EVT_SIMPLE(WRN, "RECOVERY", "SAFE_MODE_TIMEOUT");
	LOG_EVT(WRN, "RECOVERY", "SAFE_MODE_REBOOT",
		"delay_ms=%u", safe_mode_delay_ms);
	sys_reboot(SYS_REBOOT_WARM);
}

static void recovery_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	k_thread_name_set(k_current_get(), "recovery");

	while (1) {
		k_timeout_t wait = K_FOREVER;
		bool deadline_pending = false;

		k_mutex_lock(&safe_mode_lock, K_FOREVER);

			if (safe_mode_deadline != SAFE_MODE_DEADLINE_INACTIVE) {
			int64_t now = k_uptime_get();
			int64_t delta = safe_mode_deadline - now;

			if (delta <= 0) {
				safe_mode_deadline = SAFE_MODE_DEADLINE_INACTIVE;
				k_mutex_unlock(&safe_mode_lock);
				handle_safe_mode_reboot();
				continue;
			}

			if (delta > (int64_t)INT32_MAX) {
				delta = (int64_t)INT32_MAX;
			}

			wait = K_MSEC((uint32_t)delta);
			deadline_pending = true;
		}

		k_mutex_unlock(&safe_mode_lock);

		uint32_t events = k_event_wait(&recovery_event,
					       BIT_MASK(RECOVERY_REASON_COUNT),
					       true, wait);

		if (events == 0U) {
			/* timeout expired */
			if (deadline_pending) {
				continue;
			}
			/* No pending deadline; spurious timeout */
			continue;
		}

		if (events & BIT(RECOVERY_REASON_HEALTH_FAULT)) {
			LOG_EVT_SIMPLE(ERR, "RECOVERY", "HEALTH_FAULT");
			k_msleep(200);
			sys_reboot(SYS_REBOOT_WARM);
		}

		if (events & BIT(RECOVERY_REASON_MANUAL_TRIGGER)) {
			LOG_EVT_SIMPLE(WRN, "RECOVERY", "MANUAL_TRIGGER");
			k_msleep(200);
			sys_reboot(SYS_REBOOT_WARM);
		}

		if (events & BIT(RECOVERY_REASON_SAFE_MODE_TIMEOUT)) {
			k_mutex_lock(&safe_mode_lock, K_FOREVER);
			safe_mode_deadline = SAFE_MODE_DEADLINE_INACTIVE;
			k_mutex_unlock(&safe_mode_lock);
			handle_safe_mode_reboot();
		}

		if (events & BIT(RECOVERY_REASON_WATCHDOG_INIT_FAIL)) {
			LOG_EVT_SIMPLE(ERR, "RECOVERY", "WATCHDOG_INIT_REBOOT");
			k_msleep(200);
			sys_reboot(SYS_REBOOT_WARM);
		}
	}
}

void recovery_start(void)
{
	k_event_init(&recovery_event);
	k_thread_create(&recovery_tid, recovery_stack,
			K_THREAD_STACK_SIZEOF(recovery_stack),
			recovery_thread, NULL, NULL, NULL,
			5, 0, K_NO_WAIT);
}

void recovery_schedule_safe_mode_reboot(uint32_t delay_ms)
{
	k_mutex_lock(&safe_mode_lock, K_FOREVER);

	if (delay_ms == 0U) {
		bool was_scheduled = (safe_mode_deadline != SAFE_MODE_DEADLINE_INACTIVE);

		safe_mode_deadline = SAFE_MODE_DEADLINE_INACTIVE;
		safe_mode_delay_ms = 0U;
		k_mutex_unlock(&safe_mode_lock);

		if (was_scheduled) {
			LOG_EVT_SIMPLE(INF, "RECOVERY", "SAFE_MODE_REBOOT_CANCELLED");
			k_wakeup(&recovery_tid);
		}
		return;
	}

	safe_mode_deadline = k_uptime_get() + (int64_t)delay_ms;
	safe_mode_delay_ms = delay_ms;
	k_mutex_unlock(&safe_mode_lock);

	LOG_EVT(INF, "RECOVERY", "SAFE_MODE_REBOOT_SCHEDULED",
		"delay_ms=%u", delay_ms);
	k_wakeup(&recovery_tid);
}
