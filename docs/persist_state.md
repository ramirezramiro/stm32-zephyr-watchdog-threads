## Persisted State (`src/persist_state.c`, `src/persist_state.h`)

### Purpose

Maintain a small NVS-backed blob that tracks watchdog statistics and optional runtime overrides across resets. This allows the firmware to detect watchdog “storms” and to honour user-configured watchdog timeouts even after a power cycle.

### NVS Layout

| Field | Description |
| ----- | ----------- |
| `magic` | Fixed 32-bit marker (`'LEDR'`) used to detect valid records. |
| `consecutive_watchdog` | Count of successive watchdog resets. Cleared automatically after a healthy run or when safe mode is entered. |
| `total_watchdog` | Lifetime watchdog reset counter (monotonic). |
| `watchdog_override_ms` | Optional steady-state timeout override set via the UART CLI. Zero means “use Kconfig default”. |

### Key Functions

| Function | Description |
| -------- | ----------- |
| `persist_state_init(void)` | Mounts the NVS filesystem (using the `storage` partition) and loads or seeds the blob. Called once at boot. |
| `persist_state_record_boot(bool watchdog_reset)` | Updates counters depending on the most recent reset cause, then writes the blob back to NVS. |
| `persist_state_clear_watchdog_counter(void)` | Resets `consecutive_watchdog` to zero and commits the change. Used when the system exits fallback mode or successfully retunes. |
| `persist_state_get_*()` accessors | Return the current counter or override values without touching flash. |
| `persist_state_set_watchdog_override(uint32_t timeout_ms)` | Stores a new override value, writing to NVS only when the value actually changes. |

### Implementation Notes

- Uses Zephyr’s `flash_area` helpers to open the `storage_partition` (configured in the board overlay) and `nvs_mount()` to initialise the filesystem.
- Both the flash area open and `nvs_mount()` stages apply a short retry loop (`PERSIST_RETRY_LIMIT`) and emit structured `EVT,PERSIST,…` logs so transient flash glitches are visible in telemetry.
- All public APIs are protected by a `k_mutex` to make them safe from concurrent access by threads such as the supervisor or UART command handler.
- Commit operations are intentionally minimal: if a setter detects no change in value it avoids rewriting the flash page.
- When initialisation ultimately fails the module records an `EVT,PERSIST,FOO_FAIL` entry and leaves the blob zeroed so the rest of the application can continue operating without persistence.
