# ZephyrDB Test Suite CMakeLists.txt
# Configures and builds all test targets

cmake_minimum_required(VERSION 3.20.0)

# Include Zephyr's default project configuration
include($ENV{ZEPHYR_BASE}/cmake/project.cmake)

project(zephyrdb_tests)

# Add zephyrdb library (parent context)
add_subdirectory(.. zephyrdb_lib)

# Test fixtures library
add_library(zdb_test_fixtures
	fixtures/backends_mock.c
)

target_include_directories(zdb_test_fixtures
	PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/fixtures
	${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(zdb_test_fixtures PUBLIC zephyrdb)

# Unit Tests

# KV Basic Tests
zephyr_get_compile_flags(app_c_flags C)
zephyr_get_include_directories(app_include_dirs)

add_executable(kv_basic_test
	unit/kv_basic_test.c
)
target_include_directories(kv_basic_test PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${app_include_dirs}
)
target_link_libraries(kv_basic_test PRIVATE
	zdb_test_fixtures
	zephyrdb
	ztest
)

# TS Basic Tests
add_executable(ts_basic_test
	unit/ts_basic_test.c
)
target_include_directories(ts_basic_test PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${app_include_dirs}
)
target_link_libraries(ts_basic_test PRIVATE
	zdb_test_fixtures
	zephyrdb
	ztest
)

# DOC Basic Tests
add_executable(doc_basic_test
	unit/doc_basic_test.c
)
target_include_directories(doc_basic_test PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${app_include_dirs}
)
target_link_libraries(doc_basic_test PRIVATE
	zdb_test_fixtures
	zephyrdb
	ztest
)

# Integration Tests
add_executable(workflows_test
	integration/workflows_test.c
)
target_include_directories(workflows_test PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${app_include_dirs}
)
target_link_libraries(workflows_test PRIVATE
	zdb_test_fixtures
	zephyrdb
	ztest
)

# Hardware Tests
add_executable(persistence_nrf52840dk_test
	hardware/persistence_nrf52840dk_test.c
)
target_include_directories(persistence_nrf52840dk_test PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${app_include_dirs}
)
target_link_libraries(persistence_nrf52840dk_test PRIVATE
	zdb_test_fixtures
	zephyrdb
	ztest
)

# Sample Validation Tests
add_executable(verify_samples_test
	samples/verify_samples_test.c
)
target_include_directories(verify_samples_test PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${CMAKE_CURRENT_SOURCE_DIR}/..
	${app_include_dirs}
)
target_link_libraries(verify_samples_test PRIVATE
	zdb_test_fixtures
	zephyrdb
	ztest
)
