cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyrdb_unit_kv_hydrate)

target_sources(app PRIVATE
  src/main.c
  ${CMAKE_CURRENT_LIST_DIR}/../../zephyrdb_core.c
  ${CMAKE_CURRENT_LIST_DIR}/../../zephyrdb_kv.c
)

target_include_directories(app PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../..
)

target_compile_definitions(app PRIVATE
  CONFIG_ZDB_KV=1
  CONFIG_ZDB_KV_BACKEND_NVS=1
)

target_compile_options(app PRIVATE
  -Wno-error=unused-function
  -Wno-error=unused-variable
)
