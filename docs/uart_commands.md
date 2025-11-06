## UART Command Handler (`src/uart_commands.c`, `src/uart_commands.h`)

> **Note:** The UART command thread is disabled by default (`CONFIG_APP_ENABLE_UART_COMMANDS=n`) to save RAM. Re-enable it if you need runtime watchdog tuning and are comfortable with the higher stack usage.

### Responsibilities

- Provide a lightweight polling CLI on the console UART for watchdog-related diagnostics.
- Emit structured telemetry (`EVT,UART_CMD,…`) whenever commands are processed or malformed input is detected.
- Allow operators to query watchdog counters and override the steady-state timeout without recompiling firmware (where hardware permits retuning).

### Commands

| Command | Action |
| ------- | ------ |
| `wdg?` | Print structured watchdog status (`boot_ms`, current IWDG timeout, requested target, override, fallback state, consecutive reset count). |
| `wdg <ms>` | Persist a new steady-state timeout (stored via `persist_state_set_watchdog_override()`), then ask the supervisor to adopt it immediately. |
| `wdg clear` | Remove any stored override and revert to the compile-time default (`CONFIG_APP_WATCHDOG_STEADY_TIMEOUT_MS`). |

### Execution Model

- Creates a dedicated thread (`cmd_thread`) with stack size `CMD_STACK_SIZE` (288 bytes in the current build).
- Polls the console UART using `uart_poll_in()`; the thread sleeps briefly when no characters are available to avoid busy looping.
- Accepts a single-line command terminated by CR/LF, with basic whitespace trimming and error handling (`PARSE_FAIL`, `GARBAGE_TRAILING`, etc.).

### Header (`src/uart_commands.h`)

| Function | Purpose |
| -------- | ------- |
| `uart_commands_start(bool fallback_mode)` | Spawns the command thread (if the feature is enabled via Kconfig) and records whether the system booted in safe mode so that telemetry can reflect it. |

### Dependencies

- `persist_state`: to read/write override values and fetch watchdog counters.
- `supervisor`: to request runtime changes to the steady-state timeout.
- `watchdog_ctrl`: to query the current hardware timeout using `watchdog_ctrl_get_timeout()`.

### Disabling / Enabling

- When the feature is disabled, the stub implementation in `uart_commands.c` simply ignores the call to `uart_commands_start()`. This keeps code size low on 8 KB SRAM MCUs such as the STM32L053.
