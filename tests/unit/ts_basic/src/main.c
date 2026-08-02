/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * TS unit tests against the real LittleFS backend, using the fstab
 * automounted /lfs filesystem provided by boards/native_sim.overlay.
 *
 * Each test runs on a fresh zdb instance (the TS context pins one active
 * stream per instance until deinit) and uses its own stream name so
 * stream files do not interact across tests.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>
#include <string.h>

#include "zephyrdb.h"

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static void ts_before(void *fixture)
{
	ARG_UNUSED(fixture);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
}

static void ts_after(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(ts_suite, NULL, NULL, ts_before, ts_after, NULL);

static void ts_append_fixed(zdb_ts_t *ts, const int64_t *values, size_t count,
			    uint64_t first_ts_ms)
{
	for (size_t i = 0U; i < count; i++) {
		zdb_ts_sample_i64_t sample = {
			.ts_ms = first_ts_ms + i,
			.value = values[i],
		};
		zdb_status_t rc = zdb_ts_append_i64(ts, &sample);

		zassert_equal(rc, ZDB_OK, "append %zu failed: %d", i, rc);
	}
}

/*
 * cfg.work_q == NULL is documented to mean "use the system work queue"; async
 * flush used to reject it with ZDB_ERR_UNSUPPORTED instead.
 */
ZTEST(ts_suite, test_ts_flush_async_without_configured_work_q)
{
	static const zdb_cfg_t no_wq_cfg = {
		.kv_backend_fs = NULL,
		.lfs_mount_point = "/lfs",
		.work_q = NULL,
	};
	ZDB_DEFINE_STATIC(no_wq_db, no_wq_cfg);
	zdb_ts_t ts;
	int64_t values[] = {7, 8, 9};
	zdb_status_t rc;

	/* This suite's fixture already initialized g_db; use a separate instance. */
	zassert_equal(zdb_init(&no_wq_db, &no_wq_cfg), ZDB_OK, "init failed");

	rc = zdb_ts_open(&no_wq_db, "t_nowq", &ts);
	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);

	/*
	 * BUSY is fine here: filling the ingest buffer starts a flush on its
	 * own, and this call only asks for one. What must not happen is
	 * ZDB_ERR_UNSUPPORTED, which is how a NULL cfg.work_q used to be
	 * reported.
	 */
	rc = zdb_ts_flush_async(&ts);
	zassert_true((rc == ZDB_OK) || (rc == ZDB_ERR_BUSY),
		     "async flush rejected without cfg.work_q: %d", rc);

	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "sync flush failed: %d", rc);

	(void)zdb_ts_close(&ts);
	(void)zdb_deinit(&no_wq_db);
}

#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
static uint32_t g_flush_events;
static bool g_flush_name_ok = true;

static void ts_event_capture(const zdb_ts_event_t *event, void *user_ctx)
{
	ARG_UNUSED(user_ctx);

	if (event == NULL) {
		return;
	}

	if (event->type == ZDB_TS_EVENT_FLUSH) {
		g_flush_events++;
		/* A flush spans streams, so it names none — but the field must
		 * still be a readable, terminated string.
		 */
		if (memchr(event->stream_name, '\0', sizeof(event->stream_name)) == NULL) {
			g_flush_name_ok = false;
		}
	}
}

static const zdb_ts_event_listener_t g_ts_listeners[] = {
	{ .notify = ts_event_capture, .user_ctx = NULL },
};

/*
 * A flush with listeners attached must deliver an event rather than fault on
 * the stream name it does not have.
 */
ZTEST(ts_suite, test_ts_flush_event_delivered_to_listener)
{
	static const zdb_cfg_t ev_cfg = {
		.kv_backend_fs = NULL,
		.lfs_mount_point = "/lfs",
		.work_q = &k_sys_work_q,
		.ts_event_listeners = g_ts_listeners,
		.ts_event_listener_count = ARRAY_SIZE(g_ts_listeners),
	};
	ZDB_DEFINE_STATIC(ev_db, ev_cfg);
	zdb_ts_t ts;
	int64_t values[] = {1, 2, 3};

	g_flush_events = 0U;
	g_flush_name_ok = true;

	zassert_equal(zdb_init(&ev_db, &ev_cfg), ZDB_OK, "init failed");
	zassert_equal(zdb_ts_open(&ev_db, "t_evt", &ts), ZDB_OK, "open failed");

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_true(g_flush_events > 0U, "no flush event delivered");
	zassert_true(g_flush_name_ok, "flush event stream name is not a valid string");

	(void)zdb_ts_close(&ts);
	(void)zdb_deinit(&ev_db);
}
#endif /* CONFIG_ZDB_EVENTING */

ZTEST(ts_suite, test_ts_open_close_success)
{
	zdb_ts_t ts;
	zdb_status_t rc = zdb_ts_open(&g_db, "t_open", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);
	rc = zdb_ts_close(&ts);
	zassert_equal(rc, ZDB_OK, "close failed: %d", rc);
}

ZTEST(ts_suite, test_ts_open_invalid_args)
{
	zdb_ts_t ts;

	zassert_equal(zdb_ts_open(NULL, "s", &ts), ZDB_ERR_INVAL);
	zassert_equal(zdb_ts_open(&g_db, NULL, &ts), ZDB_ERR_INVAL);
	zassert_equal(zdb_ts_open(&g_db, "s", NULL), ZDB_ERR_INVAL);
	zassert_equal(zdb_ts_open(&g_db, "bad/name", &ts), ZDB_ERR_INVAL);
	zassert_equal(zdb_ts_open(&g_db, "..", &ts), ZDB_ERR_INVAL);
}

ZTEST(ts_suite, test_ts_append_single)
{
	zdb_ts_t ts;
	zdb_ts_sample_i64_t sample = { .ts_ms = 1000U, .value = 100 };
	zdb_status_t rc = zdb_ts_open(&g_db, "t_appone", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_ts_append_i64(&ts, &sample);
	zassert_equal(rc, ZDB_OK, "append failed: %d", rc);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_append_batch_api)
{
	zdb_ts_t ts;
	zdb_ts_sample_i64_t samples[4];
	zdb_ts_agg_result_t result = {0};
	zdb_status_t rc = zdb_ts_open(&g_db, "t_batch", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	for (size_t i = 0U; i < ARRAY_SIZE(samples); i++) {
		samples[i].ts_ms = 2000U + i;
		samples[i].value = (int64_t)(10 * (i + 1U));
	}

	rc = zdb_ts_append_batch_i64(&ts, samples, ARRAY_SIZE(samples));
	zassert_equal(rc, ZDB_OK, "batch append failed: %d", rc);

	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_COUNT, &result);
	zassert_equal(rc, ZDB_OK, "count aggregate failed: %d", rc);
	zassert_equal(result.points, 4U, "expected 4 points, got %u", result.points);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_flush_sync_success)
{
	zdb_ts_t ts;
	zdb_ts_sample_i64_t sample = { .ts_ms = 1000U, .value = 100 };
	zdb_status_t rc = zdb_ts_open(&g_db, "t_flush", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_ts_append_i64(&ts, &sample);
	zassert_equal(rc, ZDB_OK, "append failed: %d", rc);

	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_flush_no_wait)
{
	zdb_ts_t ts;
	zdb_ts_sample_i64_t sample = { .ts_ms = 1000U, .value = 100 };
	zdb_status_t rc = zdb_ts_open(&g_db, "t_tmo", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_ts_append_i64(&ts, &sample);
	zassert_equal(rc, ZDB_OK, "append failed: %d", rc);

	/*
	 * K_NO_WAIT may or may not observe the work completion in time.
	 * k_sem_take(K_NO_WAIT) reports -EBUSY rather than -EAGAIN, so an
	 * incomplete flush surfaces as BUSY instead of TIMEOUT.
	 */
	rc = zdb_ts_flush_sync(&ts, K_NO_WAIT);
	zassert_true((rc == ZDB_OK) || (rc == ZDB_ERR_TIMEOUT) || (rc == ZDB_ERR_BUSY),
		     "expected OK, TIMEOUT, or BUSY, got %d", rc);

	/* A patient flush afterwards must always succeed. */
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "follow-up flush failed: %d", rc);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_query_aggregate_avg)
{
	zdb_ts_t ts;
	const int64_t values[] = {10, 20, 30};
	zdb_ts_agg_result_t result = {0};
	zdb_status_t rc = zdb_ts_open(&g_db, "t_avg", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_AVG, &result);
	zassert_equal(rc, ZDB_OK, "avg aggregate failed: %d", rc);
	zassert_equal(result.points, 3U, "expected 3 points, got %u", result.points);
	zassert_within(result.value, 20.0, 0.0001, "avg mismatch");

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_query_aggregate_min_max_sum)
{
	zdb_ts_t ts;
	const int64_t values[] = {5, 15, 25, 10};
	zdb_ts_agg_result_t result = {0};
	zdb_status_t rc = zdb_ts_open(&g_db, "t_mms", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_MIN, &result);
	zassert_equal(rc, ZDB_OK, "min aggregate failed: %d", rc);
	zassert_within(result.value, 5.0, 0.0001, "min mismatch");

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_MAX, &result);
	zassert_equal(rc, ZDB_OK, "max aggregate failed: %d", rc);
	zassert_within(result.value, 25.0, 0.0001, "max mismatch");

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_SUM, &result);
	zassert_equal(rc, ZDB_OK, "sum aggregate failed: %d", rc);
	zassert_within(result.value, 55.0, 0.0001, "sum mismatch");

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_COUNT, &result);
	zassert_equal(rc, ZDB_OK, "count aggregate failed: %d", rc);
	zassert_equal(result.points, 4U, "expected 4 points, got %u", result.points);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_query_aggregate_window)
{
	zdb_ts_t ts;
	const int64_t values[] = {1, 2, 3, 4, 5};
	zdb_ts_agg_result_t result = {0};
	zdb_ts_window_t inner = { .from_ts_ms = 1001U, .to_ts_ms = 1003U };
	zdb_ts_window_t empty = { .from_ts_ms = 5000U, .to_ts_ms = 6000U };
	zdb_status_t rc = zdb_ts_open(&g_db, "t_win", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_query_aggregate(&ts, inner, ZDB_TS_AGG_COUNT, &result);
	zassert_equal(rc, ZDB_OK, "windowed count failed: %d", rc);
	zassert_equal(result.points, 3U, "expected 3 in-window points, got %u", result.points);

	/* Zero is a real answer for COUNT; only the value-bearing aggregates
	 * have nothing to report on an empty window.
	 */
	rc = zdb_ts_query_aggregate(&ts, empty, ZDB_TS_AGG_COUNT, &result);
	zassert_equal(rc, ZDB_OK, "empty-window count should succeed, got %d", rc);
	zassert_equal(result.points, 0U, "expected 0 in-window points, got %u", result.points);

	rc = zdb_ts_query_aggregate(&ts, empty, ZDB_TS_AGG_SUM, &result);
	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "empty-window sum should be NOT_FOUND, got %d", rc);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_cursor_iteration)
{
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	const int64_t values[] = {100, 101, 102};
	unsigned int count = 0U;
	zdb_status_t rc = zdb_ts_open(&g_db, "t_cur", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor);
	zassert_equal(rc, ZDB_OK, "cursor open failed: %d", rc);

	while ((rc = zdb_cursor_next(&cursor, &record)) == ZDB_OK) {
		zassert_not_null(record.data, "record data NULL");
		zassert_true(record.len > 0U, "record length zero");
		count++;
		zassert_true(count <= ARRAY_SIZE(values), "cursor overran");
	}

	zassert_equal(rc, ZDB_ERR_NOT_FOUND, "cursor should end with NOT_FOUND");
	zassert_equal(count, ARRAY_SIZE(values), "expected %zu records, got %u",
		      ARRAY_SIZE(values), count);

	rc = zdb_cursor_close(&cursor);
	zassert_equal(rc, ZDB_OK, "cursor close failed: %d", rc);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_cursor_includes_unflushed)
{
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	const int64_t flushed[] = {1, 2};
	const int64_t unflushed[] = {3, 4};
	unsigned int count = 0U;
	zdb_status_t rc = zdb_ts_open(&g_db, "t_ram", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, flushed, ARRAY_SIZE(flushed), 1000U);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	ts_append_fixed(&ts, unflushed, ARRAY_SIZE(unflushed), 2000U);

	rc = zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor);
	zassert_equal(rc, ZDB_OK, "cursor open failed: %d", rc);

	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		count++;
	}

	zassert_equal(count, 4U, "expected flushed + RAM records, got %u", count);

	(void)zdb_cursor_close(&cursor);
	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_recover_noop_when_clean)
{
	zdb_ts_t ts;
	size_t truncated = 42U;
	zdb_ts_sample_i64_t sample = { .ts_ms = 1000U, .value = 7 };
	zdb_status_t rc = zdb_ts_open(&g_db, "t_recok", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	rc = zdb_ts_append_i64(&ts, &sample);
	zassert_equal(rc, ZDB_OK, "append failed: %d", rc);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	rc = zdb_ts_recover_stream(&ts, &truncated);
	zassert_equal(rc, ZDB_OK, "recover failed: %d", rc);
	zassert_equal(truncated, 0U, "clean stream truncated %zu bytes", truncated);

	zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_recover_truncates_garbage)
{
	zdb_ts_t ts;
	struct fs_file_t file;
	zdb_ts_agg_result_t result = {0};
	const int64_t values[] = {100, 200};
	const uint8_t garbage[13] = {
		0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11, 0x22, 0x33,
		0x44, 0x55, 0x66, 0x77, 0x88,
	};
	size_t truncated = 0U;
	int fs_rc;
	zdb_status_t rc = zdb_ts_open(&g_db, "t_rec", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	rc = zdb_ts_flush_sync(&ts, K_SECONDS(2));
	zassert_equal(rc, ZDB_OK, "flush failed: %d", rc);

	/* Simulate a torn write: raw garbage appended to the stream file. */
	fs_file_t_init(&file);
	fs_rc = fs_open(&file, "/lfs/" CONFIG_ZDB_TS_DIRNAME "/t_rec.zts",
			FS_O_RDWR | FS_O_APPEND);
	zassert_equal(fs_rc, 0, "opening stream file failed: %d", fs_rc);
	fs_rc = fs_write(&file, garbage, sizeof(garbage));
	zassert_equal(fs_rc, (int)sizeof(garbage), "garbage write failed: %d", fs_rc);
	fs_rc = fs_close(&file);
	zassert_equal(fs_rc, 0, "closing stream file failed: %d", fs_rc);

	rc = zdb_ts_recover_stream(&ts, &truncated);
	zassert_equal(rc, ZDB_OK, "recover failed: %d", rc);
	zassert_equal(truncated, sizeof(garbage),
		      "expected %zu truncated bytes, got %zu", sizeof(garbage), truncated);

	rc = zdb_ts_query_aggregate(&ts, ZDB_TS_WINDOW_ALL, ZDB_TS_AGG_COUNT, &result);
	zassert_equal(rc, ZDB_OK, "count after recovery failed: %d", rc);
	zassert_equal(result.points, 2U, "expected 2 surviving records, got %u",
		      result.points);

	zdb_ts_close(&ts);
}

#if defined(CONFIG_ZDB_STATS)
ZTEST(ts_suite, test_ts_stats_track_recover_runs)
{
	zdb_ts_t ts;
	zdb_ts_stats_t before;
	zdb_ts_stats_t after;
	size_t truncated = 0U;
	zdb_status_t rc = zdb_ts_open(&g_db, "t_stats", &ts);

	zassert_equal(rc, ZDB_OK, "open failed: %d", rc);

	zdb_ts_stats_get(&g_db, &before);
	rc = zdb_ts_recover_stream(&ts, &truncated);
	zassert_equal(rc, ZDB_OK, "recover failed: %d", rc);
	zdb_ts_stats_get(&g_db, &after);

	zassert_equal(after.recover_runs, before.recover_runs + 1U,
		      "recover_runs did not increment");

	zdb_ts_stats_reset(&g_db);
	zdb_ts_stats_get(&g_db, &after);
	zassert_equal(after.recover_runs, 0U, "stats reset did not clear counters");

	zdb_ts_close(&ts);
}
#endif /* CONFIG_ZDB_STATS */

#if defined(CONFIG_ZDB_FLATBUFFERS)
ZTEST(ts_suite, test_ts_sample_export_flatbuffer)
{
	zdb_ts_sample_i64_t sample = { .ts_ms = 1000U, .value = 42 };
	uint8_t buffer[256];
	size_t out_len = 0U;
	zdb_status_t rc;

	rc = zdb_ts_sample_i64_export_flatbuffer(&sample, buffer, sizeof(buffer), &out_len);
	zassert_equal(rc, ZDB_OK, "flatbuffer export failed: %d", rc);
	zassert_true(out_len > 0U, "export produced empty output");
}
#endif /* CONFIG_ZDB_FLATBUFFERS */

/* Reset must rewind the backend read position, not just the public fields. */
ZTEST(ts_suite, test_ts_cursor_reset_rewinds)
{
	zdb_ts_t ts;
	const int64_t values[] = {10, 20, 30};
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	size_t first_pass = 0U;
	size_t second_pass = 0U;

	zassert_equal(zdb_ts_open(&g_db, "t_reset", &ts), ZDB_OK, "open failed");
	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_cursor_open(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor), ZDB_OK,
		      "cursor open failed");

	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		first_pass++;
	}
	zassert_equal(first_pass, ARRAY_SIZE(values), "first pass saw %zu", first_pass);

	zassert_equal(zdb_cursor_reset(&cursor), ZDB_OK, "reset failed");

	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		second_pass++;
	}
	zassert_equal(second_pass, ARRAY_SIZE(values), "reset cursor re-yielded %zu", second_pass);

	(void)zdb_cursor_close(&cursor);
	(void)zdb_ts_close(&ts);
}

/* Descending order must be the exact reverse of ascending. */
ZTEST(ts_suite, test_ts_cursor_descending_reverses_order)
{
	zdb_ts_t ts;
	const int64_t values[] = {1, 2, 3, 4, 5};
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	int64_t seen[ARRAY_SIZE(values)];
	size_t count = 0U;

	zassert_equal(zdb_ts_open(&g_db, "t_desc", &ts), ZDB_OK, "open failed");
	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_cursor_open_desc(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor), ZDB_OK,
		      "descending cursor open failed");

	while ((count < ARRAY_SIZE(seen)) && (zdb_cursor_next(&cursor, &record) == ZDB_OK)) {
		struct {
			uint32_t magic_le;
			uint16_t version_le;
			uint16_t reserved_le;
			uint64_t ts_ms_le;
			uint64_t value_le;
			uint32_t crc_le;
		} __packed rec;

		zassert_equal(record.len, sizeof(rec), "unexpected record size");
		(void)memcpy(&rec, record.data, sizeof(rec));
		seen[count] = (int64_t)sys_le64_to_cpu(rec.value_le);
		count++;
	}

	zassert_equal(count, ARRAY_SIZE(values), "descending yielded %zu records", count);
	for (size_t i = 0U; i < count; i++) {
		zassert_equal(seen[i], values[ARRAY_SIZE(values) - 1U - i],
			      "position %zu: expected %lld, got %lld", i,
			      (long long)values[ARRAY_SIZE(values) - 1U - i], (long long)seen[i]);
	}

	(void)zdb_cursor_close(&cursor);
	(void)zdb_ts_close(&ts);
}

/* Unflushed samples are the newest, so a reverse walk must see them first. */
ZTEST(ts_suite, test_ts_cursor_descending_covers_unflushed)
{
	zdb_ts_t ts;
	const int64_t flushed[] = {1, 2};
	const int64_t buffered[] = {3};
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	int64_t first_value = 0;
	size_t count = 0U;

	zassert_equal(zdb_ts_open(&g_db, "t_desc_ram", &ts), ZDB_OK, "open failed");
	ts_append_fixed(&ts, flushed, ARRAY_SIZE(flushed), 1000U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");
	ts_append_fixed(&ts, buffered, ARRAY_SIZE(buffered), 1010U);

	zassert_equal(zdb_ts_cursor_open_desc(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor), ZDB_OK,
		      "descending cursor open failed");

	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		struct {
			uint32_t magic_le;
			uint16_t version_le;
			uint16_t reserved_le;
			uint64_t ts_ms_le;
			uint64_t value_le;
			uint32_t crc_le;
		} __packed rec;

		(void)memcpy(&rec, record.data, sizeof(rec));
		if (count == 0U) {
			first_value = (int64_t)sys_le64_to_cpu(rec.value_le);
		}
		count++;
	}

	zassert_equal(count, ARRAY_SIZE(flushed) + ARRAY_SIZE(buffered),
		      "descending missed records: %zu", count);
	zassert_equal(first_value, 3, "unflushed sample not returned first: %lld",
		      (long long)first_value);

	(void)zdb_cursor_close(&cursor);
	(void)zdb_ts_close(&ts);
}

/* Window filtering applies in reverse too. */
ZTEST(ts_suite, test_ts_cursor_descending_honours_window)
{
	zdb_ts_t ts;
	const int64_t values[] = {1, 2, 3, 4, 5};
	zdb_ts_window_t inner = {.from_ts_ms = 1001U, .to_ts_ms = 1003U};
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	size_t count = 0U;

	zassert_equal(zdb_ts_open(&g_db, "t_desc_win", &ts), ZDB_OK, "open failed");
	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	zassert_equal(zdb_ts_cursor_open_desc(&ts, inner, NULL, NULL, &cursor), ZDB_OK,
		      "descending cursor open failed");

	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		count++;
	}
	zassert_equal(count, 3U, "window not honoured in reverse: %zu", count);

	(void)zdb_cursor_close(&cursor);
	(void)zdb_ts_close(&ts);
}

/* An empty stream terminates immediately in reverse. */
ZTEST(ts_suite, test_ts_cursor_descending_empty_stream)
{
	zdb_ts_t ts;
	zdb_cursor_t cursor;
	zdb_bytes_t record;

	zassert_equal(zdb_ts_open(&g_db, "t_desc_empty", &ts), ZDB_OK, "open failed");
	zassert_equal(zdb_ts_cursor_open_desc(&ts, ZDB_TS_WINDOW_ALL, NULL, NULL, &cursor), ZDB_OK,
		      "descending cursor open failed");
	zassert_equal(zdb_cursor_next(&cursor, &record), ZDB_ERR_NOT_FOUND,
		      "empty stream yielded a record");

	(void)zdb_cursor_close(&cursor);
	(void)zdb_ts_close(&ts);
}

ZTEST(ts_suite, test_ts_watermark_roundtrip)
{
	zdb_ts_t ts;
	uint64_t mark = 0U;

	zassert_equal(zdb_ts_open(&g_db, "t_wmk", &ts), ZDB_OK, "open failed");

	/* Nothing stored yet. */
	zassert_equal(zdb_ts_watermark_get(&ts, &mark), ZDB_ERR_NOT_FOUND,
		      "unset watermark should report NOT_FOUND");

	zassert_equal(zdb_ts_watermark_set(&ts, 1234U), ZDB_OK, "set failed");
	zassert_equal(zdb_ts_watermark_get(&ts, &mark), ZDB_OK, "get failed");
	zassert_equal(mark, 1234U, "watermark mismatch: %llu", (unsigned long long)mark);

	/* The value is stored, not interpreted: moving it back is allowed. */
	zassert_equal(zdb_ts_watermark_set(&ts, 100U), ZDB_OK, "rewind set failed");
	zassert_equal(zdb_ts_watermark_get(&ts, &mark), ZDB_OK, "get failed");
	zassert_equal(mark, 100U, "watermark did not move back: %llu", (unsigned long long)mark);

	zassert_equal(zdb_ts_watermark_clear(&ts), ZDB_OK, "clear failed");
	zassert_equal(zdb_ts_watermark_get(&ts, &mark), ZDB_ERR_NOT_FOUND,
		      "cleared watermark still readable");

	/* Clearing again is the state the caller asked for. */
	zassert_equal(zdb_ts_watermark_clear(&ts), ZDB_OK, "second clear failed");

	(void)zdb_ts_close(&ts);
}

/* The point of the watermark is that it outlives the process. */
ZTEST(ts_suite, test_ts_watermark_survives_reinit)
{
	zdb_ts_t ts;
	uint64_t mark = 0U;

	zassert_equal(zdb_ts_open(&g_db, "t_wmk_boot", &ts), ZDB_OK, "open failed");
	zassert_equal(zdb_ts_watermark_set(&ts, 5150U), ZDB_OK, "set failed");
	(void)zdb_ts_close(&ts);

	(void)zdb_deinit(&g_db);
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "re-init failed");

	zassert_equal(zdb_ts_open(&g_db, "t_wmk_boot", &ts), ZDB_OK, "reopen failed");
	zassert_equal(zdb_ts_watermark_get(&ts, &mark), ZDB_OK, "get after re-init failed");
	zassert_equal(mark, 5150U, "watermark lost across re-init: %llu",
		      (unsigned long long)mark);

	(void)zdb_ts_close(&ts);
}

/* A damaged mark must read as unset, so the consumer replays rather than skips. */
ZTEST(ts_suite, test_ts_watermark_corrupt_reads_as_unset)
{
	zdb_ts_t ts;
	struct fs_file_t file;
	uint64_t mark = 0U;
	uint8_t byte = 0U;
	int fs_rc;

	zassert_equal(zdb_ts_open(&g_db, "t_wmk_bad", &ts), ZDB_OK, "open failed");
	zassert_equal(zdb_ts_watermark_set(&ts, 4242U), ZDB_OK, "set failed");

	fs_file_t_init(&file);
	fs_rc = fs_open(&file, "/lfs/zdb/t_wmk_bad.wmk", FS_O_READ | FS_O_WRITE);
	zassert_equal(fs_rc, 0, "opening watermark file failed: %d", fs_rc);
	/* Flip a byte inside the timestamp, which the CRC covers. */
	fs_rc = fs_seek(&file, 8, FS_SEEK_SET);
	zassert_equal(fs_rc, 0, "seek failed: %d", fs_rc);
	fs_rc = fs_read(&file, &byte, sizeof(byte));
	zassert_equal(fs_rc, (int)sizeof(byte), "read failed: %d", fs_rc);
	byte ^= 0xFFU;
	fs_rc = fs_seek(&file, 8, FS_SEEK_SET);
	zassert_equal(fs_rc, 0, "seek failed: %d", fs_rc);
	fs_rc = fs_write(&file, &byte, sizeof(byte));
	zassert_equal(fs_rc, (int)sizeof(byte), "write failed: %d", fs_rc);
	zassert_equal(fs_close(&file), 0, "close failed");

	zassert_equal(zdb_ts_watermark_get(&ts, &mark), ZDB_ERR_NOT_FOUND,
		      "corrupt watermark was trusted");

	(void)zdb_ts_close(&ts);
}

/* The workflow the watermark exists for: resume where the last drain stopped. */
ZTEST(ts_suite, test_ts_watermark_resumes_drain)
{
	zdb_ts_t ts;
	const int64_t values[] = {1, 2, 3, 4, 5};
	zdb_ts_window_t window = ZDB_TS_WINDOW_ALL;
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	uint64_t mark = 0U;
	size_t remaining = 0U;

	zassert_equal(zdb_ts_open(&g_db, "t_wmk_drain", &ts), ZDB_OK, "open failed");
	ts_append_fixed(&ts, values, ARRAY_SIZE(values), 1000U);
	zassert_equal(zdb_ts_flush_sync(&ts, K_SECONDS(2)), ZDB_OK, "flush failed");

	/* Consumer handled the first three samples (1000..1002). */
	zassert_equal(zdb_ts_watermark_set(&ts, 1002U), ZDB_OK, "set failed");

	zassert_equal(zdb_ts_watermark_get(&ts, &mark), ZDB_OK, "get failed");
	window.from_ts_ms = mark + 1U;

	zassert_equal(zdb_ts_cursor_open(&ts, window, NULL, NULL, &cursor), ZDB_OK,
		      "cursor open failed");
	while (zdb_cursor_next(&cursor, &record) == ZDB_OK) {
		remaining++;
	}
	zassert_equal(remaining, 2U, "expected 2 unprocessed samples, got %zu", remaining);

	(void)zdb_cursor_close(&cursor);
	(void)zdb_ts_close(&ts);
}
