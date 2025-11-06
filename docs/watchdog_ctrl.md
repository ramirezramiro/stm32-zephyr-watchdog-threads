## Watchdog Controller (`src/watchdog_ctrl.c`, `src/watchdog_ctrl.h`)

### Purpose

Wrap the STM32 IWDG peripheral behind a small API that can be safely called from the rest of the application. The controller encapsulates installation of the watchdog window, feeding, enable/disable logic, and best-effort “retuning”.

### Public API (`src/watchdog_ctrl.h`)

| Function | Description |
| -------- | ----------- |
| `watchdog_ctrl_init(uint32_t timeout_ms)` | Install the IWDG timeout window (if not already done), call `wdt_setup()`, cache the active timeout, and perform an initial feed. Returns 0 on success. |
| `watchdog_ctrl_feed(void)` | Feed the IWDG channel if it is installed and enabled; returns standard Zephyr watchdog error codes (`-EAGAIN`, `-EBUSY`). |
| `watchdog_ctrl_set_enabled(bool enable)` | Gate feeding by flipping an atomic flag. When re-enabling, performs one immediate feed as a safety measure. |
| `watchdog_ctrl_is_enabled(void)` | Query the atomic enable flag. |
| `watchdog_ctrl_retune(uint32_t timeout_ms)` | Attempt to change the active timeout in-place. This succeeds on MCUs that allow runtime reconfiguration but returns `-ENOTSUP` on STM32L0 once the watchdog is running. |
| `watchdog_ctrl_get_timeout(void)` | Return the currently programmed timeout (cached locally). |

### Implementation Details

- Uses Zephyr’s generic watchdog API with the IWDG device node (`DT_NODELABEL(iwdg)`).
- Guards the feed path with an `atomic_t` (`feed_enabled`) so other modules can temporarily suspend feeding without tearing down the peripheral.
- When `CONFIG_APP_WATCHDOG_STEADY_TIMEOUT_MS` differs from the boot timeout the supervisor calls `watchdog_ctrl_retune()`; on STM32L053 this logs an informative warning (`EVT,WATCHDOG,RETUNE_NOT_SUPPORTED,rc=-134`).
- The module caches the active timeout in `current_timeout_ms` because the driver has no way to query it back from hardware.

### Hardware Considerations

- STM32L053 IWDG only allows configuration before the watchdog is started (as per RM0367 §20). This module therefore supports retuning on paper but anticipates `-ENOTSUP` on this board.
- Feeding is performed through the HAL LL macro `LL_IWDG_ReloadCounter()` inside the driver; application code never touches the registers directly.

### Error Handling

- All errors are logged using `LOG_MODULE_REGISTER(watchdog_ctrl, LOG_LEVEL_INF)` so that failures to install or feed the watchdog can be correlated with system resets.
- `watchdog_ctrl_init()` is idempotent; if a channel is already installed it skips reinstallation.
- The main application treats any non-zero return from `watchdog_ctrl_init()` as fatal and requests `RECOVERY_REASON_WATCHDOG_INIT_FAIL`, ensuring the MCU does not continue without a controllable watchdog channel.
