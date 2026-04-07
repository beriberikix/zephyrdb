cmake_minimum_required(VERSION 3.20.0)

set(ZEPHYRDB_LOCAL_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR}/modules/zephyrdb-local)
set(ZEPHYRDB_REPO_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
if(EXISTS ${ZEPHYRDB_LOCAL_MODULE_DIR}/zephyr/module.yml)
  list(APPEND EXTRA_ZEPHYR_MODULES ${ZEPHYRDB_LOCAL_MODULE_DIR})
elseif(EXISTS ${ZEPHYRDB_REPO_MODULE_DIR}/zephyr/module.yml)
  list(APPEND EXTRA_ZEPHYR_MODULES ${ZEPHYRDB_REPO_MODULE_DIR})
else()
  message(FATAL_ERROR
    "core_health_stats: zephyrdb module not found. Expected one of: "
    "${ZEPHYRDB_LOCAL_MODULE_DIR}/zephyr/module.yml or "
    "${ZEPHYRDB_REPO_MODULE_DIR}/zephyr/module.yml")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyrdb_core_health_stats)

target_sources(app PRIVATE src/main.c)
