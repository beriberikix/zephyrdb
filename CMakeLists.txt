cmake_minimum_required(VERSION 3.20.0)

zephyr_library()
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
zephyr_library_include_directories(${CMAKE_CURRENT_SOURCE_DIR})
zephyr_library_sources_ifdef(CONFIG_ZEPHYRDB zephyrdb_core.c)
zephyr_library_sources_ifdef(CONFIG_ZDB_KV zephyrdb_kv.c)
zephyr_library_sources_ifdef(CONFIG_ZDB_TS zephyrdb_ts.c)
zephyr_library_sources_ifdef(CONFIG_ZDB_DOC zephyrdb_doc.c)
zephyr_library_sources_ifdef(CONFIG_ZDB_SHELL zephyrdb_shell.c)
# Each model's commands live in their own source, so the shell sources carry no
# CONFIG_ZDB_* preprocessor gates of their own.
if(CONFIG_ZDB_SHELL)
	zephyr_library_sources_ifdef(CONFIG_ZDB_KV zephyrdb_shell_kv.c)
	zephyr_library_sources_ifdef(CONFIG_ZDB_TS zephyrdb_shell_ts.c)
	zephyr_library_sources_ifdef(CONFIG_ZDB_DOC zephyrdb_shell_doc.c)
endif()
zephyr_library_sources_ifdef(CONFIG_ZDB_EVENTING_ZBUS zephyrdb_eventing_zbus.c)

if(CONFIG_ZDB_FLATBUFFERS)
	zephyr_library_link_libraries(flatccrt)
endif()
