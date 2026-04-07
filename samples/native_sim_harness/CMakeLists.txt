cmake_minimum_required(VERSION 3.20.0)

# Build as a standalone Zephyr app while consuming ZephyrDB either through a
# local module wrapper or directly from the repository root when that wrapper
# is not present in the checkout.
set(ZEPHYRDB_LOCAL_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR}/modules/zephyrdb-local)
set(ZEPHYRDB_REPO_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
if(EXISTS ${ZEPHYRDB_LOCAL_MODULE_DIR}/zephyr/module.yml)
  list(APPEND EXTRA_ZEPHYR_MODULES ${ZEPHYRDB_LOCAL_MODULE_DIR})
elseif(EXISTS ${ZEPHYRDB_REPO_ROOT_DIR}/zephyr/module.yml)
  list(APPEND EXTRA_ZEPHYR_MODULES ${ZEPHYRDB_REPO_ROOT_DIR})
else()
  message(FATAL_ERROR
    "native_sim_harness: ZephyrDB module not found. Expected either "
    "${ZEPHYRDB_LOCAL_MODULE_DIR}/zephyr/module.yml or "
    "${ZEPHYRDB_REPO_ROOT_DIR}/zephyr/module.yml.")
endif()

# Stage 2 path needs flatcc-zephyr overlay module.
set(FLATCC_ZEPHYR_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../flatcc-zephyr/modules/lib/flatcc-zephyr)
if(EXISTS ${FLATCC_ZEPHYR_MODULE_DIR}/zephyr/module.yml)
  list(APPEND EXTRA_ZEPHYR_MODULES ${FLATCC_ZEPHYR_MODULE_DIR})
else()
  message(FATAL_ERROR
    "native_sim_harness: flatcc-zephyr module not found at ${FLATCC_ZEPHYR_MODULE_DIR}. "
    "Clone beriberikix/flatcc-zephyr next to zephyrdb.")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyrdb_native_sim_harness)

target_sources(app PRIVATE src/main.c)
target_link_libraries(app PRIVATE flatccrt)
