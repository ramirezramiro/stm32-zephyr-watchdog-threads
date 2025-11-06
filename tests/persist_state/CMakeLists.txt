cmake_minimum_required(VERSION 3.20.0)

set(KCONFIG_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../Kconfig)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(persist_state_tests)

target_sources(app PRIVATE ../../src/persist_state.c)
target_include_directories(app PRIVATE ../../src)
