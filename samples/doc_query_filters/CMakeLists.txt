cmake_minimum_required(VERSION 3.20.0)

list(APPEND EXTRA_ZEPHYR_MODULES
  ${CMAKE_CURRENT_LIST_DIR}/modules/zephyrdb-local
)

set(FLATCC_ZEPHYR_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR}/../../../flatcc-zephyr/modules/lib/flatcc-zephyr)
if(EXISTS ${FLATCC_ZEPHYR_MODULE_DIR}/zephyr/module.yml)
  list(APPEND EXTRA_ZEPHYR_MODULES ${FLATCC_ZEPHYR_MODULE_DIR})
else()
  message(FATAL_ERROR
    "doc_query_filters: flatcc-zephyr module not found at ${FLATCC_ZEPHYR_MODULE_DIR}. "
    "Clone beriberikix/flatcc-zephyr next to zephyrdb.")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyrdb_doc_query_filters)

target_sources(app PRIVATE src/main.c)
target_link_libraries(app PRIVATE flatccrt)
