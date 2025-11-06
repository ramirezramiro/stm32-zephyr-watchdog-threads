#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/time_units.h>
#include <zephyr/sys/util.h>

#if DT_NODE_HAS_COMPAT(DT_NODELABEL(iwdg), st_stm32_iwdg)
#include <stm32_ll_iwdg.h>
#endif

#include "watchdog_ctrl.h"

LOG_MODULE_REGISTER(watchdog_ctrl, LOG_LEVEL_INF);

#define WDT_NODE DT_NODELABEL(iwdg)
BUILD_ASSERT(DT_NODE_HAS_STATUS(WDT_NODE, okay), "IWDG node must be available");

static const struct device *const wdt = DEVICE_DT_GET(WDT_NODE);
static atomic_t feed_enabled = ATOMIC_INIT(1);
static int wdt_channel_id = -1;
static uint32_t current_timeout_ms;

#if DT_NODE_HAS_COMPAT(DT_NODELABEL(iwdg), st_stm32_iwdg)
#ifndef LSI_VALUE
#define LSI_VALUE 32000U
#endif

#define IWDG_RELOAD_MAX 0x0FFFU
#define IWDG_PRESCALER_MIN 4U
#define IWDG_PRESCALER_MAX 256U
#define IWDG_SR_UPDATE_TIMEOUT_MS (6U * IWDG_PRESCALER_MAX * MSEC_PER_SEC / LSI_VALUE)

static int stm32_iwdg_compute(uint32_t timeout_ms, uint32_t *prescaler, uint32_t *reload)
{
	uint64_t timeout_us = (uint64_t)timeout_ms * USEC_PER_MSEC;
	uint32_t divider = IWDG_PRESCALER_MIN;
	uint32_t shift = 0U;
	uint32_t ticks = (uint64_t)timeout_us * LSI_VALUE / USEC_PER_SEC;

	while ((ticks / divider) > IWDG_RELOAD_MAX) {
		shift++;
		divider = IWDG_PRESCALER_MIN << shift;
		if (divider > IWDG_PRESCALER_MAX) {
			return -EINVAL;
		}
	}

	uint32_t value = ticks / divider;
	if (value == 0U) {
		return -EINVAL;
	}

	value -= 1U;
	if (value > IWDG_RELOAD_MAX) {
		value = IWDG_RELOAD_MAX;
	}

	*prescaler = shift;
	*reload = value;
	return 0;
}

static int stm32_iwdg_retune_hw(uint32_t timeout_ms)
{
	uint32_t prescaler = 0U;
	uint32_t reload = 0U;
	int rc = stm32_iwdg_compute(timeout_ms, &prescaler, &reload);
	if (rc != 0) {
		return rc;
	}

	LL_IWDG_EnableWriteAccess(IWDG);
	LL_IWDG_SetPrescaler(IWDG, prescaler);
	LL_IWDG_SetReloadCounter(IWDG, reload);

	uint32_t start = k_uptime_get_32();
	while (LL_IWDG_IsReady(IWDG) == 0U) {
		if ((k_uptime_get_32() - start) > IWDG_SR_UPDATE_TIMEOUT_MS) {
			return -EAGAIN;
		}
	}

	LL_IWDG_ReloadCounter(IWDG);
	return 0;
}
#endif

int watchdog_ctrl_init(uint32_t timeout_ms)
{
	if (!device_is_ready(wdt)) {
		LOG_ERR("Watchdog device not ready");
		return -ENODEV;
	}

	struct wdt_timeout_cfg cfg = {
		.window = {
			.min = 0,
			.max = timeout_ms,
		},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	if (wdt_channel_id < 0) {
		int ret = wdt_install_timeout(wdt, &cfg);
		if (ret < 0) {
			LOG_ERR("Failed to install watchdog timeout: %d", ret);
			return ret;
		}

		wdt_channel_id = ret;
	}

	int ret = wdt_setup(wdt, 0);
	if (ret) {
		LOG_ERR("Watchdog setup failed: %d", ret);
		return ret;
	}

	current_timeout_ms = timeout_ms;
	return watchdog_ctrl_feed();
}

int watchdog_ctrl_feed(void)
{
	if (wdt_channel_id < 0) {
		return -EAGAIN;
	}

	if (!atomic_get(&feed_enabled)) {
		return -EBUSY;
	}

	return wdt_feed(wdt, wdt_channel_id);
}

void watchdog_ctrl_set_enabled(bool enable)
{
	atomic_set(&feed_enabled, enable ? 1 : 0);

	if (enable) {
		int ret = watchdog_ctrl_feed();
		if (ret && ret != -EBUSY) {
			LOG_WRN("Watchdog feed after enabling failed: %d", ret);
		}
	}
}

bool watchdog_ctrl_is_enabled(void)
{
	return atomic_get(&feed_enabled) != 0;
}

int watchdog_ctrl_retune(uint32_t timeout_ms)
{
	if (wdt_channel_id < 0) {
		return -EAGAIN;
	}

	if (timeout_ms == 0U) {
		return -EINVAL;
	}

	if (timeout_ms == current_timeout_ms) {
		return 0;
	}

#if DT_NODE_HAS_COMPAT(DT_NODELABEL(iwdg), st_stm32_iwdg)
	int rc = stm32_iwdg_retune_hw(timeout_ms);
	if (rc == 0) {
		current_timeout_ms = timeout_ms;
	}
	return rc;
#else
	ARG_UNUSED(timeout_ms);
	return -ENOTSUP;
#endif
}

uint32_t watchdog_ctrl_get_timeout(void)
{
	return current_timeout_ms;
}
