# Testing Workflow

This guide expands on the quick summary in the top-level `README.md` and walks
through the tooling you need to execute the native simulation (`native_sim`)
ztest suites that back this project.

## Prerequisites

- Zephyr workspace initialised (`west init …`, `west update …`) and the Zephyr
  environment sourced, for example:

  ```bash
  source $ZEPHYR_BASE/zephyr-env.sh            # or activate the project venv
  ```
- Host packages required by Zephyr's native simulator:
  - `ninja-build`
  - `gcc-multilib`
  - `libc6-dev-i386`
- Python tooling available on the path: `west` and `pykwalify`. These are
  present automatically if you bootstrap via `zephyr-env.sh` or the repository's
  `.venv`. Verify with `pip show west pykwalify`.

## Running the Suites

All commands are issued from the repository root (`zephyr-apps/stm32-zephyr-watchdog-threads`).

### Persist State (`tests/persist_state`)

```bash
west build -b native_sim -p always tests/persist_state --build-dir build/tests/persist_state
west build -t run --build-dir build/tests/persist_state
```

The run target executes the binary via the native simulator and prints the
ztest summary. This suite validates:

- Flash/NVS initialisation retry logic
- Watchdog boot counters and fallback thresholding
- Persistence of watchdog override values across reloads

### Supervisor (`tests/supervisor`)

```bash
west build -b native_sim -p always tests/supervisor --build-dir build/tests/supervisor
west build -t run --build-dir build/tests/supervisor
```

The supervisor suite links the production supervisor code against host-side
stubs to exercise:

- Post-boot watchdog retune scheduling
- Safe-mode behaviour with LED monitoring disabled
- Recovery escalation on heartbeat stalls or manual requests

## Verifying the Environment

- Expect each run to finish with `PROJECT EXECUTION SUCCESSFUL`. Any failure
  halts the build and returns a non-zero exit code.
- The build directories (`build/tests/persist_state`, `build/tests/supervisor`)
  can be re-used for incremental iterations. Pass `-p=auto` instead of
  `-p always` when you no longer need a pristine rebuild.
- For troubleshooting tips, as well as suite-by-suite log excerpts and advanced
  verification commands (e.g., checking generated DTS output), consult
  `tests/README.md`.

## Keeping CI in Sync

The GitHub Actions workflow `.github/workflows/native-sim.yml` runs the same
commands. If you adjust paths or add new suites, update both this document and
the workflow to keep developer instructions aligned with CI.
