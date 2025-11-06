# Board Overlays

This directory contains the Devicetree overlays that tailor Zephyr’s STM32L053R8
board support package for the **stm32-zephyr-watchdog-threads** application.

## Files

- `nucleo_l053r8.overlay` &nbsp;— Base overlay applied to the production build.
  - Routes the Zephyr console (`zephyr,console`) and shell UART to `USART2` at
    115200 bps so logs land on the ST-LINK VCP port.
  - Enables the STM32 Independent Watchdog (`&iwdg`) so the application can
    assume ownership during boot (`watchdog_ctrl_init()`).
  - Re-defines the flash partitions, carving out a 4 KB `storage_partition`
    used by `persist_state.c` for NVS telemetry.
- `nucleo_l053r8_app.overlay` &nbsp;— Secondary overlay referenced by
  `CMakeLists.txt` when building for the NUCLEO-L053R8.
  - Clears the default `zephyr,code-partition` flag from the board DTS.
  - Reassigns `zephyr,code-partition` to the application’s custom
    `code_partition` node defined in `nucleo_l053r8.overlay`.

Zephyr merges these overlays on top of the upstream NUCLEO-L053R8 board DTS,
yielding a devicetree that the application assumes at runtime.

## Adding New Hardware Hooks

When extending the project to other boards or peripherals:

1. Copy the relevant overlay and adjust UART/watchdog aliases to match your
   target board.
2. Ensure any new partitions or GPIO aliases are reflected in your source code
   and Kconfig defaults.
3. Update `CMakeLists.txt` to point `DTC_OVERLAY_FILE` at the new overlay set.

For more background on how these overlays feed the final devicetree, see
`docs/architecture.md` and the generated `build/zephyr/zephyr.dts` after a
successful build.
