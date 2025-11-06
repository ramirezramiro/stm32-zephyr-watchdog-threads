#include <zephyr/ztest.h>

#include "persist_state.h"

static void *persist_setup(void)
{
	persist_state_test_reset();
	int rc = persist_state_init();
	zassert_ok(rc, "persist_state_init failed (%d)", rc);
	return NULL;
}

static void persist_before(void *fixture)
{
	ARG_UNUSED(fixture);
	persist_state_test_reset();
	int rc = persist_state_init();
	zassert_ok(rc, "persist_state_init failed (%d)", rc);
}

ZTEST_SUITE(persist_state_suite, NULL, persist_setup, persist_before, NULL, NULL);

ZTEST(persist_state_suite, test_watchdog_counters)
{
	zassert_equal(persist_state_get_consecutive_watchdog(), 0U,
		      "consecutive counter should start at 0");
	zassert_equal(persist_state_get_total_watchdog(), 0U,
		      "total counter should start at 0");

	persist_state_record_boot(true);
	zassert_equal(persist_state_get_consecutive_watchdog(), 1U,
		      "consecutive counter increments on watchdog reset");
	zassert_equal(persist_state_get_total_watchdog(), 1U,
		      "total counter increments on watchdog reset");

	persist_state_record_boot(true);
	zassert_equal(persist_state_get_consecutive_watchdog(), 2U,
		      "consecutive counter accumulates successive watchdog resets");
	zassert_equal(persist_state_get_total_watchdog(), 2U,
		      "total counter accumulates successive watchdog resets");

	persist_state_record_boot(false);
	zassert_equal(persist_state_get_consecutive_watchdog(), 0U,
		      "non-watchdog boot clears consecutive counter");
	zassert_equal(persist_state_get_total_watchdog(), 2U,
		      "total counter persists across clean boot");
}

ZTEST(persist_state_suite, test_fallback_activation)
{
	uint32_t threshold = CONFIG_APP_RESET_WATCHDOG_THRESHOLD;

	for (uint32_t i = 0U; i < (threshold - 1U); ++i) {
		persist_state_record_boot(true);
	}

	zassert_false(persist_state_is_fallback_active(),
		     "fallback should not trigger before threshold");

	persist_state_record_boot(true);
	zassert_true(persist_state_is_fallback_active(),
		    "fallback triggers at threshold");

	persist_state_record_boot(false);
	zassert_false(persist_state_is_fallback_active(),
		     "clean boot exits fallback mode");
}

ZTEST(persist_state_suite, test_watchdog_override_persistence)
{
	zassert_equal(persist_state_get_watchdog_override(), 0U,
		      "override should default to 0");

	int rc = persist_state_set_watchdog_override(2500U);
	zassert_ok(rc, "setting override should succeed (%d)", rc);
	zassert_equal(persist_state_get_watchdog_override(), 2500U,
		      "override should reflect stored value");

	persist_state_test_reload();
	rc = persist_state_init();
	zassert_ok(rc, "persist_state_init after reload failed (%d)", rc);
	zassert_equal(persist_state_get_watchdog_override(), 2500U,
		      "override should persist across reload");

	rc = persist_state_set_watchdog_override(0U);
	zassert_ok(rc, "clearing override should succeed (%d)", rc);
	zassert_equal(persist_state_get_watchdog_override(), 0U,
		      "override should clear to 0");
}
