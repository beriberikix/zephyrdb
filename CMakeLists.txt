cmake_minimum_required(VERSION 3.20.0)

zephyr_library()
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
zephyr_library_sources_ifdef(CONFIG_ZEPHYRDB zephyrdb.c)

if(CONFIG_ZDB_FLATBUFFERS)
	zephyr_library_link_libraries(flatccrt)
endif()
