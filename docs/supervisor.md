## Supervisor Module (`src/supervisor.c`, `src/supervisor.h`)

### Responsibilities

- Own the **health monitoring thread** that feeds the independent watchdog (IWDG) only when both the LED and heartbeat liveness markers are fresh.
- Enforce a **boot grace period** (`SUPERVISOR_BOOT_GRACE_MS`; accelerated to 150 ms when `CONFIG_ZTEST` is enabled) so the rest of the application can start before strict monitoring begins.
- Gate access to `watchdog_ctrl_feed()` and request recovery when persistent faults are detected.
- Attempt watchdog retuning when supported, logging structured events (`EVT,…`) for both successful and failed attempts (`RETUNE_NOT_SUPPORTED` on STM32L0 is expected).

### Key Data Structures

| Symbol | Description |
| ------ | ----------- |
| `struct watchdog_cfg` | Runtime configuration snapshot (desired timeout, retune delay, monitoring flags). Access is protected by `cfg_lock`. |
| `led_last_seen`, `sys_last_seen` | `atomic_t` timestamps (32-bit uptime in ms) that the health thread touches via `supervisor_notify_led_alive()` and `supervisor_notify_system_alive()`. |
| `watchdog_counter_cleared` | Boolean flag used to clear persistent reset counters once the system proves healthy again. |

### Public API (`src/supervisor.h`)

| Function | Purpose |
| -------- | ------- |
| `supervisor_start(uint32_t steady_timeout_ms, uint32_t retune_delay_ms, bool monitor_led)` | Spawns the supervisor thread, programs the desired watchdog targets, and decides whether LED liveness should be monitored (disabled when running in safe mode). |
| `supervisor_notify_led_alive(void)` / `supervisor_notify_system_alive(void)` | Called by the health thread to refresh the liveness timestamps. |
| `supervisor_request_watchdog_target(uint32_t timeout_ms, bool apply_immediately)` | Allows other modules (e.g., the UART CLI) to request a different steady-state timeout. Retune attempts are serialized by `cfg_lock`. |
| `supervisor_get_watchdog_target(void)` | Returns the current desired steady-state timeout in milliseconds. |
| `supervisor_request_manual_recovery(void)` | Forwards a manual recovery request to the recovery module (raised by CLI/tests). |
| `supervisor_test_reset(void)` *(test builds only)* | Aborts the supervisor thread and clears internal state so ztests can restart it cleanly. |

### Thread Behaviour

- Runs once per second (`SUPERVISOR_PERIOD_MS`; shortened to 50 ms in test builds).
- Disables LED liveness tracking entirely when started with `monitor_led=false` (safe mode) while still honouring heartbeat freshness.
- Clears the NVS counter the first time both liveness flags are fresh after a successful retune (or immediately when retuning is unsupported but the system proves healthy).
- Emits structured logs such as:
  - `EVT,WATCHDOG,RETUNED,timeout_ms=2000`
  - `EVT,WATCHDOG,RETUNE_NOT_SUPPORTED,rc=-134` (hardware refuses live retune)
  - `EVT,HEALTH,DEGRADED,fail=1,led=stale,led_age_ms=2000,hb=ok,hb_age_ms=200`
- After `SUPERVISOR_MAX_FAILURES` consecutive misses it posts `RECOVERY_REASON_HEALTH_FAULT` to the recovery module, allowing recovery to reboot.

- **Watchdog controller:** uses `watchdog_ctrl_feed()` / `watchdog_ctrl_retune()` to manipulate the hardware IWDG.
- **Recovery module:** on escalation the supervisor raises a recovery request; it also depends on recovery to schedule safe-mode reboots.
- **Persist state:** clears the NVS counters once stability is proven and honours overrides requested via UART/tests.
- **Native tests:** `tests/supervisor` stubs `watchdog_ctrl_*`, `persist_state_clear_watchdog_counter()`, and `recovery_request()` to validate degradation escalation without hardware.

### Notes

- Stack size defaults to 672 bytes (see `K_THREAD_STACK_DEFINE(supervisor_stack, 672)`), which fits within the tight 8 KB SRAM budget.
- Retuning is optional; on STM32L053 it always logs the `RETUNE_NOT_SUPPORTED` warning, but the logic remains portable to MCUs where runtime retuning is possible.
