cmake_minimum_required(VERSION 3.20.0)

if (BOARD STREQUAL "nucleo_l053r8")
  set(DTC_OVERLAY_FILE
      ${CMAKE_CURRENT_SOURCE_DIR}/boards/nucleo_l053r8.overlay
      ${CMAKE_CURRENT_SOURCE_DIR}/boards/nucleo_l053r8_app.overlay)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(led_threads)

# Add app sources explicitly
target_sources(app PRIVATE
  src/main.c
  src/supervisor.c
  src/recovery.c
  src/persist_state.c
  src/uart_commands.c
  src/watchdog_ctrl.c
)

# Allow #include "supervisor.h" etc without full path (autocomplete)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
