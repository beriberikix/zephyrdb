/*
 * Instance-level (core) events: lifecycle and health.
 *
 * Built with CONFIG_ZDB_STATS=n on purpose. The durability counters compile
 * away in that configuration, so this suite is what proves health degradation
 * is tracked independently of them rather than silently never happening.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/fs/fs.h>
#include <string.h>

#include "zephyrdb.h"

#define CORE_EVENTS_MAX 16U

static zdb_core_event_t g_events[CORE_EVENTS_MAX];
static uint32_t g_event_count;
static uint32_t g_second_count;

static void core_capture(const zdb_core_event_t *event, void *user_ctx)
{
	ARG_UNUSED(user_ctx);

	if (event == NULL) {
		return;
	}

	if (g_event_count < CORE_EVENTS_MAX) {
		g_events[g_event_count] = *event;
	}
	g_event_count++;
}

static void core_second(const zdb_core_event_t *event, void *user_ctx)
{
	ARG_UNUSED(user_ctx);
	ARG_UNUSED(event);

	g_second_count++;
}

static const zdb_core_event_listener_t g_listeners[] = {
	/* A null-notify slot must be skipped rather than dereferenced. */
	{ .notify = NULL,         .user_ctx = NULL },
	{ .notify = core_capture, .user_ctx = NULL },
	{ .notify = core_second,  .user_ctx = NULL },
};

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
	.core_event_listeners = g_listeners,
	.core_event_listener_count = ARRAY_SIZE(g_listeners),
};

static uint32_t count_of(zdb_core_event_type_t type)
{
	uint32_t seen = 0U;
	uint32_t i;

	for (i = 0U; (i < g_event_count) && (i < CORE_EVENTS_MAX); i++) {
		if (g_events[i].type == type) {
			seen++;
		}
	}

	return seen;
}

static void setup(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)memset(g_events, 0, sizeof(g_events));
	g_event_count = 0U;
	g_second_count = 0U;
}

ZTEST_SUITE(zephyrdb_core_events, NULL, NULL, setup, NULL, NULL);

/*
 * A successful init reports itself, reaching every listener with a notify and
 * skipping the ones without.
 */
ZTEST(zephyrdb_core_events, test_init_emits_event)
{
	ZDB_DEFINE_STATIC(db, g_cfg);

	zassert_equal(zdb_init(&db, &g_cfg), ZDB_OK, "init failed");

	zassert_equal(g_event_count, 1U, "expected 1 event, got %u", g_event_count);
	zassert_equal(g_events[0].type, ZDB_CORE_EVENT_INIT, "wrong event type");
	zassert_equal(g_events[0].status, ZDB_OK, "init event status not OK");
	zassert_equal(g_events[0].health, ZDB_HEALTH_OK, "fresh instance is not healthy");
	zassert_equal(g_second_count, 1U, "second listener was not called");

	(void)zdb_deinit(&db);
}

/*
 * Teardown is announced while the listeners are still bound — emitting after
 * they are cleared would reach nobody.
 */
ZTEST(zephyrdb_core_events, test_deinit_emits_event)
{
	ZDB_DEFINE_STATIC(db, g_cfg);

	zassert_equal(zdb_init(&db, &g_cfg), ZDB_OK, "init failed");
	zassert_equal(zdb_deinit(&db), ZDB_OK, "deinit failed");

	zassert_equal(g_event_count, 2U, "expected init then deinit, got %u events",
		      g_event_count);
	zassert_equal(g_events[1].type, ZDB_CORE_EVENT_DEINIT, "deinit event missing");
	zassert_equal(g_events[1].status, ZDB_OK, "deinit event status not OK");
}

/* Re-initialising a torn-down instance reports again, in order. */
ZTEST(zephyrdb_core_events, test_lifecycle_events_repeat)
{
	ZDB_DEFINE_STATIC(db, g_cfg);

	zassert_equal(zdb_init(&db, &g_cfg), ZDB_OK, "first init failed");
	zassert_equal(zdb_deinit(&db), ZDB_OK, "first deinit failed");
	zassert_equal(zdb_init(&db, &g_cfg), ZDB_OK, "second init failed");
	zassert_equal(zdb_deinit(&db), ZDB_OK, "second deinit failed");

	zassert_equal(g_event_count, 4U, "expected 4 lifecycle events, got %u", g_event_count);
	zassert_equal(count_of(ZDB_CORE_EVENT_INIT), 2U, "wrong init count");
	zassert_equal(count_of(ZDB_CORE_EVENT_DEINIT), 2U, "wrong deinit count");
}

/*
 * Corrupt a flushed stream, then walk it. Decoding the garbage degrades the
 * instance, which must be reported once — with the value it held before — and
 * must happen even though CONFIG_ZDB_STATS is off in this build.
 */
ZTEST(zephyrdb_core_events, test_health_degrades_without_stats)
{
	ZDB_DEFINE_STATIC(db, g_cfg);
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	struct fs_file_t file;
	uint8_t garbage[64];
	uint32_t health_events;
	int fs_rc;

	(void)memset(garbage, 0xA5, sizeof(garbage));

	zassert_equal(zdb_init(&db, &g_cfg), ZDB_OK, "init failed");
	zassert_equal(zdb_health(&db), ZDB_HEALTH_OK, "fresh instance is not healthy");

	zassert_equal(zdb_ts_open(&db, "ce_bad", &ts), ZDB_OK, "open failed");
	for (int64_t i = 0; i < 4; i++) {
		zdb_ts_sample_i64_t sample = { .ts_ms = 1000U + (uint64_t)i, .value = i };

		zassert_equal(zdb_ts_append_i64(&ts, &sample), ZDB_OK, "append failed");
	}
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	/* Append bytes that cannot decode as a record. */
	fs_file_t_init(&file);
	fs_rc = fs_open(&file, "/lfs/" CONFIG_ZDB_TS_DIRNAME "/ce_bad.zts",
			FS_O_WRITE | FS_O_APPEND);
	zassert_equal(fs_rc, 0, "opening the stream file failed: %d", fs_rc);
	fs_rc = fs_write(&file, garbage, sizeof(garbage));
	zassert_true(fs_rc > 0, "writing garbage failed: %d", fs_rc);
	(void)fs_close(&file);

	zassert_equal(zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor), ZDB_OK,
		      "cursor open failed");
	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		/* Walk to the corruption. */
	}
	(void)zdb_cursor_close(&cursor);

	zassert_equal(zdb_health(&db), ZDB_HEALTH_DEGRADED,
		      "corruption did not degrade health with CONFIG_ZDB_STATS=n");

	health_events = count_of(ZDB_CORE_EVENT_HEALTH);
	zassert_equal(health_events, 1U, "expected exactly 1 health event, got %u",
		      health_events);

	for (uint32_t i = 0U; (i < g_event_count) && (i < CORE_EVENTS_MAX); i++) {
		if (g_events[i].type != ZDB_CORE_EVENT_HEALTH) {
			continue;
		}
		zassert_equal(g_events[i].prev_health, ZDB_HEALTH_OK, "wrong previous health");
		zassert_equal(g_events[i].health, ZDB_HEALTH_DEGRADED, "wrong new health");
	}

	/* A second walk over the same damage must not re-announce the change. */
	zassert_equal(zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor), ZDB_OK,
		      "second cursor open failed");
	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		/* Walk again. */
	}
	(void)zdb_cursor_close(&cursor);

	zassert_equal(count_of(ZDB_CORE_EVENT_HEALTH), 1U,
		      "health reported more than once for one transition");

	(void)zdb_ts_close(&ts);
	(void)zdb_deinit(&db);
}
