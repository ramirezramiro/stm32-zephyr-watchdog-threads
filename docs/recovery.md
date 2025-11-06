## Recovery Module (`src/recovery.c`, `src/recovery.h`)

### Responsibilities

- Provide a central place to request and execute warm reboots for different reasons (health fault, manual trigger, safe-mode timeout, watchdog initialisation failure).
- Manage a dedicated recovery thread that blocks on a `k_event`, logs the trigger, sleeps briefly (log flush), and performs `sys_reboot()`.
- Maintain an internal deadline for the optional safe-mode reboot instead of relying on the global system workqueue, eliminating stack-pressure faults.
- Emit structured `EVT,RECOVERY,…` log lines so field captures can explain every reboot cause.

### Public API (`src/recovery.h`)

| Function | Purpose |
| -------- | ------- |
| `recovery_start(void)` | Initializes the recovery event object and spawns the recovery thread. Call once during application startup before any recovery requests are issued. |
| `recovery_request(enum recovery_reason reason)` | Queue a reboot for a specific reason; safe to call from ISR or thread context. Reasons include `RECOVERY_REASON_HEALTH_FAULT`, `RECOVERY_REASON_MANUAL_TRIGGER`, `RECOVERY_REASON_SAFE_MODE_TIMEOUT`, and `RECOVERY_REASON_WATCHDOG_INIT_FAIL`. |
| `recovery_schedule_safe_mode_reboot(uint32_t delay_ms)` | Arms or cancels the safe-mode reboot deadline managed by the recovery thread. Pass `0` to cancel; otherwise schedules a warm restart after `delay_ms`. |

### Internal Components

- **`struct k_event recovery_event`**: receives individual bit flags for each recovery reason.
- **Recovery thread**: waits on `k_event_wait()`, checks each bit, logs an event, sleeps briefly (allowing telemetry flush), and calls `sys_reboot(SYS_REBOOT_WARM)`.
- **Safe-mode scheduler**: a mutex-protected deadline (`safe_mode_deadline`) that the recovery thread polls; once the deadline expires the thread logs `SAFE_MODE_TIMEOUT` and reboots directly.

### Interaction Notes

- The supervisor module calls `recovery_request(RECOVERY_REASON_HEALTH_FAULT)` when liveness checks fail repeatedly.
- The UART CLI (when enabled) can surface manual reset reasons by calling `recovery_request(RECOVERY_REASON_MANUAL_TRIGGER)`.
- `main.c` requests `RECOVERY_REASON_WATCHDOG_INIT_FAIL` if the watchdog cannot be initialised, preventing the firmware from running unsupervised.
- `recovery_schedule_safe_mode_reboot()` is invoked from `main.c` when fallback mode is entered or exited.

### Logging Examples

- `EVT,RECOVERY,QUEUED,reason=0(persistent health fault)` – supervisor escalation.
- `EVT,RECOVERY,SAFE_MODE_REBOOT_SCHEDULED,delay_ms=60000` – safe-mode auto reboot armed.
- `EVT,RECOVERY,SAFE_MODE_TIMEOUT` followed by `EVT,RECOVERY,SAFE_MODE_REBOOT,delay_ms=60000` – recovery thread observed the deadline and rebooted.
- `EVT,RECOVERY,WATCHDOG_INIT_FAIL` – watchdog driver failed to initialise; firmware reboots immediately.

These structured lines combine with the persistence counters to give post-mortem visibility into watchdog storms or manual interventions.
