#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/time_units.h>
#include <zephyr/sys/util.h>
#include <limits.h>
#include <string.h>

#include "log_utils.h"
#include "persist_state.h"
#include "recovery.h"
#include "supervisor.h"
#include "watchdog_ctrl.h"

LOG_MODULE_REGISTER(supervisor, LOG_LEVEL_INF);

#if defined(CONFIG_ZTEST)
#define SUPERVISOR_PERIOD_MS 50U
#define SUPERVISOR_BOOT_GRACE_MS 150U
#else
#define SUPERVISOR_PERIOD_MS 1000U
#define SUPERVISOR_BOOT_GRACE_MS 3000U
#endif
#define SUPERVISOR_MAX_FAILURES 3U

K_THREAD_STACK_DEFINE(supervisor_stack, 672);
static struct k_thread supervisor_tid;
static atomic_t led_last_seen = ATOMIC_INIT(0);
static atomic_t sys_last_seen = ATOMIC_INIT(0);
static int64_t supervisor_boot_ts;
static bool watchdog_counter_cleared;
#if defined(CONFIG_ZTEST)
static bool supervisor_thread_started;
#endif

struct watchdog_cfg {
	uint32_t desired_timeout_ms;
	uint32_t retune_delay_ms;
	int64_t retune_ready_ts;
	bool monitor_led;
	bool retune_pending;
	bool retune_done_once;
	bool retune_failed_logged;
};

static struct watchdog_cfg wd_cfg;
static K_MUTEX_DEFINE(cfg_lock);

static void update_retune_schedule_locked(bool apply_immediately)
{
	int64_t now = k_uptime_get();

	wd_cfg.retune_ready_ts = now + (apply_immediately ? 0 : (int64_t)wd_cfg.retune_delay_ms);
	wd_cfg.retune_pending = (wd_cfg.desired_timeout_ms != watchdog_ctrl_get_timeout());
	if (!wd_cfg.retune_pending) {
		wd_cfg.retune_done_once = true;
	}
	wd_cfg.retune_failed_logged = false;
}

static void snapshot_watchdog_cfg(struct watchdog_cfg *out_cfg)
{
	k_mutex_lock(&cfg_lock, K_FOREVER);
	*out_cfg = wd_cfg;
	k_mutex_unlock(&cfg_lock);
}

static bool feed_watchdog(const char *context, uint32_t *fail_count)
{
	int ret = watchdog_ctrl_feed();

	if (ret == 0) {
		return true;
	}

	if (ret == -EBUSY) {
		LOG_DBG("Watchdog feed skipped (%s): disabled", context);
		return false;
	}

	if (ret == -EAGAIN) {
		LOG_WRN("Watchdog feed unavailable (%s)", context);
		return false;
	}

	LOG_EVT(ERR, "WATCHDOG", "FEED_FAIL", "context=%s,rc=%d", context, ret);

	if (fail_count != NULL) {
		(*fail_count)++;
	}

	return false;
}

struct health_status {
	bool led_ok;
	bool hb_ok;
	uint32_t led_age;
	uint32_t hb_age;
};

static struct health_status sample_health(const struct watchdog_cfg *cfg, uint32_t now32)
{
	struct health_status status = {
		.led_ok = true,
		.hb_ok = false,
		.led_age = 0U,
		.hb_age = 0U,
	};

	uint32_t hb_last = atomic_get(&sys_last_seen);
	if (hb_last == 0U) {
		status.hb_ok = false;
		status.hb_age = UINT32_MAX;
	} else {
		uint32_t age = now32 - hb_last;
		status.hb_age = age;
		status.hb_ok = age <= CONFIG_APP_HEALTH_SYS_STALE_MS;
	}

	if (!cfg->monitor_led) {
		status.led_ok = true;
		status.led_age = 0U;
		return status;
	}

	uint32_t led_last = atomic_get(&led_last_seen);
	if (led_last == 0U) {
		status.led_ok = false;
		status.led_age = UINT32_MAX;
	} else {
		uint32_t age = now32 - led_last;
		status.led_age = age;
		status.led_ok = age <= CONFIG_APP_HEALTH_LED_STALE_MS;
	}

	return status;
}

void supervisor_notify_led_alive(void)
{
	atomic_set(&led_last_seen, k_uptime_get_32());
}

void supervisor_notify_system_alive(void)
{
	atomic_set(&sys_last_seen, k_uptime_get_32());
}

static void attempt_watchdog_retune(const struct watchdog_cfg *cfg, int64_t now)
{
	if (!cfg->retune_pending || now < cfg->retune_ready_ts) {
		return;
	}

	int rc = watchdog_ctrl_retune(cfg->desired_timeout_ms);

	if (rc == 0) {
		LOG_EVT(INF, "WATCHDOG", "RETUNED", "timeout_ms=%u", cfg->desired_timeout_ms);
		k_mutex_lock(&cfg_lock, K_FOREVER);
		wd_cfg.retune_pending = false;
		wd_cfg.retune_done_once = true;
		wd_cfg.retune_failed_logged = false;
		k_mutex_unlock(&cfg_lock);
		persist_state_clear_watchdog_counter();
		watchdog_counter_cleared = true;
		return;
	}

	if (rc == -ENOTSUP || rc == -ENOTTY) {
		if (!cfg->retune_failed_logged) {
			LOG_EVT(WRN, "WATCHDOG", "RETUNE_NOT_SUPPORTED", "rc=%d", rc);
		}
		k_mutex_lock(&cfg_lock, K_FOREVER);
		wd_cfg.retune_pending = false;
		wd_cfg.retune_done_once = true;
		wd_cfg.retune_failed_logged = true;
		k_mutex_unlock(&cfg_lock);
		return;
	}

	LOG_EVT(WRN, "WATCHDOG", "RETUNE_DEFERRED", "rc=%d", rc);
	k_mutex_lock(&cfg_lock, K_FOREVER);
	wd_cfg.retune_ready_ts = now + SUPERVISOR_PERIOD_MS;
	k_mutex_unlock(&cfg_lock);
}

static void supervisor_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_thread_name_set(k_current_get(), "supervisor");

	uint32_t fail_count = 0;

	while (1) {
		int64_t now = k_uptime_get();
		uint32_t now32 = (uint32_t)now;
		struct watchdog_cfg cfg;

		snapshot_watchdog_cfg(&cfg);
		attempt_watchdog_retune(&cfg, now);

		struct health_status health = sample_health(&cfg, now32);
		bool in_boot_grace = (now - supervisor_boot_ts) < SUPERVISOR_BOOT_GRACE_MS;

		if (!watchdog_counter_cleared && cfg.retune_done_once && !in_boot_grace &&
		    health.led_ok && health.hb_ok) {
			persist_state_clear_watchdog_counter();
			watchdog_counter_cleared = true;
		}

		if (in_boot_grace) {
			if (watchdog_ctrl_is_enabled()) {
				(void)feed_watchdog("boot grace", NULL);
			}
			fail_count = 0;
		} else if (health.led_ok && health.hb_ok) {
			if (watchdog_ctrl_is_enabled()) {
				bool fed = feed_watchdog("steady-state", &fail_count);

				if (fed) {
					fail_count = 0;
				} else if (fail_count >= SUPERVISOR_MAX_FAILURES) {
					LOG_ERR("Repeated watchdog feed failures -- requesting recovery");
					recovery_request(RECOVERY_REASON_HEALTH_FAULT);
					fail_count = 0;
				}
			} else {
				fail_count = 0;
			}
		} else {
			fail_count++;
			const char *led_status = cfg.monitor_led ? (health.led_ok ? "ok" : "stale") : "disabled";
			LOG_EVT(WRN, "HEALTH", "DEGRADED",
				"fail=%u,led=%s,led_age_ms=%u,hb=%s,hb_age_ms=%u",
				fail_count,
				led_status,
				cfg.monitor_led ? health.led_age : 0U,
				health.hb_ok ? "ok" : "stale",
				health.hb_age);

			if (fail_count >= SUPERVISOR_MAX_FAILURES) {
				LOG_EVT_SIMPLE(ERR, "HEALTH", "RECOVERY_REQUEST");
				recovery_request(RECOVERY_REASON_HEALTH_FAULT);
				fail_count = 0;
			}
		}

		k_msleep(SUPERVISOR_PERIOD_MS);
	}
}

void supervisor_start(uint32_t steady_timeout_ms, uint32_t retune_delay_ms, bool monitor_led)
{
	supervisor_boot_ts = k_uptime_get();
	watchdog_counter_cleared = false;
	atomic_set(&sys_last_seen, k_uptime_get_32());
	if (!monitor_led) {
		atomic_set(&led_last_seen, k_uptime_get_32());
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	wd_cfg.desired_timeout_ms = steady_timeout_ms;
	wd_cfg.retune_delay_ms = retune_delay_ms;
	wd_cfg.monitor_led = monitor_led;
	wd_cfg.retune_done_once = false;
	wd_cfg.retune_pending = true;
	wd_cfg.retune_failed_logged = false;
	update_retune_schedule_locked(false);
	k_mutex_unlock(&cfg_lock);

	k_thread_create(&supervisor_tid, supervisor_stack,
			K_THREAD_STACK_SIZEOF(supervisor_stack), supervisor_thread,
			NULL, NULL, NULL, 6, 0, K_NO_WAIT);
#if defined(CONFIG_ZTEST)
	supervisor_thread_started = true;
#endif
}

int supervisor_request_watchdog_target(uint32_t timeout_ms, bool apply_immediately)
{
	k_mutex_lock(&cfg_lock, K_FOREVER);
	wd_cfg.desired_timeout_ms = timeout_ms;
	wd_cfg.retune_done_once = false;
	watchdog_counter_cleared = false;
	update_retune_schedule_locked(apply_immediately);
	k_mutex_unlock(&cfg_lock);
	return 0;
}

uint32_t supervisor_get_watchdog_target(void)
{
	k_mutex_lock(&cfg_lock, K_FOREVER);
	uint32_t value = wd_cfg.desired_timeout_ms;
	k_mutex_unlock(&cfg_lock);
	return value;
}

void supervisor_request_manual_recovery(void)
{
	recovery_request(RECOVERY_REASON_MANUAL_TRIGGER);
}

#if defined(CONFIG_ZTEST)
void supervisor_test_reset(void)
{
	if (supervisor_thread_started) {
		k_thread_abort(&supervisor_tid);
		supervisor_thread_started = false;
	}

	atomic_set(&led_last_seen, 0);
	atomic_set(&sys_last_seen, 0);
	watchdog_counter_cleared = false;
	supervisor_boot_ts = 0;

	k_mutex_lock(&cfg_lock, K_FOREVER);
	memset(&wd_cfg, 0, sizeof(wd_cfg));
	k_mutex_unlock(&cfg_lock);
}
#endif
