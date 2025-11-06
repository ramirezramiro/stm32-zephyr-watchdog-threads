#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/ztest.h>

#include "persist_state.h"
#include "recovery.h"
#include "supervisor.h"
#include "watchdog_ctrl.h"

static atomic_t stub_feed_calls = ATOMIC_INIT(0);
static atomic_t stub_watchdog_enabled = ATOMIC_INIT(1);
static atomic_t stub_watchdog_timeout = ATOMIC_INIT(8000);
static atomic_t stub_retune_calls = ATOMIC_INIT(0);
static atomic_t stub_persist_clear_calls = ATOMIC_INIT(0);
static atomic_t stub_recovery_calls = ATOMIC_INIT(0);

static int stub_feed_rc;
static int stub_retune_rc;
static atomic_t stub_last_recovery_reason = ATOMIC_INIT(RECOVERY_REASON_COUNT);

static void reset_stub_state(void)
{
	atomic_set(&stub_feed_calls, 0);
	atomic_set(&stub_retune_calls, 0);
	atomic_set(&stub_persist_clear_calls, 0);
	atomic_set(&stub_recovery_calls, 0);
	atomic_set(&stub_last_recovery_reason, RECOVERY_REASON_COUNT);
	atomic_set(&stub_watchdog_enabled, 1);
	atomic_set(&stub_watchdog_timeout, 8000);
	stub_feed_rc = 0;
	stub_retune_rc = 0;
}

int watchdog_ctrl_feed(void)
{
	atomic_inc(&stub_feed_calls);
	return stub_feed_rc;
}

bool watchdog_ctrl_is_enabled(void)
{
	return atomic_get(&stub_watchdog_enabled) != 0;
}

int watchdog_ctrl_retune(uint32_t timeout_ms)
{
	atomic_inc(&stub_retune_calls);
	atomic_set(&stub_watchdog_timeout, (atomic_val_t)timeout_ms);
	return stub_retune_rc;
}

uint32_t watchdog_ctrl_get_timeout(void)
{
	return (uint32_t)atomic_get(&stub_watchdog_timeout);
}

void persist_state_clear_watchdog_counter(void)
{
	atomic_inc(&stub_persist_clear_calls);
}

void recovery_request(enum recovery_reason reason)
{
	atomic_inc(&stub_recovery_calls);
	atomic_set(&stub_last_recovery_reason, (atomic_val_t)reason);
}

static void *supervisor_setup(void)
{
	reset_stub_state();
	supervisor_test_reset();
	supervisor_start(200U, 0U, true);
	supervisor_notify_led_alive();
	supervisor_notify_system_alive();

	return NULL;
}

static void supervisor_before(void *fixture)
{
	ARG_UNUSED(fixture);
	reset_stub_state();
	supervisor_test_reset();
	supervisor_start(200U, 0U, true);
	supervisor_notify_led_alive();
	supervisor_notify_system_alive();
}

ZTEST_SUITE(supervisor_suite, NULL, supervisor_setup, supervisor_before, NULL, NULL);

#if defined(CONFIG_APP_TESTING)
#define TEST_SUPERVISOR_PERIOD_MS 50U
#else
#define TEST_SUPERVISOR_PERIOD_MS 1000U
#endif

ZTEST(supervisor_suite, test_retune_and_recovery_path)
{
	/* Stage 1: ensure the supervisor retunes the watchdog and clears counters. */
	uint32_t waited_ms = 0U;
	while (atomic_get(&stub_persist_clear_calls) == 0 && waited_ms < 500U) {
		supervisor_notify_led_alive();
		supervisor_notify_system_alive();
		k_sleep(K_MSEC(10));
		waited_ms += 10U;
	}

	zassert_true(atomic_get(&stub_retune_calls) > 0,
		     "watchdog retune should be attempted");
	zassert_true(atomic_get(&stub_persist_clear_calls) > 0,
		     "persistent watchdog counter should clear after retune");
	uint32_t retuned_timeout = (uint32_t)atomic_get(&stub_watchdog_timeout);
	zassert_equal(retuned_timeout, 200U,
		      "watchdog retune should adopt steady timeout");
	zassert_equal(atomic_get(&stub_recovery_calls), 0,
		      "recovery must not trigger while healthy");

	/* Stage 2: allow heartbeat data to stale and expect recovery escalation. */
	k_sleep(K_MSEC(CONFIG_APP_HEALTH_LED_STALE_MS + CONFIG_APP_HEALTH_SYS_STALE_MS));

	uint32_t recovery_wait_ms = 0U;
	while (atomic_get(&stub_recovery_calls) == 0 && recovery_wait_ms < 600U) {
		k_sleep(K_MSEC(TEST_SUPERVISOR_PERIOD_MS));
		recovery_wait_ms += TEST_SUPERVISOR_PERIOD_MS;
	}

	zassert_true(atomic_get(&stub_recovery_calls) > 0,
		     "recovery should trigger when health stays stale");
	enum recovery_reason reason =
		(enum recovery_reason)atomic_get(&stub_last_recovery_reason);
	zassert_equal(reason, RECOVERY_REASON_HEALTH_FAULT,
		      "recovery reason should indicate health fault");
}

ZTEST(supervisor_suite, test_safe_mode_ignores_led_monitor)
{
	/* Restart supervisor in safe mode (LED monitoring disabled). */
	supervisor_test_reset();
	reset_stub_state();

	supervisor_start(200U, 0U, false);
	supervisor_notify_system_alive();

	/* With LED monitoring disabled the supervisor should still feed. */
	k_sleep(K_MSEC(TEST_SUPERVISOR_PERIOD_MS + 50U));
	zassert_true(atomic_get(&stub_feed_calls) > 0,
		     "watchdog feed should occur even without LED liveness");
	zassert_equal(atomic_get(&stub_recovery_calls), 0,
		      "recovery must not trigger while heartbeat is fresh");

	/* Let the heartbeat go stale and ensure recovery still escalates. */
	k_sleep(K_MSEC(CONFIG_APP_HEALTH_SYS_STALE_MS + 200U));

	uint32_t waited_ms = 0U;
	while (atomic_get(&stub_recovery_calls) == 0 && waited_ms < 600U) {
		k_sleep(K_MSEC(TEST_SUPERVISOR_PERIOD_MS));
		waited_ms += TEST_SUPERVISOR_PERIOD_MS;
	}

	zassert_true(atomic_get(&stub_recovery_calls) > 0,
		     "recovery should trigger when heartbeat stales in safe mode");
	enum recovery_reason reason =
		(enum recovery_reason)atomic_get(&stub_last_recovery_reason);
	zassert_equal(reason, RECOVERY_REASON_HEALTH_FAULT,
		      "safe-mode recovery should signal health fault");
}

ZTEST(supervisor_suite, test_manual_recovery_request)
{
	supervisor_test_reset();
	reset_stub_state();

	supervisor_request_manual_recovery();

	zassert_equal(atomic_get(&stub_recovery_calls), 1,
		      "manual recovery should route through recovery_request");
	enum recovery_reason reason =
		(enum recovery_reason)atomic_get(&stub_last_recovery_reason);
	zassert_equal(reason, RECOVERY_REASON_MANUAL_TRIGGER,
		      "manual recovery should mark MANUAL_TRIGGER reason");
}
