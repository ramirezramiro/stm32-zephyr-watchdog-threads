#include "uart_commands.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "log_utils.h"
#include "persist_state.h"
#include "supervisor.h"
#include "watchdog_ctrl.h"

LOG_MODULE_REGISTER(uart_cmd, LOG_LEVEL_INF);

#if IS_ENABLED(CONFIG_APP_ENABLE_UART_COMMANDS)

#define CMD_STACK_SIZE 288
#define CMD_THREAD_PRIORITY 9

K_THREAD_STACK_DEFINE(cmd_stack, CMD_STACK_SIZE);
static struct k_thread cmd_thread;
static bool cmd_fallback_mode;
static const struct device *uart_dev;

static void print_status(void)
{
	uint32_t boot_timeout = CONFIG_APP_WATCHDOG_BOOT_TIMEOUT_MS;
	uint32_t steady_target = supervisor_get_watchdog_target();
	uint32_t current_hw = watchdog_ctrl_get_timeout();
	uint32_t override = persist_state_get_watchdog_override();
	uint32_t consecutive = persist_state_get_consecutive_watchdog();

	LOG_EVT(INF, "TELEMETRY", "WATCHDOG_STATUS",
		"boot_ms=%u,current_ms=%u,target_ms=%u,override_ms=%u,fallback=%s",
		boot_timeout, current_hw, steady_target, override,
		cmd_fallback_mode ? "yes" : "no");
	if (consecutive != 0U) {
		LOG_EVT(INF, "TELEMETRY", "WATCHDOG_RESETS", "count=%u", consecutive);
	}
}

static void apply_timeout(uint32_t timeout_ms)
{
	if (timeout_ms < 100U || timeout_ms > 60000U) {
		LOG_WRN("Timeout %u ms out of range (100-60000)", timeout_ms);
		return;
	}

	int rc = persist_state_set_watchdog_override(timeout_ms);
	if (rc < 0) {
		LOG_ERR("Failed to persist watchdog override: %d", rc);
		return;
	}

	supervisor_request_watchdog_target(timeout_ms, true);
	LOG_EVT(INF, "WATCHDOG", "OVERRIDE_SET", "timeout_ms=%u", timeout_ms);
}

static void clear_override(void)
{
	int rc = persist_state_set_watchdog_override(0U);
	if (rc < 0) {
		LOG_ERR("Failed to clear watchdog override: %d", rc);
		return;
	}

	supervisor_request_watchdog_target(CONFIG_APP_WATCHDOG_STEADY_TIMEOUT_MS, true);
	LOG_EVT(INF, "WATCHDOG", "OVERRIDE_CLEARED",
		"steady_ms=%u", CONFIG_APP_WATCHDOG_STEADY_TIMEOUT_MS);
}

static void handle_line(const char *line)
{
	while (isspace((unsigned char)*line)) {
		line++;
	}

	if (*line == '\0') {
		return;
	}

	if (strncmp(line, "wdg", 3) != 0) {
		LOG_EVT(WRN, "UART_CMD", "UNKNOWN", "cmd=%s", line);
		return;
	}

	line += 3;
	while (isspace((unsigned char)*line)) {
		line++;
	}

	if (*line == '?') {
		print_status();
		return;
	}

	if (strncmp(line, "clear", 5) == 0) {
		clear_override();
		return;
	}

	errno = 0;
	char *end = NULL;
	unsigned long val = strtoul(line, &end, 10);
	if (errno || end == line) {
		LOG_EVT(WRN, "UART_CMD", "PARSE_FAIL", "arg=%s", line);
		return;
	}

	if (*end != '\0' && !isspace((unsigned char)*end)) {
		LOG_EVT(WRN, "UART_CMD", "GARBAGE_TRAILING", "suffix=%s", end);
		return;
	}

	apply_timeout((uint32_t)val);
}

static void command_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_thread_name_set(k_current_get(), "uart_cmd");

	char buffer[48];
	size_t len = 0U;

	LOG_EVT(INF, "UART_CMD", "READY",
		"fallback=%s", cmd_fallback_mode ? "yes" : "no");

	while (1) {
		uint8_t ch;
		int rc = uart_poll_in(uart_dev, &ch);
		if (rc != 0) {
			k_sleep(K_MSEC(20));
			continue;
		}

		if (ch == '\r' || ch == '\n') {
			if (len > 0U) {
				buffer[len] = '\0';
				handle_line(buffer);
				len = 0U;
			}
			continue;
		}

		if (len < sizeof(buffer) - 1U) {
			buffer[len++] = (char)ch;
		}
	}
}

void uart_commands_start(bool fallback_mode)
{
	cmd_fallback_mode = fallback_mode;

	uart_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("Console UART not ready; disabling command handler");
		return;
	}

	k_thread_create(&cmd_thread, cmd_stack, K_THREAD_STACK_SIZEOF(cmd_stack),
			command_thread, NULL, NULL, NULL, CMD_THREAD_PRIORITY, 0,
			K_NO_WAIT);
}

#else /* CONFIG_APP_ENABLE_UART_COMMANDS */

void uart_commands_start(bool fallback_mode)
{
	ARG_UNUSED(fallback_mode);
}

#endif
