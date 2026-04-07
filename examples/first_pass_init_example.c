#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>

#include "zephyrdb.h"

/*
 * Static slabs satisfy the zero-malloc policy for first-pass integration.
 */
ZDB_DEFINE_CORE_SLAB(g_zdb_core_slab);
ZDB_DEFINE_CURSOR_SLAB(g_zdb_cursor_slab);
ZDB_DEFINE_KV_IO_SLAB(g_zdb_kv_io_slab);
ZDB_DEFINE_TS_INGEST_SLAB(g_zdb_ts_ingest_slab);

static zdb_t g_db;
static const zdb_cfg_t g_cfg = {
	.partition_ref = NULL,
	.lfs_mount_point = CONFIG_ZDB_LFS_MOUNT_POINT,
	.kv_namespace = "default",
	.work_q = NULL,
	.scan_yield_every_n = CONFIG_ZDB_SCAN_YIELD_EVERY_N,
};

static zdb_status_t zdb_example_init(void)
{
	g_db.core_slab = &g_zdb_core_slab;
	g_db.cursor_slab = &g_zdb_cursor_slab;
	g_db.kv_io_slab = &g_zdb_kv_io_slab;
	g_db.ts_ingest_slab = &g_zdb_ts_ingest_slab;

	return zdb_init(&g_db, &g_cfg);
}

void zephyrdb_first_pass_example(void)
{
	zdb_status_t rc = zdb_example_init();
	zdb_stats_t stats;
	zdb_ts_stats_t ts_stats;
	zdb_ts_stats_export_t ts_export;
	if (rc != ZDB_OK) {
		return;
	}

#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
	/*
	 * KV requires g_cfg.partition_ref to point to an initialized struct nvs_fs.
	 * This example leaves it NULL, so KV open is intentionally skipped.
	 */
#endif

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
	zdb_ts_t ts;
	zdb_ts_sample_i64_t sample = {
		.ts_ms = k_uptime_get(),
		.value = 42,
	};

	(void)zdb_ts_open(&g_db, "metrics", &ts);
	(void)zdb_ts_append_i64(&ts, &sample);
	(void)zdb_ts_close(&ts);
#endif

	zdb_stats_get(&g_db, &stats);
	zdb_ts_stats_get(&g_db, &ts_stats);
	(void)zdb_ts_stats_export(&g_db, &ts_export);
	/* Validate export CRC before using in telemetry pipeline */
	if (zdb_ts_stats_export_validate(&ts_export) == ZDB_OK) {
		/* Safe to transmit or store */
	}
	zdb_ts_stats_reset(&g_db);
	ARG_UNUSED(stats);
	ARG_UNUSED(ts_stats);
	ARG_UNUSED(ts_export);

	(void)zdb_deinit(&g_db);
}

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
/*
 * Corruption and recovery demonstration.
 * Shows TS stats instrumentation by appending records, corrupting the stream file,
 * then recovering to observe counter deltas.
 */
void zephyrdb_recovery_demo(void)
{
	zdb_status_t rc = zdb_example_init();
	zdb_ts_stats_t before_corruption, after_recovery;
	zdb_ts_t ts;
	zdb_ts_sample_i64_t samples[3] = {
		{.ts_ms = 1000, .value = 10},
		{.ts_ms = 2000, .value = 20},
		{.ts_ms = 3000, .value = 30},
	};
	struct fs_file_t file;
	char path[256];
	off_t file_size;
	int fs_rc;

	if (rc != ZDB_OK) {
		return;
	}

	/*
	 * Phase 1: Append multiple samples and flush to disk.
	 */
	rc = zdb_ts_open(&g_db, "demo_stream", &ts);
	if (rc != ZDB_OK) {
		goto cleanup;
	}

	(void)zdb_ts_append_batch_i64(&ts, samples, 3U);
	(void)zdb_ts_flush_sync(&ts, K_FOREVER);
	(void)zdb_ts_close(&ts);

	/*
	 * Phase 2: Snapshot stats before corruption.
	 */
	zdb_ts_stats_get(&g_db, &before_corruption);

	/*
	 * Phase 3: Corrupt the stream file by truncating the last 10 bytes.
	 * This leaves an incomplete/partial record that recovery will detect and remove.
	 */
	(void)snprintf(path, sizeof(path), "%s/demo_stream.zts", CONFIG_ZDB_LFS_MOUNT_POINT);
	fs_file_t_init(&file);

	fs_rc = fs_open(&file, path, FS_O_RDWR);
	if (fs_rc == 0) {
		/* Get file size and truncate to size - 10 bytes. */
		file_size = fs_tell(&file);
		if (file_size > 10) {
			(void)fs_truncate(&file, (off_t)(file_size - 10));
		}
		(void)fs_close(&file);
	}

	/*
	 * Phase 4: Reopen stream to trigger auto-recovery.
	 * If CONFIG_ZDB_TS_AUTO_RECOVER_ON_OPEN is enabled, the open will scan,
	 * detect the truncated record, and remove it.
	 */
	rc = zdb_ts_open(&g_db, "demo_stream", &ts);
	(void)zdb_ts_close(&ts);

	/*
	 * Phase 5: Snapshot stats after recovery and show deltas.
	 */
	zdb_ts_stats_get(&g_db, &after_recovery);

	/*
	 * Deltas demonstrate instrumentation:
	 * - recover_runs should increment (recovery was invoked)
	 * - recover_truncated_bytes should show the 10 bytes we removed
	 * - corrupt_records may increment if partial record was detected
	 */
	ARG_UNUSED(before_corruption);
	ARG_UNUSED(after_recovery);

cleanup:
	(void)zdb_deinit(&g_db);
}
#endif /* CONFIG_ZDB_TS */
