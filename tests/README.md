# Native Simulation Test Suites

This document captures the full context for the host-based (`native_sim`) test
suites that ship with the project. It expands on the overview in
`README.md` and `docs/testing.md` with in-depth notes, commands, and
troubleshooting guidance.

## Suite Inventory

| Suite                | Purpose |
| -------------------- | ------------------------------------------------------------ |
| `tests/persist_state` | Exercises the flash-backed persistent state layer, ensuring watchdog counters and override values survive reboots. |
| `tests/supervisor`    | Validates watchdog retune scheduling, health monitoring, and recovery escalation using host-side stubs. |

Both suites run entirely on the workstation via Zephyr's native simulator, so
they are a fast way to vet behavioural changes before flashing hardware.

## Environment Setup

1. **Activate the Zephyr toolchain**

   ```bash
   source $ZEPHYR_BASE/zephyr-env.sh                # or activate ~/zephyrproject/.venv
   ```

   This populates the required environment variables (`ZEPHYR_BASE`, `PATH`,
   etc.) and ensures the Python tooling (`west`, `pykwalify`) is available.

2. **Install native simulator prerequisites**

   ```
   sudo apt-get install ninja-build gcc-multilib libc6-dev-i386
   ```

   (If you already ran the GitHub Actions workflow locally, these packages are
   likely present.)

3. **Confirm Python packages**

   ```
   pip show west pykwalify
   ```

   Missing packages? Install them inside your Zephyr virtual environment:

   ```
   pip install west pykwalify
   ```

## Running the Suites

> All commands assume you are at the repository root:
> `~/zephyr-apps/stm32-zephyr-watchdog-threads`.

### 1. Persist State

```bash
west build -b native_sim -p always tests/persist_state --build-dir build/tests/persist_state
west build -t run --build-dir build/tests/persist_state
```

What to look for:

- `PROJECT EXECUTION SUCCESSFUL` at the end of the simulator run.
- Logs confirming that counters increment/reset correctly and that override
  values persist after `persist_state_test_reload()`.

### 2. Supervisor

```bash
west build -b native_sim -p always tests/supervisor --build-dir build/tests/supervisor
west build -t run --build-dir build/tests/supervisor
```

This suite links `supervisor.c` against stubbed implementations of the watchdog,
persistence, and recovery interfaces. Expect the output to show:

- Successful watchdog retune attempts during startup
- Recovery escalation when heartbeats are allowed to go stale
- Manual recovery requests propagating to the stubbed recovery handler
- Safe-mode operation continuing to feed the watchdog even with LED monitoring
  disabled, followed by escalation once the heartbeat goes stale

## Incremental Rebuild Tips

- After the first build, you can switch to `-p=auto` to keep object files
  between iterations.
- Use `west build -t run` by itself to re-run tests without reconfiguring CMake.
- Clean a single suite with `west build -t clean --build-dir build/tests/<suite>`.

## Advanced Verification

The CI workflow and the top-level README highlight several manual checks that
are helpful when testing new watchdog logic. For convenience they are duplicated
here:

1. **Inspect generated Devicetree**

   ```bash
   west build -b nucleo_l053r8 -- -t devicetree
   grep -n "iwdg" build/zephyr/zephyr.dts | head
   ```

   Expect the `iwdg` node to be `status = "okay"` and the
   `storage_partition` definition to match `boards/nucleo_l053r8.overlay`.

2. **List USB serial devices (hardware runs)**

   ```bash
   ls /dev/ttyACM*
   ```

3. **Attach to the board console**

   ```bash
   sudo screen /dev/ttyACM0 115200
   ```

   Watch for structured `EVT,...` log lines signalling watchdog configuration,
   heartbeat telemetry, or safe-mode entry.

## Troubleshooting

- **`west: command not found`** — ensure `zephyr-env.sh` or the project virtual
  environment is sourced.
- **`ModuleNotFoundError: No module named 'pykwalify'` during CMake configure** —
  install `pykwalify` in the active Python environment.
- **`PROJECT EXECUTION FAILED`** — the ztest framework prints assertion
  messages before exiting; scroll up for the first failure to identify the
  regression.
- **Permission errors when installing host packages** — run the `apt-get`
  commands in an elevated shell or coordinate with your system administrator.

For any changes that adjust paths, introduce new test suites, or modify
dependencies, remember to update `.github/workflows/native-sim.yml`,
`docs/testing.md`, and the quick summary in the main README so developer
instructions stay in sync.
