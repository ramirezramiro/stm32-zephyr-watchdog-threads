# Architecture Overview

This application is built around a handful of tightly scoped threads that
coordinate LED liveness, watchdog feeding, persistence, and recovery on the
NUCLEO-L053R8.

## Threads and Responsibilities

- `main`: boot-time orchestration. Loads persistent counters, applies watchdog
  boot configuration, starts the recovery thread up front, and spawns the
  remaining workers. If `watchdog_ctrl_init()` ever fails, `main` immediately
  queues a recovery so the MCU does not run without a controllable watchdog.
- `health_thread`: drives the LED heartbeat (500 ms cadence in steady mode) and
  raises system liveness marks once per second.
- `supervisor_thread`: samples liveness marks, feeds or retunes the watchdog,
  and escalates to recovery if heartbeat data goes stale.
- `recovery_thread`: listens for escalations or timeouts, logs the trigger, and
  issues a warm reboot after a short delay.
- `uart_cmd` (optional): parses `wdg` commands on the console to inspect or
  adjust watchdog settings during development.

## Watchdog Flow

1. `watchdog_ctrl_init()` installs the single STM32 IWDG channel and applies the
   generous boot timeout defined by `CONFIG_APP_WATCHDOG_BOOT_TIMEOUT_MS`.
2. After `CONFIG_APP_WATCHDOG_RETUNE_DELAY_MS`, the supervisor retunes the
   hardware watchdog to the steady-state window stored in persistent state or
   the Kconfig default (on STM32L0 this logs `RETUNE_NOT_SUPPORTED` and the boot
   timeout remains in effect).
3. Each supervisor loop checks LED and system heartbeat ages. The watchdog is
   fed only when both are fresh.
4. Repeated feed failures or stale health data queue a recovery request, which
   ultimately triggers a warm reboot.
5. If the watchdog driver cannot be initialised (`watchdog_ctrl_init()` returns
   an error), the application logs `EVT,RECOVERY,WATCHDOG_INIT_FAIL` and
   reboots immediately rather than risking an uncontrollable watchdog window.

## Persistent State

`persist_state.c` stores a small blob inside the `storage_partition`:

- Consecutive watchdog reset counter (used to gate fallback mode).
- Total watchdog reset counter (for fleet metrics).
- Optional steady-state watchdog override (set via CLI).

Counters are incremented on every boot and cleared once the supervisor confirms
that the system is healthy again.

## Safe-Mode Fallback

When the consecutive reset counter crosses
`CONFIG_APP_RESET_WATCHDOG_THRESHOLD`, the next boot enters fallback mode:

- The health thread slows the LED cadence to 1 Hz.
- The watchdog retune step is skipped, keeping the wider boot window active.
- An optional delayed warm reboot is scheduled with
  `CONFIG_APP_SAFE_MODE_REBOOT_DELAY_MS`. The recovery thread tracks this
  deadline internally and performs the reboot itself, avoiding extra stack
  pressure on the system workqueue.

This behavior provides a visual indication of trouble while keeping the MCU
alive long enough to collect telemetry or manual intervention.
