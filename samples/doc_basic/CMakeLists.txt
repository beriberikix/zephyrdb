cmake_minimum_required(VERSION 3.20.0)

set(ZEPHYRDB_LOCAL_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR}/modules/zephyrdb-local)
set(ZEPHYRDB_REPO_MODULE_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
if(EXISTS ${ZEPHYRDB_LOCAL_MODULE_DIR}/zephyr/module.yml OR
   EXISTS ${ZEPHYRDB_LOCAL_MODULE_DIR}/module.yml)
  list(APPEND EXTRA_ZEPHYR_MODULES ${ZEPHYRDB_LOCAL_MODULE_DIR})
elseif(EXISTS ${ZEPHYRDB_REPO_MODULE_DIR}/zephyr/module.yml OR
       EXISTS ${ZEPHYRDB_REPO_MODULE_DIR}/module.yml)
  list(APPEND EXTRA_ZEPHYR_MODULES ${ZEPHYRDB_REPO_MODULE_DIR})
else()
  message(FATAL_ERROR
    "doc_basic: zephyrdb module not found. Expected module.yml (or zephyr/module.yml) in one of: "
    "${ZEPHYRDB_LOCAL_MODULE_DIR} or ${ZEPHYRDB_REPO_MODULE_DIR}")
endif()

# flatcc-zephyr nests its Zephyr module.yml under modules/lib/flatcc-zephyr.
# Look in the west workspace layout (<topdir>/modules/lib/flatcc-zephyr)
# first, then a checkout next to this repo.
set(FLATCC_ZEPHYR_CANDIDATES
    ${CMAKE_CURRENT_LIST_DIR}/../../../../modules/lib/flatcc-zephyr/modules/lib/flatcc-zephyr
    ${CMAKE_CURRENT_LIST_DIR}/../../../modules/lib/flatcc-zephyr/modules/lib/flatcc-zephyr
    ${CMAKE_CURRENT_LIST_DIR}/../../../flatcc-zephyr/modules/lib/flatcc-zephyr)
set(FLATCC_ZEPHYR_MODULE_DIR "")
foreach(candidate ${FLATCC_ZEPHYR_CANDIDATES})
  if(EXISTS ${candidate}/zephyr/module.yml)
    set(FLATCC_ZEPHYR_MODULE_DIR ${candidate})
    break()
  endif()
endforeach()
if(FLATCC_ZEPHYR_MODULE_DIR)
  list(APPEND EXTRA_ZEPHYR_MODULES ${FLATCC_ZEPHYR_MODULE_DIR})
else()
  message(FATAL_ERROR
    "doc_basic: flatcc-zephyr module not found. Searched: ${FLATCC_ZEPHYR_CANDIDATES}. "
    "Run 'west update' (flatcc-zephyr is in west.yml) or clone beriberikix/flatcc-zephyr next to zephyrdb.")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zephyrdb_doc_basic)

target_sources(app PRIVATE src/main.c)
target_link_libraries(app PRIVATE flatccrt)
