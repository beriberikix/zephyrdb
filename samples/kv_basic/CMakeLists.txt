cmake_minimum_required(VERSION 3.20.0)

list(APPEND EXTRA_ZEPHYR_MODULES
  ${CMAKE_CURRENT_LIST_DIR}/modules/zephyrdb-local
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyrdb_kv_basic)

target_sources(app PRIVATE src/main.c)
