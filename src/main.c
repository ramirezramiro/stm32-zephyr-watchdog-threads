/* main.c - NUCLEO-L053R8: LED + Heartbeat + Watchdog (STM32 IWDG) */
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <stdint.h>

#include "log_utils.h"
#include "persist_state.h"
#include "recovery.h"
#include "supervisor.h"
#include "uart_commands.h"
#include "watchdog_ctrl.h"

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);

/* -------- LED: via devicetree alias 'led0' -------- */
#define LED0_NODE DT_ALIAS(led0)
#if !DT_NODE_HAS_STATUS(LED0_NODE, okay)
#error "Unsupported board: led0 devicetree alias is not defined"
#endif
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/* Optional reset cause printout */
static uint32_t log_reset_cause(void)
{
	uint32_t cause = 0;
	if (hwinfo_get_reset_cause(&cause) == 0) {
		if (cause & RESET_WATCHDOG) LOG_WRN("Reset cause: WATCHDOG");
		if (cause & RESET_SOFTWARE) LOG_WRN("Reset cause: SOFTWARE");
		if (cause & RESET_POR)      LOG_WRN("Reset cause: POWER-ON");
		hwinfo_clear_reset_cause();
	}
	return cause;
}

/* -------- Threads -------- */
K_THREAD_STACK_DEFINE(health_stack, 704);
static struct k_thread health_tid;

static void health_thread(void *p1, void *p2, void *p3)
{
	bool fallback_mode = (bool)(uintptr_t)p1;
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	k_thread_name_set(k_current_get(),
			      fallback_mode ? "health (fallback)" : "health");

	int ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	if (ret) {
		LOG_ERR("LED config failed: %d", ret);
		return;
	}

	if (fallback_mode) {
		LOG_EVT_SIMPLE(WRN, "SAFE_MODE", "LED_SLOW_BLINK");
	}

	const uint32_t led_period_ms = fallback_mode ? 1000U : 500U;
	const uint32_t heartbeat_period_ms = 1000U;
	uint32_t hb_elapsed = 0U;
	uint32_t heartbeat_counter = 0U;

	while (1) {
		ret = gpio_pin_toggle_dt(&led);
		if (ret) {
			LOG_ERR("LED toggle failed: %d", ret);
			k_msleep(1000);
			continue;
		}

		supervisor_notify_led_alive();

		hb_elapsed += led_period_ms;
		if (hb_elapsed >= heartbeat_period_ms) {
			hb_elapsed -= heartbeat_period_ms;
			supervisor_notify_system_alive();
			heartbeat_counter++;

		if ((heartbeat_counter % 10U) == 0U) {
			LOG_EVT(INF, "HEARTBEAT", "OK", "count=%u", heartbeat_counter);
		}
		}

		k_msleep(led_period_ms);
	}
}

void main(void)
{
	k_thread_name_set(k_current_get(), "main");
	LOG_EVT_SIMPLE(INF, "APP", "START");

	int ret = persist_state_init();
	if (ret) {
		LOG_ERR("Persistent state init failed: %d", ret);
	}

	uint32_t reset_cause = log_reset_cause();
	bool watchdog_reset = (reset_cause & RESET_WATCHDOG) != 0U;
	persist_state_record_boot(watchdog_reset);

	uint32_t consecutive = persist_state_get_consecutive_watchdog();
	if (consecutive != 0U) {
		LOG_EVT(WRN, "WATCHDOG", "RESET_HISTORY",
			"consecutive=%u,total=%u", consecutive, persist_state_get_total_watchdog());
	}

	bool fallback_mode = persist_state_is_fallback_active();
	if (fallback_mode) {
		LOG_EVT_SIMPLE(ERR, "SAFE_MODE", "ENTERED");
		persist_state_clear_watchdog_counter();
		LOG_EVT_SIMPLE(INF, "WATCHDOG", "COUNTER_CLEARED");
	}

	recovery_start();
	recovery_schedule_safe_mode_reboot(
		fallback_mode ? CONFIG_APP_SAFE_MODE_REBOOT_DELAY_MS : 0U);

	uint32_t boot_timeout_ms = CONFIG_APP_WATCHDOG_BOOT_TIMEOUT_MS;
	uint32_t steady_timeout_ms = persist_state_get_watchdog_override();
	if (steady_timeout_ms == 0U) {
		steady_timeout_ms = CONFIG_APP_WATCHDOG_STEADY_TIMEOUT_MS;
	}
	uint32_t retune_delay_ms = CONFIG_APP_WATCHDOG_RETUNE_DELAY_MS;

	if (fallback_mode) {
		steady_timeout_ms = MAX(steady_timeout_ms, boot_timeout_ms);
		retune_delay_ms = 0U;
	}

	ret = watchdog_ctrl_init(boot_timeout_ms);
	if (ret) {
		LOG_EVT(ERR, "WATCHDOG", "INIT_FAIL", "rc=%d", ret);
		LOG_EVT_SIMPLE(ERR, "RECOVERY", "WATCHDOG_INIT_FAIL");
		recovery_request(RECOVERY_REASON_WATCHDOG_INIT_FAIL);
		return;
	}

	LOG_EVT(INF, "WATCHDOG", "CONFIGURED",
		"boot_ms=%u,steady_ms=%u,retune_delay_ms=%u", boot_timeout_ms,
		steady_timeout_ms, retune_delay_ms);
	if (fallback_mode) {
		LOG_EVT_SIMPLE(WRN, "WATCHDOG", "RETUNE_DISABLED_SAFE_MODE");
	}

	k_thread_create(&health_tid, health_stack, K_THREAD_STACK_SIZEOF(health_stack),
			health_thread, (void *)(uintptr_t)fallback_mode,
			NULL, NULL, 5, 0, K_NO_WAIT);
	k_thread_name_set(&health_tid, fallback_mode ?
			       "Health Thread (fallback)" : "Health Thread");

	supervisor_start(steady_timeout_ms, retune_delay_ms, !fallback_mode);

	if (IS_ENABLED(CONFIG_APP_ENABLE_UART_COMMANDS)) {
		uart_commands_start(fallback_mode);
	}

	/* small delay to let logging flush before threads settle */
	k_msleep(120);

	LOG_EVT_SIMPLE(INF, "APP", "READY");

}
