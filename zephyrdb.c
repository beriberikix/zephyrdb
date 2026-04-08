#include "zephyrdb.h"

#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
#include <zephyr/fs/nvs.h>
#endif
#if defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
#include <zephyr/kvss/zms.h>
#endif
#endif

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
#include <zephyr/fs/fs.h>
#include <zephyr/sys/byteorder.h>

#if defined(CONFIG_ZDB_FLATBUFFERS) && (CONFIG_ZDB_FLATBUFFERS)
#include <flatcc/flatcc_builder.h>
#endif
#endif

#include <zephyr/sys/crc.h>

#ifndef CONFIG_ZDB_MAX_KEY_LEN
#define CONFIG_ZDB_MAX_KEY_LEN 48
#endif

#ifndef CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN
#define CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN 24
#endif

#ifndef CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_SIZE
#define CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_SIZE 64
#endif

#ifndef CONFIG_ZDB_TS_INGEST_BUFFER_BYTES
#define CONFIG_ZDB_TS_INGEST_BUFFER_BYTES 1024
#endif

#ifndef CONFIG_ZDB_TS_MAX_AGG_POINTS
#define CONFIG_ZDB_TS_MAX_AGG_POINTS 4096
#endif

#ifndef CONFIG_ZDB_TS_MAX_RECOVERY_TRUNCATE_BYTES
#define CONFIG_ZDB_TS_MAX_RECOVERY_TRUNCATE_BYTES 4096
#endif

#ifndef CONFIG_ZDB_TS_DIRNAME
#define CONFIG_ZDB_TS_DIRNAME "zdb"
#endif

#define ZDB_TS_PATH_MAX 128
#define ZDB_TS_REC_MAGIC 0x5A445442u
#define ZDB_TS_REC_VERSION 1u
#define ZDB_TS_STREAM_MAGIC 0x5A445453u
#define ZDB_TS_STREAM_VERSION 1u

#if defined(CONFIG_ZDB_STATS) && (CONFIG_ZDB_STATS)
#define ZDB_STATS_ENABLED 1
#else
#define ZDB_STATS_ENABLED 0
#endif

#define ZDB_STAT_INC(db, field)                                                                \
	do {                                                                                         \
		if (ZDB_STATS_ENABLED && ((db) != NULL)) {                                                \
			(db)->stats.field++;                                                                    \
		}                                                                                        \
	} while (0)

#define ZDB_STAT_ADD(db, field, value)                                                         \
	do {                                                                                         \
		if (ZDB_STATS_ENABLED && ((db) != NULL)) {                                                \
			(db)->stats.field += (value);                                                           \
		}                                                                                        \
	} while (0)

static zdb_status_t zdb_status_from_errno(int err)
{
	int e = (err < 0) ? -err : err;

	switch (e) {
	case 0:
		return ZDB_OK;
	case EINVAL:
		return ZDB_ERR_INVAL;
	case ENOMEM:
		return ZDB_ERR_NOMEM;
	case ENOENT:
		return ZDB_ERR_NOT_FOUND;
	case EBUSY:
		return ZDB_ERR_BUSY;
	case ETIMEDOUT:
		return ZDB_ERR_TIMEOUT;
	case ENOTSUP:
		return ZDB_ERR_UNSUPPORTED;
	default:
		return ZDB_ERR_IO;
	}
}

#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
static bool zdb_key_valid(const char *key)
{
	size_t key_len;

	if ((key == NULL) || ((*key) == '\0')) {
		return false;
	}

	key_len = strlen(key);
	return (key_len <= (size_t)CONFIG_ZDB_MAX_KEY_LEN);
}
#endif
#if (defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)) || \
	(defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS))
static uint32_t zdb_fnv1a32(const char *s)
{
	uint32_t hash = 0x811C9DC5u;

	while ((*s) != '\0') {
		hash ^= (uint8_t)(*s);
		hash *= 0x01000193u;
		s++;
	}

	return hash;
}
#endif

static zdb_status_t zdb_lock_read(zdb_t *db)
{
	int rc = k_mutex_lock(&db->lock, K_FOREVER);
	return (rc == 0) ? ZDB_OK : ZDB_ERR_BUSY;
}

static zdb_status_t zdb_lock_write(zdb_t *db)
{
	int rc = k_mutex_lock(&db->lock, K_FOREVER);
	return (rc == 0) ? ZDB_OK : ZDB_ERR_BUSY;
}

static void zdb_unlock_read(zdb_t *db)
{
	k_mutex_unlock(&db->lock);
}

static void zdb_unlock_write(zdb_t *db)
{
	k_mutex_unlock(&db->lock);
}

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
struct __packed zdb_ts_record_i64 {
	uint32_t magic_le;
	uint16_t version_le;
	uint16_t reserved_le;
	uint64_t ts_ms_le;
	uint64_t value_le;
	uint32_t crc_le;
};

BUILD_ASSERT(sizeof(struct zdb_ts_record_i64) == 28,
	     "Unexpected TS record layout size");

struct __packed zdb_ts_stream_header {
	uint32_t magic_le;
	uint16_t version_le;
	uint16_t reserved_le;
	uint32_t stream_id_le;
	uint32_t crc_le;
};

BUILD_ASSERT(sizeof(struct zdb_ts_stream_header) == 16,
	     "Unexpected TS stream header layout size");

struct zdb_ts_cursor_ctx {
	zdb_t *db;
	const char *stream_name;
	zdb_ts_window_t window;
	struct zdb_ts_record_i64 cache;
	size_t file_offset;
	size_t ram_offset;
	bool file_done;
};

struct zdb_ts_core_ctx {
	struct k_work flush_work;
	struct k_work_q *work_q;
	zdb_t *db;
	uint8_t *ingest_buf;
	size_t ingest_capacity;
	size_t ingest_used;
	const char *active_stream;
	bool flush_pending;
};

zdb_status_t zdb_ts_recover_stream(zdb_ts_t *ts, size_t *out_truncated_bytes);

static bool zdb_ts_stream_name_valid(const char *stream_name)
{
	size_t n;

	if ((stream_name == NULL) || ((*stream_name) == '\0')) {
		return false;
	}

	n = strlen(stream_name);
	return (n <= (size_t)CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN);
}

static void zdb_ts_stream_header_encode(const char *stream_name,
					struct zdb_ts_stream_header *out)
{
	uint32_t crc;

	out->magic_le = sys_cpu_to_le32(ZDB_TS_STREAM_MAGIC);
	out->version_le = sys_cpu_to_le16(ZDB_TS_STREAM_VERSION);
	out->reserved_le = 0U;
	out->stream_id_le = sys_cpu_to_le32(zdb_fnv1a32(stream_name));
	crc = crc32_ieee((const uint8_t *)out,
			 offsetof(struct zdb_ts_stream_header, crc_le));
	out->crc_le = sys_cpu_to_le32(crc);
}

static zdb_status_t zdb_ts_stream_header_decode(zdb_t *db,
						const struct zdb_ts_stream_header *hdr,
						const char *stream_name)
{
	uint32_t expect_crc;
	uint32_t got_crc;
	uint32_t stream_id;

	if ((hdr == NULL) || (stream_name == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (sys_le32_to_cpu(hdr->magic_le) != ZDB_TS_STREAM_MAGIC) {
		ZDB_STAT_INC(db, ts_corrupt_records);
		return ZDB_ERR_CORRUPT;
	}

	if (sys_le16_to_cpu(hdr->version_le) != ZDB_TS_STREAM_VERSION) {
		ZDB_STAT_INC(db, ts_unsupported_versions);
		return ZDB_ERR_UNSUPPORTED;
	}

	expect_crc = crc32_ieee((const uint8_t *)hdr,
			       offsetof(struct zdb_ts_stream_header, crc_le));
	got_crc = sys_le32_to_cpu(hdr->crc_le);
	if (got_crc != expect_crc) {
		ZDB_STAT_INC(db, ts_crc_failures);
		ZDB_STAT_INC(db, ts_corrupt_records);
		return ZDB_ERR_CORRUPT;
	}

	stream_id = sys_le32_to_cpu(hdr->stream_id_le);
	if (stream_id != zdb_fnv1a32(stream_name)) {
		ZDB_STAT_INC(db, ts_corrupt_records);
		return ZDB_ERR_CORRUPT;
	}

	return ZDB_OK;
}

static void zdb_ts_record_encode(const zdb_ts_sample_i64_t *sample, struct zdb_ts_record_i64 *out)
{
	uint32_t crc;

	out->magic_le = sys_cpu_to_le32(ZDB_TS_REC_MAGIC);
	out->version_le = sys_cpu_to_le16(ZDB_TS_REC_VERSION);
	out->reserved_le = 0U;
	out->ts_ms_le = sys_cpu_to_le64(sample->ts_ms);
	out->value_le = sys_cpu_to_le64((uint64_t)sample->value);
	crc = crc32_ieee((const uint8_t *)out, offsetof(struct zdb_ts_record_i64, crc_le));
	out->crc_le = sys_cpu_to_le32(crc);
}

static zdb_status_t zdb_ts_record_decode(zdb_t *db,
					 const struct zdb_ts_record_i64 *rec,
					 uint64_t *out_ts_ms, int64_t *out_value)
{
	uint32_t expect_crc;
	uint32_t got_crc;

	if ((rec == NULL) || (out_ts_ms == NULL) || (out_value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (sys_le32_to_cpu(rec->magic_le) != ZDB_TS_REC_MAGIC) {
		ZDB_STAT_INC(db, ts_corrupt_records);
		return ZDB_ERR_CORRUPT;
	}

	if (sys_le16_to_cpu(rec->version_le) != ZDB_TS_REC_VERSION) {
		ZDB_STAT_INC(db, ts_unsupported_versions);
		return ZDB_ERR_UNSUPPORTED;
	}

	expect_crc = crc32_ieee((const uint8_t *)rec, offsetof(struct zdb_ts_record_i64, crc_le));
	got_crc = sys_le32_to_cpu(rec->crc_le);
	if (got_crc != expect_crc) {
		ZDB_STAT_INC(db, ts_crc_failures);
		ZDB_STAT_INC(db, ts_corrupt_records);
		return ZDB_ERR_CORRUPT;
	}

	*out_ts_ms = sys_le64_to_cpu(rec->ts_ms_le);
	*out_value = (int64_t)sys_le64_to_cpu(rec->value_le);
	return ZDB_OK;
}

static int zdb_ts_build_path(const zdb_cfg_t *cfg, const char *stream_name,
			     char *path, size_t path_len)
{
	char ts_dir[ZDB_TS_PATH_MAX];
	int n;
	int rc;

	if ((cfg == NULL) || (cfg->lfs_mount_point == NULL) || (stream_name == NULL) ||
	    (path == NULL) || (path_len == 0U)) {
		return -EINVAL;
	}

	n = snprintf(ts_dir, sizeof(ts_dir), "%s/%s", cfg->lfs_mount_point, CONFIG_ZDB_TS_DIRNAME);
	if ((n < 0) || ((size_t)n >= sizeof(ts_dir))) {
		return -ENAMETOOLONG;
	}

	rc = fs_mkdir(ts_dir);
	if ((rc < 0) && (rc != -EEXIST)) {
		return rc;
	}

	n = snprintf(path, path_len, "%s/%s/%s.zts", cfg->lfs_mount_point,
		     CONFIG_ZDB_TS_DIRNAME, stream_name);
	if ((n < 0) || ((size_t)n >= path_len)) {
		return -ENAMETOOLONG;
	}

	return 0;
}

static int zdb_ts_flush_buffer_locked(struct zdb_ts_core_ctx *ctx)
{
	struct fs_file_t file;
	char path[ZDB_TS_PATH_MAX];
	ssize_t wr;
	int rc;

	if ((ctx == NULL) || (ctx->db == NULL) || (ctx->db->cfg == NULL)) {
		return -EINVAL;
	}

	if ((ctx->active_stream == NULL) || (ctx->ingest_buf == NULL)) {
		return -EINVAL;
	}

	if (ctx->ingest_used == 0U) {
		return 0;
	}

	rc = zdb_ts_build_path(ctx->db->cfg, ctx->active_stream, path, sizeof(path));
	if (rc < 0) {
		return rc;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (rc < 0) {
		return rc;
	}

	wr = fs_write(&file, ctx->ingest_buf, ctx->ingest_used);
	if ((wr < 0) || ((size_t)wr != ctx->ingest_used)) {
		(void)fs_close(&file);
		return (wr < 0) ? (int)wr : -EIO;
	}

	rc = fs_close(&file);
	if (rc < 0) {
		return rc;
	}

	ctx->ingest_used = 0U;
	return 0;
}

static bool zdb_ts_agg_update(zdb_ts_agg_t agg, double sample, uint32_t *points, double *acc)
{
	if ((*points) == 0U) {
		*acc = sample;
		(*points)++;
		return true;
	}

	switch (agg) {
	case ZDB_TS_AGG_MIN:
		if (sample < (*acc)) {
			*acc = sample;
		}
		break;
	case ZDB_TS_AGG_MAX:
		if (sample > (*acc)) {
			*acc = sample;
		}
		break;
	case ZDB_TS_AGG_AVG:
	case ZDB_TS_AGG_SUM:
		*acc += sample;
		break;
	case ZDB_TS_AGG_COUNT:
		break;
	default:
		return false;
	}

	(*points)++;
	return true;
}

static zdb_status_t zdb_ts_cursor_read_file_record(struct zdb_ts_cursor_ctx *cctx,
						    zdb_bytes_t *out_record)
{
	struct fs_file_t file;
	char path[ZDB_TS_PATH_MAX];
	ssize_t rd;
	int rc;

	if ((cctx == NULL) || (cctx->db == NULL) || (cctx->db->cfg == NULL) ||
	    (cctx->stream_name == NULL) || (out_record == NULL)) {
		return ZDB_ERR_INVAL;
	}

	rc = zdb_ts_build_path(cctx->db->cfg, cctx->stream_name, path, sizeof(path));
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc < 0) {
		if (rc == -ENOENT) {
			return ZDB_ERR_NOT_FOUND;
		}
		return zdb_status_from_errno(rc);
	}

	if (cctx->file_offset > 0U) {
		rc = fs_seek(&file, (off_t)cctx->file_offset, FS_SEEK_SET);
		if (rc < 0) {
			(void)fs_close(&file);
			return zdb_status_from_errno(rc);
		}
	}

	rd = fs_read(&file, &cctx->cache, sizeof(cctx->cache));
	(void)fs_close(&file);

	if (rd == 0) {
		return ZDB_ERR_NOT_FOUND;
	}
	if (rd < 0) {
		return zdb_status_from_errno((int)rd);
	}
	if ((size_t)rd != sizeof(cctx->cache)) {
		return ZDB_ERR_CORRUPT;
	}

	cctx->file_offset += sizeof(cctx->cache);
	out_record->data = (const uint8_t *)&cctx->cache;
	out_record->len = sizeof(cctx->cache);
	return ZDB_OK;
}

static zdb_status_t zdb_ts_ensure_stream_header(zdb_t *db, const char *stream_name)
{
	struct fs_file_t file;
	struct zdb_ts_stream_header hdr;
	char path[ZDB_TS_PATH_MAX];
	ssize_t rd;
	int rc;

	if ((db == NULL) || (db->cfg == NULL) || (stream_name == NULL)) {
		return ZDB_ERR_INVAL;
	}

	rc = zdb_ts_build_path(db->cfg, stream_name, path, sizeof(path));
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_RDWR);
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	rd = fs_read(&file, &hdr, sizeof(hdr));
	if (rd == 0) {
		ssize_t wr;

		zdb_ts_stream_header_encode(stream_name, &hdr);
		rc = fs_seek(&file, 0, FS_SEEK_SET);
		if (rc < 0) {
			(void)fs_close(&file);
			return zdb_status_from_errno(rc);
		}
		wr = fs_write(&file, &hdr, sizeof(hdr));
		if ((wr < 0) || ((size_t)wr != sizeof(hdr))) {
			(void)fs_close(&file);
			return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
		}
		(void)fs_close(&file);
		return ZDB_OK;
	}

	if (rd < 0) {
		(void)fs_close(&file);
		return zdb_status_from_errno((int)rd);
	}

	if ((size_t)rd != sizeof(hdr)) {
		(void)fs_close(&file);
		return ZDB_ERR_CORRUPT;
	}

	(void)fs_close(&file);
	return zdb_ts_stream_header_decode(db, &hdr, stream_name);
}

static bool zdb_ts_window_match(zdb_ts_window_t window, uint64_t ts_ms)
{
	if ((window.from_ts_ms == 0U) && (window.to_ts_ms == 0U)) {
		return true;
	}

	if (ts_ms < window.from_ts_ms) {
		return false;
	}

	if ((window.to_ts_ms != 0U) && (ts_ms > window.to_ts_ms)) {
		return false;
	}

	return true;
}

static bool zdb_ts_predicate_match(const zdb_cursor_t *cursor, const zdb_bytes_t *record)
{
	if (cursor->predicate == NULL) {
		return true;
	}

	return cursor->predicate(ZDB_MODEL_TS, record, cursor->predicate_ctx);
}

static void zdb_ts_flush_work_handler(struct k_work *work)
{
	struct zdb_ts_core_ctx *ctx = CONTAINER_OF(work, struct zdb_ts_core_ctx, flush_work);
	int rc;

	if ((ctx == NULL) || (ctx->db == NULL)) {
		return;
	}

	if (zdb_lock_write(ctx->db) != ZDB_OK) {
		ctx->flush_pending = false;
		return;
	}

	rc = zdb_ts_flush_buffer_locked(ctx);
	if (rc < 0) {
		/* Keep data in buffer for retry by not mutating ingest_used on error. */
	}
	ctx->flush_pending = false;
	zdb_unlock_write(ctx->db);
}

static struct zdb_ts_core_ctx *zdb_ts_ctx_get_or_alloc(zdb_t *db)
{
	struct zdb_ts_core_ctx *ctx = NULL;

	if ((db == NULL) || (db->core_slab == NULL) || (db->cfg == NULL)) {
		return NULL;
	}

	if (db->ts_ctx != NULL) {
		return (struct zdb_ts_core_ctx *)db->ts_ctx;
	}

	if (k_mem_slab_alloc(db->core_slab, (void **)&ctx, K_NO_WAIT) != 0) {
		return NULL;
	}

	(void)memset(ctx, 0, sizeof(*ctx));
	ctx->work_q = db->cfg->work_q;
	ctx->db = db;
	ctx->ingest_capacity = MIN((size_t)CONFIG_ZDB_TS_INGEST_BUFFER_BYTES,
				   (size_t)CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_SIZE);
	if ((db->ts_ingest_slab == NULL) ||
	    (k_mem_slab_alloc(db->ts_ingest_slab, (void **)&ctx->ingest_buf, K_NO_WAIT) != 0)) {
		k_mem_slab_free(db->core_slab, ctx);
		return NULL;
	}

	k_work_init(&ctx->flush_work, zdb_ts_flush_work_handler);
	db->ts_ctx = ctx;

	return ctx;
}
#endif /* CONFIG_ZDB_TS */

static bool zdb_valid_scan_yield(uint16_t n)
{
	return (n > 0U);
}

zdb_status_t zdb_init(zdb_t *db, const zdb_cfg_t *cfg)
{
	if ((db == NULL) || (cfg == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (!zdb_valid_scan_yield(cfg->scan_yield_every_n)) {
		return ZDB_ERR_INVAL;
	}

	if ((db->core_slab == NULL) || (db->cursor_slab == NULL)) {
		return ZDB_ERR_INVAL;
	}

	k_mutex_init(&db->lock);
	db->cfg = cfg;
	db->core_ctx = NULL;
	db->kv_ctx = NULL;
	db->ts_ctx = NULL;
	(void)memset(&db->stats, 0, sizeof(db->stats));

	return ZDB_OK;
}

zdb_status_t zdb_deinit(zdb_t *db)
{
	if (db == NULL) {
		return ZDB_ERR_INVAL;
	}

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
	if ((db->core_slab != NULL) && (db->ts_ctx != NULL)) {
		struct zdb_ts_core_ctx *ctx = (struct zdb_ts_core_ctx *)db->ts_ctx;

		if ((db->ts_ingest_slab != NULL) && (ctx->ingest_buf != NULL)) {
			k_mem_slab_free(db->ts_ingest_slab, ctx->ingest_buf);
		}
		k_mem_slab_free(db->core_slab, ctx);
	}
#endif

	db->cfg = NULL;
	db->core_ctx = NULL;
	db->kv_ctx = NULL;
	db->ts_ctx = NULL;

	return ZDB_OK;
}

zdb_health_t zdb_health(const zdb_t *db)
{
	if ((db == NULL) || (db->cfg == NULL)) {
		return ZDB_HEALTH_FAULT;
	}

	return ZDB_HEALTH_OK;
}

void zdb_stats_get(const zdb_t *db, zdb_stats_t *out_stats)
{
	if ((db == NULL) || (out_stats == NULL)) {
		return;
	}

	*out_stats = db->stats;
}

void zdb_stats_reset(zdb_t *db)
{
	if (db == NULL) {
		return;
	}

	(void)memset(&db->stats, 0, sizeof(db->stats));
}

void zdb_ts_stats_get(const zdb_t *db, zdb_ts_stats_t *out_stats)
{
	if ((db == NULL) || (out_stats == NULL)) {
		return;
	}

	out_stats->recover_runs = db->stats.ts_recover_runs;
	out_stats->recover_failures = db->stats.ts_recover_failures;
	out_stats->recover_truncated_bytes = db->stats.ts_recover_truncated_bytes;
	out_stats->crc_failures = db->stats.ts_crc_failures;
	out_stats->corrupt_records = db->stats.ts_corrupt_records;
	out_stats->unsupported_versions = db->stats.ts_unsupported_versions;
}

void zdb_ts_stats_reset(zdb_t *db)
{
	if (db == NULL) {
		return;
	}

	db->stats.ts_recover_runs = 0U;
	db->stats.ts_recover_failures = 0U;
	db->stats.ts_recover_truncated_bytes = 0U;
	db->stats.ts_crc_failures = 0U;
	db->stats.ts_corrupt_records = 0U;
	db->stats.ts_unsupported_versions = 0U;
}

zdb_status_t zdb_ts_stats_export(const zdb_t *db, zdb_ts_stats_export_t *out_export)
{
	uint32_t crc;

	if ((db == NULL) || (out_export == NULL)) {
		return ZDB_ERR_INVAL;
	}

	out_export->version = 1U;
	out_export->reserved = 0U;
	out_export->recover_runs = db->stats.ts_recover_runs;
	out_export->recover_failures = db->stats.ts_recover_failures;
	out_export->recover_truncated_bytes = db->stats.ts_recover_truncated_bytes;
	out_export->crc_failures = db->stats.ts_crc_failures;
	out_export->corrupt_records = db->stats.ts_corrupt_records;
	out_export->unsupported_versions = db->stats.ts_unsupported_versions;

	/* Compute CRC over all fields except the crc field itself */
	crc = crc32_ieee((const uint8_t *)out_export,
			 offsetof(zdb_ts_stats_export_t, crc));
	out_export->crc = crc;

	return ZDB_OK;
}

zdb_status_t zdb_ts_stats_export_validate(const zdb_ts_stats_export_t *export)
{
	uint32_t expect_crc;
	uint32_t got_crc;

	if (export == NULL) {
		return ZDB_ERR_INVAL;
	}

	/* Recompute CRC over all fields except the crc field itself */
	expect_crc = crc32_ieee((const uint8_t *)export,
			       offsetof(zdb_ts_stats_export_t, crc));
	got_crc = export->crc;

	if (got_crc != expect_crc) {
		return ZDB_ERR_CORRUPT;
	}

	/* Check version for forward compatibility */
	if (export->version != 1U) {
		return ZDB_ERR_UNSUPPORTED;
	}

	return ZDB_OK;
}

zdb_status_t zdb_cursor_reset(zdb_cursor_t *cursor)
{
	if (cursor == NULL) {
		return ZDB_ERR_INVAL;
	}

	cursor->flags = 0U;
	cursor->iter_count = 0U;
	cursor->position = 0U;
	cursor->current.data = NULL;
	cursor->current.len = 0U;

	return ZDB_OK;
}

zdb_status_t zdb_cursor_close(zdb_cursor_t *cursor)
{
	if (cursor == NULL) {
		return ZDB_ERR_INVAL;
	}

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
	if ((cursor->model == ZDB_MODEL_TS) && (cursor->impl != NULL)) {
		struct zdb_ts_cursor_ctx *ctx = (struct zdb_ts_cursor_ctx *)cursor->impl;

		if ((ctx->db != NULL) && (ctx->db->cursor_slab != NULL)) {
			k_mem_slab_free(ctx->db->cursor_slab, ctx);
		}
		cursor->impl = NULL;
	}
#endif

	return zdb_cursor_reset(cursor);
}

#if defined(CONFIG_ZDB_KV) && (CONFIG_ZDB_KV)
static void *zdb_kv_backend_fs_from_db(zdb_t *db)
{
	if ((db == NULL) || (db->cfg == NULL)) {
		return NULL;
	}

	/*
	 * cfg->partition_ref points at an initialized backend fs mounted by
	 * board/application startup:
	 * - struct nvs_fs when CONFIG_ZDB_KV_BACKEND_NVS=y
	 * - struct zms_fs when CONFIG_ZDB_KV_BACKEND_ZMS=y
	 */
	return (void *)db->cfg->partition_ref;
}

static uint16_t __unused zdb_fnv1a16(const char *s)
{
	uint32_t hash = 0x811C9DC5u;

	while ((*s) != '\0') {
		hash ^= (uint8_t)(*s);
		hash *= 0x01000193u;
		s++;
	}

	/* Avoid returning 0 which can be reserved by some backends. */
	hash = (hash & 0xFFFFu);
	if (hash == 0U) {
		hash = 1U;
	}

	return (uint16_t)hash;
}
static int zdb_kv_backend_write(zdb_t *db, uint32_t id, const void *value, size_t value_len)
{
#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
	struct nvs_fs *nvs = (struct nvs_fs *)zdb_kv_backend_fs_from_db(db);

	if (nvs == NULL) {
		return -EINVAL;
	}

	return nvs_write(nvs, (uint16_t)id, value, value_len);
#elif defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
	struct zms_fs *zms = (struct zms_fs *)zdb_kv_backend_fs_from_db(db);

	if (zms == NULL) {
		return -EINVAL;
	}

	return zms_write(zms, (zms_id_t)id, value, value_len);
#else
	ARG_UNUSED(db);
	ARG_UNUSED(id);
	ARG_UNUSED(value);
	ARG_UNUSED(value_len);
	return -ENOTSUP;
#endif
}

static int zdb_kv_backend_read(zdb_t *db, uint32_t id, void *out_value, size_t out_capacity)
{
#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
	struct nvs_fs *nvs = (struct nvs_fs *)zdb_kv_backend_fs_from_db(db);

	if (nvs == NULL) {
		return -EINVAL;
	}

	return nvs_read(nvs, (uint16_t)id, out_value, out_capacity);
#elif defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
	struct zms_fs *zms = (struct zms_fs *)zdb_kv_backend_fs_from_db(db);

	if (zms == NULL) {
		return -EINVAL;
	}

	return zms_read(zms, (zms_id_t)id, out_value, out_capacity);
#else
	ARG_UNUSED(db);
	ARG_UNUSED(id);
	ARG_UNUSED(out_value);
	ARG_UNUSED(out_capacity);
	return -ENOTSUP;
#endif
}

static int zdb_kv_backend_delete(zdb_t *db, uint32_t id)
{
#if defined(CONFIG_ZDB_KV_BACKEND_NVS) && (CONFIG_ZDB_KV_BACKEND_NVS)
	struct nvs_fs *nvs = (struct nvs_fs *)zdb_kv_backend_fs_from_db(db);

	if (nvs == NULL) {
		return -EINVAL;
	}

	return nvs_delete(nvs, (uint16_t)id);
#elif defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
	struct zms_fs *zms = (struct zms_fs *)zdb_kv_backend_fs_from_db(db);

	if (zms == NULL) {
		return -EINVAL;
	}

	return zms_delete(zms, (zms_id_t)id);
#else
	ARG_UNUSED(db);
	ARG_UNUSED(id);
	return -ENOTSUP;
#endif
}

zdb_status_t zdb_kv_open(zdb_t *db, const char *namespace_name, zdb_kv_t *kv)
{
	void *backend_fs;

	if ((db == NULL) || (namespace_name == NULL) || (kv == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (db->cfg == NULL) {
		return ZDB_ERR_INVAL;
	}

	backend_fs = zdb_kv_backend_fs_from_db(db);
	if (backend_fs == NULL) {
		return ZDB_ERR_INVAL;
	}

	kv->db = db;
	kv->namespace_name = namespace_name;
	return ZDB_OK;
}

zdb_status_t zdb_kv_close(zdb_kv_t *kv)
{
	if (kv == NULL) {
		return ZDB_ERR_INVAL;
	}

	kv->db = NULL;
	kv->namespace_name = NULL;
	return ZDB_OK;
}

zdb_status_t zdb_kv_set(zdb_kv_t *kv, const char *key, const void *value, size_t value_len)
{
	uint32_t id;
	int wr;
	zdb_status_t lock_rc;

	if ((kv == NULL) || (kv->db == NULL) || (value == NULL) || (value_len == 0U) ||
	    !zdb_key_valid(key)) {
		return ZDB_ERR_INVAL;
	}

	if (zdb_kv_backend_fs_from_db(kv->db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	id = zdb_kv_key_to_id(key);
	lock_rc = zdb_lock_write(kv->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	wr = zdb_kv_backend_write(kv->db, id, value, value_len);
	zdb_unlock_write(kv->db);

	if (wr < 0) {
		return zdb_status_from_errno((int)wr);
	}

	if ((size_t)wr != value_len) {
		return ZDB_ERR_IO;
	}

	return ZDB_OK;
}

zdb_status_t zdb_kv_get(zdb_kv_t *kv, const char *key, void *out_value,
			size_t out_capacity, size_t *out_len)
{
	uint32_t id;
	int rd;
	zdb_status_t lock_rc;

	if ((kv == NULL) || (kv->db == NULL) || (out_len == NULL) || !zdb_key_valid(key)) {
		return ZDB_ERR_INVAL;
	}

	if ((out_value == NULL) && (out_capacity > 0U)) {
		return ZDB_ERR_INVAL;
	}

	if (zdb_kv_backend_fs_from_db(kv->db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	id = zdb_kv_key_to_id(key);
	lock_rc = zdb_lock_read(kv->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	rd = zdb_kv_backend_read(kv->db, id, out_value, out_capacity);
	zdb_unlock_read(kv->db);

	if (rd < 0) {
		*out_len = 0U;
		return zdb_status_from_errno((int)rd);
	}

	*out_len = (size_t)rd;
	return ZDB_OK;
}

zdb_status_t zdb_kv_delete(zdb_kv_t *kv, const char *key)
{
	uint32_t id;
	int rc;
	zdb_status_t lock_rc;

	if ((kv == NULL) || (kv->db == NULL) || !zdb_key_valid(key)) {
		return ZDB_ERR_INVAL;
	}

	if (zdb_kv_backend_fs_from_db(kv->db) == NULL) {
		return ZDB_ERR_INVAL;
	}

	id = zdb_kv_key_to_id(key);
	lock_rc = zdb_lock_write(kv->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	rc = zdb_kv_backend_delete(kv->db, id);
	zdb_unlock_write(kv->db);

	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	return ZDB_OK;
}

uint32_t zdb_kv_key_to_id(const char *key)
{
	uint32_t id;

	if (key == NULL) {
		return 1U;
	}

#if defined(CONFIG_ZDB_KV_BACKEND_ZMS) && (CONFIG_ZDB_KV_BACKEND_ZMS)
	id = zdb_fnv1a32(key);
#else
	id = (uint32_t)zdb_fnv1a16(key);
#endif

	if (id == 0U) {
		id = 1U;
	}

	return id;
}
#endif /* CONFIG_ZDB_KV */

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
zdb_status_t zdb_ts_open(zdb_t *db, const char *stream_name, zdb_ts_t *ts)
{
	struct zdb_ts_core_ctx *ctx;
	zdb_status_t rc;
	size_t truncated = 0U;

	if ((db == NULL) || (stream_name == NULL) || (ts == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (!zdb_ts_stream_name_valid(stream_name)) {
		return ZDB_ERR_INVAL;
	}

	if (db->cfg == NULL) {
		return ZDB_ERR_INVAL;
	}

	ctx = zdb_ts_ctx_get_or_alloc(db);
	if (ctx == NULL) {
		return ZDB_ERR_NOMEM;
	}

	rc = zdb_ts_ensure_stream_header(db, stream_name);
	if (rc != ZDB_OK) {
		return rc;
	}

	if ((ctx->active_stream != NULL) && (strcmp(ctx->active_stream, stream_name) != 0)) {
		return ZDB_ERR_BUSY;
	}

	ts->db = db;
	ts->stream_name = stream_name;
	ctx->active_stream = stream_name;

#if defined(CONFIG_ZDB_TS_AUTO_RECOVER_ON_OPEN) && (CONFIG_ZDB_TS_AUTO_RECOVER_ON_OPEN)
	rc = zdb_ts_recover_stream(ts, &truncated);
	if (rc != ZDB_OK) {
		ZDB_STAT_INC(db, ts_recover_failures);
		return rc;
	}
	if ((CONFIG_ZDB_TS_MAX_RECOVERY_TRUNCATE_BYTES > 0) &&
	    (truncated > (size_t)CONFIG_ZDB_TS_MAX_RECOVERY_TRUNCATE_BYTES)) {
		ZDB_STAT_INC(db, ts_recover_failures);
		return ZDB_ERR_CORRUPT;
	}
#endif

	return ZDB_OK;
}

zdb_status_t zdb_ts_close(zdb_ts_t *ts)
{
	if (ts == NULL) {
		return ZDB_ERR_INVAL;
	}

	ts->db = NULL;
	ts->stream_name = NULL;
	return ZDB_OK;
}

zdb_status_t zdb_ts_append_i64(zdb_ts_t *ts, const zdb_ts_sample_i64_t *sample)
{
	struct zdb_ts_core_ctx *ctx;
	struct zdb_ts_record_i64 rec;
	zdb_status_t lock_rc;
	int rc;
	bool need_async_flush = false;

	if ((ts == NULL) || (ts->db == NULL) || (sample == NULL)) {
		return ZDB_ERR_INVAL;
	}

	ctx = zdb_ts_ctx_get_or_alloc(ts->db);
	if ((ctx == NULL) || (ctx->active_stream == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (strcmp(ctx->active_stream, ts->stream_name) != 0) {
		return ZDB_ERR_BUSY;
	}

	lock_rc = zdb_lock_write(ts->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	if ((ctx->ingest_capacity < sizeof(rec)) || (ctx->ingest_buf == NULL)) {
		zdb_unlock_write(ts->db);
		return ZDB_ERR_NOMEM;
	}

	if ((ctx->ingest_used + sizeof(rec)) > ctx->ingest_capacity) {
		rc = zdb_ts_flush_buffer_locked(ctx);
		if (rc < 0) {
			zdb_unlock_write(ts->db);
			return zdb_status_from_errno(rc);
		}
	}

	zdb_ts_record_encode(sample, &rec);
	(void)memcpy(&ctx->ingest_buf[ctx->ingest_used], &rec, sizeof(rec));
	ctx->ingest_used += sizeof(rec);
	need_async_flush = (ctx->ingest_used == ctx->ingest_capacity);
	zdb_unlock_write(ts->db);

	if (need_async_flush) {
		return zdb_ts_flush_async(ts);
	}

	return ZDB_OK;
}

zdb_status_t zdb_ts_append_batch_i64(zdb_ts_t *ts, const zdb_ts_sample_i64_t *samples,
			      size_t sample_count)
{
	size_t i;
	zdb_status_t rc;

	if ((ts == NULL) || (samples == NULL) || (sample_count == 0U)) {
		return ZDB_ERR_INVAL;
	}

	for (i = 0U; i < sample_count; i++) {
		rc = zdb_ts_append_i64(ts, &samples[i]);
		if (rc != ZDB_OK) {
			return rc;
		}
	}

	return ZDB_OK;
}

zdb_status_t zdb_ts_sample_i64_export_flatbuffer(const zdb_ts_sample_i64_t *sample,
						  uint8_t *out_buf,
						  size_t out_capacity,
						  size_t *out_len)
{
	if (sample == NULL) {
		return ZDB_ERR_INVAL;
	}

	if ((out_buf == NULL) && (out_capacity > 0U)) {
		return ZDB_ERR_INVAL;
	}

#if defined(CONFIG_ZDB_FLATBUFFERS) && (CONFIG_ZDB_FLATBUFFERS) && \
	defined(CONFIG_FLATCC) && (CONFIG_FLATCC)
	struct {
		uint64_t ts_ms;
		uint64_t value;
	} payload;
	flatcc_builder_t builder;
	flatcc_builder_ref_t root;
	flatcc_builder_ref_t buf_ref;
	void *direct_buf;
	size_t direct_size = 0U;
	int rc;

	payload.ts_ms = sys_cpu_to_le64(sample->ts_ms);
	payload.value = sys_cpu_to_le64((uint64_t)sample->value);

	rc = flatcc_builder_init(&builder);
	if (rc != 0) {
		return ZDB_ERR_NOMEM;
	}

	rc = flatcc_builder_start_buffer(&builder, 0, 0, 0);
	if (rc != 0) {
		flatcc_builder_clear(&builder);
		return ZDB_ERR_IO;
	}

	root = flatcc_builder_create_struct(&builder, &payload, sizeof(payload), sizeof(uint64_t));
	if (root == 0) {
		flatcc_builder_clear(&builder);
		return ZDB_ERR_NOMEM;
	}

	buf_ref = flatcc_builder_end_buffer(&builder, root);
	if (buf_ref == 0) {
		flatcc_builder_clear(&builder);
		return ZDB_ERR_IO;
	}

	direct_buf = flatcc_builder_get_direct_buffer(&builder, &direct_size);
	if ((direct_buf == NULL) || (direct_size == 0U)) {
		flatcc_builder_clear(&builder);
		return ZDB_ERR_IO;
	}

	if (out_buf == NULL) {
		if (out_len == NULL) {
			flatcc_builder_clear(&builder);
			return ZDB_ERR_INVAL;
		}

		*out_len = direct_size;
		flatcc_builder_clear(&builder);
		return ZDB_OK;
	}

	if (out_len != NULL) {
		*out_len = direct_size;
	}
	if (out_capacity < direct_size) {
		flatcc_builder_clear(&builder);
		return ZDB_ERR_NOMEM;
	}

	(void)memcpy(out_buf, direct_buf, direct_size);
	flatcc_builder_clear(&builder);
	return ZDB_OK;
#else
	ARG_UNUSED(out_buf);
	ARG_UNUSED(out_capacity);
	if (out_len != NULL) {
		*out_len = 0U;
	}
	return ZDB_ERR_UNSUPPORTED;
#endif
}

zdb_status_t zdb_ts_flush_async(zdb_ts_t *ts)
{
	struct zdb_ts_core_ctx *ctx;
	zdb_status_t lock_rc;
	int rc;

	if ((ts == NULL) || (ts->db == NULL) || (ts->db->cfg == NULL)) {
		return ZDB_ERR_INVAL;
	}

	ctx = zdb_ts_ctx_get_or_alloc(ts->db);
	if ((ctx == NULL) || (ctx->work_q == NULL)) {
		return ZDB_ERR_UNSUPPORTED;
	}

	lock_rc = zdb_lock_write(ts->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	if (ctx->flush_pending) {
		zdb_unlock_write(ts->db);
		return ZDB_ERR_BUSY;
	}

	ctx->flush_pending = true;
	rc = k_work_submit_to_queue(ctx->work_q, &ctx->flush_work);
	zdb_unlock_write(ts->db);

	if (rc < 0) {
		ctx->flush_pending = false;
		return zdb_status_from_errno(rc);
	}

	return ZDB_OK;
}

zdb_status_t zdb_ts_flush_sync(zdb_ts_t *ts, k_timeout_t timeout)
{
	int64_t deadline;
	zdb_status_t rc;
	struct zdb_ts_core_ctx *ctx;

	if ((ts == NULL) || (ts->db == NULL)) {
		return ZDB_ERR_INVAL;
	}

	rc = zdb_ts_flush_async(ts);
	if ((rc != ZDB_OK) && (rc != ZDB_ERR_BUSY)) {
		return rc;
	}

	ctx = zdb_ts_ctx_get_or_alloc(ts->db);
	if (ctx == NULL) {
		return ZDB_ERR_INTERNAL;
	}

	if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		return ctx->flush_pending ? ZDB_ERR_BUSY : ZDB_OK;
	}

	if (K_TIMEOUT_EQ(timeout, K_FOREVER)) {
		while (ctx->flush_pending) {
			k_yield();
		}
		return ZDB_OK;
	}

	deadline = k_uptime_get() + k_ticks_to_ms_floor64(timeout.ticks);
	while (ctx->flush_pending) {
		if (k_uptime_get() >= deadline) {
			return ZDB_ERR_TIMEOUT;
		}
		k_yield();
	}

	return ZDB_OK;
}

zdb_status_t zdb_ts_query_aggregate(zdb_ts_t *ts, zdb_ts_window_t window,
			    zdb_ts_agg_t agg, zdb_ts_agg_result_t *out_result)
{
	struct zdb_ts_core_ctx *ctx;
	struct fs_file_t file;
	struct zdb_ts_stream_header hdr;
	char path[ZDB_TS_PATH_MAX];
	struct zdb_ts_record_i64 rec;
	size_t off;
	uint32_t points = 0U;
	double acc = 0.0;
	zdb_status_t lock_rc;
	int rc;
	ssize_t rd;

	if ((ts == NULL) || (ts->db == NULL) || (out_result == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (agg > ZDB_TS_AGG_COUNT) {
		return ZDB_ERR_INVAL;
	}

	ctx = zdb_ts_ctx_get_or_alloc(ts->db);
	if ((ctx == NULL) || (ctx->ingest_buf == NULL)) {
		return ZDB_ERR_INVAL;
	}

	{
		zdb_status_t rc_hdr = zdb_ts_ensure_stream_header(ts->db, ts->stream_name);
		if (rc_hdr != ZDB_OK) {
			return rc_hdr;
		}
	}

	rc = zdb_ts_build_path(ts->db->cfg, ts->stream_name, path, sizeof(path));
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if ((rc < 0) && (rc != -ENOENT)) {
		return zdb_status_from_errno(rc);
	}

	if (rc == 0) {
		rd = fs_read(&file, &hdr, sizeof(hdr));
		if ((rd < 0) || ((size_t)rd != sizeof(hdr))) {
			(void)fs_close(&file);
			return (rd < 0) ? zdb_status_from_errno((int)rd) : ZDB_ERR_CORRUPT;
		}
		{
			zdb_status_t dec = zdb_ts_stream_header_decode(ts->db, &hdr, ts->stream_name);
			if (dec != ZDB_OK) {
				(void)fs_close(&file);
				return dec;
			}
		}

		while (points < CONFIG_ZDB_TS_MAX_AGG_POINTS) {
			uint64_t ts_ms;
			int64_t val;
			zdb_status_t dec_rc;

			rd = fs_read(&file, &rec, sizeof(rec));
			if (rd == 0) {
				break;
			}
			if (rd < 0) {
				(void)fs_close(&file);
				return zdb_status_from_errno((int)rd);
			}
			if ((size_t)rd != sizeof(rec)) {
				(void)fs_close(&file);
				return ZDB_ERR_CORRUPT;
			}

			dec_rc = zdb_ts_record_decode(ts->db, &rec, &ts_ms, &val);
			if (dec_rc == ZDB_ERR_UNSUPPORTED) {
				continue;
			}
			if (dec_rc != ZDB_OK) {
				(void)fs_close(&file);
				return dec_rc;
			}
			if (!zdb_ts_window_match(window, ts_ms)) {
				continue;
			}

			if (!zdb_ts_agg_update(agg, (double)val, &points, &acc)) {
				(void)fs_close(&file);
				return ZDB_ERR_INVAL;
			}
		}

		(void)fs_close(&file);
	}

	lock_rc = zdb_lock_read(ts->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	for (off = 0U; (off + sizeof(rec)) <= ctx->ingest_used; off += sizeof(rec)) {
		uint64_t ts_ms;
		int64_t val;
		zdb_status_t dec_rc;

		if (points >= CONFIG_ZDB_TS_MAX_AGG_POINTS) {
			break;
		}

		(void)memcpy(&rec, &ctx->ingest_buf[off], sizeof(rec));
		dec_rc = zdb_ts_record_decode(ts->db, &rec, &ts_ms, &val);
		if (dec_rc == ZDB_ERR_UNSUPPORTED) {
			continue;
		}
		if (dec_rc != ZDB_OK) {
			zdb_unlock_read(ts->db);
			return dec_rc;
		}
		if (!zdb_ts_window_match(window, ts_ms)) {
			continue;
		}

		if (!zdb_ts_agg_update(agg, (double)val, &points, &acc)) {
			zdb_unlock_read(ts->db);
			return ZDB_ERR_INVAL;
		}
	}

	zdb_unlock_read(ts->db);

	if (points == 0U) {
		return ZDB_ERR_NOT_FOUND;
	}

	out_result->agg = agg;
	out_result->points = points;
	if (agg == ZDB_TS_AGG_COUNT) {
		out_result->value = (double)points;
	} else if (agg == ZDB_TS_AGG_AVG) {
		out_result->value = acc / (double)points;
	} else {
		out_result->value = acc;
	}

	return ZDB_OK;
}

zdb_status_t zdb_ts_cursor_open(zdb_ts_t *ts, zdb_ts_window_t window,
			zdb_predicate_fn predicate, void *predicate_ctx,
			zdb_cursor_t *out_cursor)
{
	struct zdb_ts_cursor_ctx *ctx = NULL;
	zdb_status_t rc;

	if ((ts == NULL) || (ts->db == NULL) || (out_cursor == NULL)) {
		return ZDB_ERR_INVAL;
	}

	rc = zdb_ts_ensure_stream_header(ts->db, ts->stream_name);
	if (rc != ZDB_OK) {
		return rc;
	}

	if (k_mem_slab_alloc(ts->db->cursor_slab, (void **)&ctx, K_NO_WAIT) != 0) {
		return ZDB_ERR_NOMEM;
	}

	ctx->db = ts->db;
	ctx->stream_name = ts->stream_name;
	ctx->window = window;
	ctx->file_offset = sizeof(struct zdb_ts_stream_header);
	ctx->ram_offset = 0U;
	ctx->file_done = false;

	out_cursor->model = ZDB_MODEL_TS;
	out_cursor->backend = ZDB_BACKEND_LFS;
	out_cursor->predicate = predicate;
	out_cursor->predicate_ctx = predicate_ctx;
	out_cursor->impl = ctx;
	return zdb_cursor_reset(out_cursor);
}

zdb_status_t zdb_cursor_next(zdb_cursor_t *cursor, zdb_bytes_t *out_record)
{
	struct zdb_ts_cursor_ctx *cctx;
	struct zdb_ts_core_ctx *tctx;
	zdb_bytes_t candidate;
	struct zdb_ts_record_i64 rec;
	zdb_status_t lock_rc;

	if ((cursor == NULL) || (out_record == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if ((cursor->model != ZDB_MODEL_TS) || (cursor->impl == NULL)) {
		return ZDB_ERR_UNSUPPORTED;
	}

	cctx = (struct zdb_ts_cursor_ctx *)cursor->impl;

	while (!cctx->file_done) {
		uint64_t ts_ms;
		int64_t val;
		zdb_status_t dec_rc;
		zdb_status_t rc = zdb_ts_cursor_read_file_record(cctx, &candidate);

		if (rc == ZDB_ERR_NOT_FOUND) {
			cctx->file_done = true;
			break;
		}
		if (rc != ZDB_OK) {
			return rc;
		}

		(void)memcpy(&rec, candidate.data, sizeof(rec));
		dec_rc = zdb_ts_record_decode(cctx->db, &rec, &ts_ms, &val);
		if (dec_rc == ZDB_ERR_UNSUPPORTED) {
			continue;
		}
		if (dec_rc != ZDB_OK) {
			return dec_rc;
		}
		ARG_UNUSED(val);
		cursor->iter_count++;

		if ((CONFIG_ZDB_SCAN_YIELD_EVERY_N > 0) &&
		    ((cursor->iter_count % CONFIG_ZDB_SCAN_YIELD_EVERY_N) == 0U)) {
			k_yield();
		}

		if (!zdb_ts_window_match(cctx->window, ts_ms)) {
			continue;
		}

		if (!zdb_ts_predicate_match(cursor, &candidate)) {
			continue;
		}

		*out_record = candidate;
		cursor->current = candidate;
		return ZDB_OK;
	}

	tctx = zdb_ts_ctx_get_or_alloc(cctx->db);
	if ((tctx == NULL) || (tctx->ingest_buf == NULL)) {
		return ZDB_ERR_INVAL;
	}
	if ((tctx->active_stream != NULL) && (strcmp(tctx->active_stream, cctx->stream_name) != 0)) {
		return ZDB_ERR_BUSY;
	}

	lock_rc = zdb_lock_read(cctx->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	while ((cctx->ram_offset + sizeof(rec)) <= tctx->ingest_used) {
		uint64_t ts_ms;
		int64_t val;
		zdb_status_t dec_rc;

		candidate.data = &tctx->ingest_buf[cctx->ram_offset];
		candidate.len = sizeof(rec);
		(void)memcpy(&rec, candidate.data, sizeof(rec));
		dec_rc = zdb_ts_record_decode(cctx->db, &rec, &ts_ms, &val);
		if (dec_rc == ZDB_ERR_UNSUPPORTED) {
			cctx->ram_offset += sizeof(rec);
			continue;
		}
		if (dec_rc != ZDB_OK) {
			zdb_unlock_read(cctx->db);
			return dec_rc;
		}
		ARG_UNUSED(val);
		cctx->ram_offset += sizeof(rec);
		cursor->iter_count++;

		if ((CONFIG_ZDB_SCAN_YIELD_EVERY_N > 0) &&
		    ((cursor->iter_count % CONFIG_ZDB_SCAN_YIELD_EVERY_N) == 0U)) {
			k_yield();
		}

		if (!zdb_ts_window_match(cctx->window, ts_ms)) {
			continue;
		}

		if (!zdb_ts_predicate_match(cursor, &candidate)) {
			continue;
		}

		*out_record = candidate;
		cursor->current = candidate;
		zdb_unlock_read(cctx->db);
		return ZDB_OK;
	}

	zdb_unlock_read(cctx->db);

	out_record->data = NULL;
	out_record->len = 0U;
	return ZDB_ERR_NOT_FOUND;
}

zdb_status_t zdb_ts_recover_stream(zdb_ts_t *ts, size_t *out_truncated_bytes)
{
	struct fs_file_t file;
	struct zdb_ts_stream_header hdr;
	struct zdb_ts_record_i64 rec;
	char path[ZDB_TS_PATH_MAX];
	size_t good_end;
	int rc;
	ssize_t rd;

	if ((ts == NULL) || (ts->db == NULL) || (ts->stream_name == NULL)) {
		return ZDB_ERR_INVAL;
	}

	ZDB_STAT_INC(ts->db, ts_recover_runs);

	if (out_truncated_bytes != NULL) {
		*out_truncated_bytes = 0U;
	}

	rc = zdb_ts_build_path(ts->db->cfg, ts->stream_name, path, sizeof(path));
	if (rc < 0) {
		ZDB_STAT_INC(ts->db, ts_recover_failures);
		return zdb_status_from_errno(rc);
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_RDWR);
	if (rc < 0) {
		ZDB_STAT_INC(ts->db, ts_recover_failures);
		return zdb_status_from_errno(rc);
	}

	rd = fs_read(&file, &hdr, sizeof(hdr));
	if (rd == 0) {
		zdb_ts_stream_header_encode(ts->stream_name, &hdr);
		rc = fs_seek(&file, 0, FS_SEEK_SET);
		if (rc < 0) {
			(void)fs_close(&file);
			ZDB_STAT_INC(ts->db, ts_recover_failures);
			return zdb_status_from_errno(rc);
		}
		rd = fs_write(&file, &hdr, sizeof(hdr));
		(void)fs_close(&file);
		if ((rd < 0) || ((size_t)rd != sizeof(hdr))) {
			ZDB_STAT_INC(ts->db, ts_recover_failures);
			return (rd < 0) ? zdb_status_from_errno((int)rd) : ZDB_ERR_IO;
		}
		return ZDB_OK;
	}

	if ((rd < 0) || ((size_t)rd != sizeof(hdr))) {
		(void)fs_close(&file);
		ZDB_STAT_INC(ts->db, ts_recover_failures);
		return (rd < 0) ? zdb_status_from_errno((int)rd) : ZDB_ERR_CORRUPT;
	}

	{
		zdb_status_t dec = zdb_ts_stream_header_decode(ts->db, &hdr, ts->stream_name);
		if (dec != ZDB_OK) {
			(void)fs_close(&file);
			ZDB_STAT_INC(ts->db, ts_recover_failures);
			return dec;
		}
	}

	good_end = sizeof(struct zdb_ts_stream_header);
	while (true) {
		uint64_t ts_ms;
		int64_t val;
		zdb_status_t dec_rc;

		rd = fs_read(&file, &rec, sizeof(rec));
		if (rd == 0) {
			break;
		}
		if (rd < 0) {
			(void)fs_close(&file);
			ZDB_STAT_INC(ts->db, ts_recover_failures);
			return zdb_status_from_errno((int)rd);
		}
		if ((size_t)rd != sizeof(rec)) {
			break;
		}

		dec_rc = zdb_ts_record_decode(ts->db, &rec, &ts_ms, &val);
		if (dec_rc != ZDB_OK) {
			break;
		}

		good_end += sizeof(rec);
	}

	{
		off_t end_pos = fs_tell(&file);
		if (end_pos < 0) {
			(void)fs_close(&file);
			ZDB_STAT_INC(ts->db, ts_recover_failures);
			return zdb_status_from_errno((int)end_pos);
		}

		if ((size_t)end_pos > good_end) {
			rc = fs_truncate(&file, (off_t)good_end);
			if (rc < 0) {
				(void)fs_close(&file);
				ZDB_STAT_INC(ts->db, ts_recover_failures);
				return zdb_status_from_errno(rc);
			}

			if (out_truncated_bytes != NULL) {
				*out_truncated_bytes = (size_t)end_pos - good_end;
			}
			ZDB_STAT_ADD(ts->db, ts_recover_truncated_bytes, (uint64_t)((size_t)end_pos - good_end));
		}
	}

	(void)fs_close(&file);
	return ZDB_OK;
}

/*
 * Stage 3: Document model implementation
 */
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)

struct zdb_doc_hdr_v1 {
	uint32_t magic_le;
	uint16_t version_le;
	uint16_t field_count_le;
	uint64_t created_ms_le;
	uint64_t updated_ms_le;
	uint32_t crc_le;
} __packed;

struct zdb_doc_field_hdr_v1 {
	uint16_t name_len_le;
	uint16_t reserved_le;
	uint8_t type;
	uint8_t reserved[3];
} __packed;

#define ZDB_DOC_PATH_MAX 160
#define ZDB_DOC_DIRNAME "zdb_docs"
#define ZDB_DOC_FILE_EXT ".zdoc"
#define ZDB_DOC_MAGIC 0x5A444F43u
#define ZDB_DOC_VERSION 1u

static int zdb_doc_build_root_path(const zdb_cfg_t *cfg, char *path, size_t path_len);
static int zdb_doc_build_collection_path(const zdb_cfg_t *cfg, const char *collection, char *path,
						 size_t path_len);
static int zdb_doc_build_doc_path(const zdb_cfg_t *cfg, const char *collection,
					  const char *document_id, char *path, size_t path_len);
static int zdb_doc_ensure_dirs(const zdb_cfg_t *cfg, const char *collection);

static void *zdb_alloc_copy(const void *src, size_t len)
{
	void *dst;

	if ((src == NULL) || (len == 0U)) {
		return NULL;
	}

	dst = k_malloc(len);
	if (dst == NULL) {
		return NULL;
	}

	(void)memcpy(dst, src, len);
	return dst;
}

static zdb_status_t zdb_fs_read_exact(struct fs_file_t *file, void *buf, size_t len)
{
	ssize_t rd;

	rd = fs_read(file, buf, len);
	if (rd < 0) {
		return zdb_status_from_errno((int)rd);
	}

	if (rd != (ssize_t)len) {
		return ZDB_ERR_CORRUPT;
	}

	return ZDB_OK;
}

static char *zdb_strdup_local(const char *s)
{
	size_t n;
	char *dst;

	if (s == NULL) {
		return NULL;
	}

	n = strlen(s) + 1U;
	dst = zdb_alloc_copy(s, n);
	return dst;
}

/**
 * @brief Free allocated metadata from a query result entry.
 *
 * @param metadata Metadata entry to free
 */
static inline void zdb_doc_metadata_entry_free(zdb_doc_metadata_t *metadata)
{
	if (metadata == NULL) {
		return;
	}
	if (metadata->collection_name != NULL) {
		k_free((void *)metadata->collection_name);
		metadata->collection_name = NULL;
	}
	if (metadata->document_id != NULL) {
		k_free((void *)metadata->document_id);
		metadata->document_id = NULL;
	}
}

static zdb_status_t zdb_doc_open_impl(zdb_t *db, const char *collection_name,
				      const char *document_id, zdb_doc_t *out_doc,
				      bool take_read_lock)
{
	char path[ZDB_DOC_PATH_MAX];
	struct fs_file_t file;
	struct zdb_doc_hdr_v1 hdr;
	struct zdb_doc_field_hdr_v1 field_hdr;
	zdb_status_t lock_rc = ZDB_OK;
	uint64_t saved_created_ms, saved_updated_ms;
	int rc = 0;
	size_t i;
	bool lock_held = false;
	bool file_open = false;
	zdb_status_t ret = ZDB_OK;

	rc = zdb_doc_create(db, collection_name, document_id, out_doc);
	if (rc != ZDB_OK) {
		return rc;
	}

	if (take_read_lock && (db != NULL)) {
		lock_rc = zdb_lock_read(db);
		if (lock_rc != ZDB_OK) {
			(void)zdb_doc_close(out_doc);
			return lock_rc;
		}
		lock_held = true;
	}

	rc = zdb_doc_build_doc_path(db->cfg, collection_name, document_id, path, sizeof(path));
	if (rc < 0) {
		ret = zdb_status_from_errno(rc);
		goto cleanup;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc < 0) {
		ret = zdb_status_from_errno(rc);
		goto cleanup;
	}
	file_open = true;

	ret = zdb_fs_read_exact(&file, &hdr, sizeof(hdr));
	if (ret != ZDB_OK) {
		goto cleanup;
	}

	if ((sys_le32_to_cpu(hdr.magic_le) != ZDB_DOC_MAGIC) ||
	    (sys_le16_to_cpu(hdr.version_le) != ZDB_DOC_VERSION)) {
		ret = ZDB_ERR_CORRUPT;
		goto cleanup;
	}

	saved_created_ms = sys_le64_to_cpu(hdr.created_ms_le);
	saved_updated_ms = sys_le64_to_cpu(hdr.updated_ms_le);
	if ((size_t)sys_le16_to_cpu(hdr.field_count_le) > out_doc->max_fields) {
		ret = ZDB_ERR_NOMEM;
		goto cleanup;
	}

	for (i = 0U; i < (size_t)sys_le16_to_cpu(hdr.field_count_le); i++) {
		char *name = NULL;
		uint16_t name_len;
		zdb_doc_field_type_t type;

		ret = zdb_fs_read_exact(&file, &field_hdr, sizeof(field_hdr));
		if (ret != ZDB_OK) {
			goto cleanup;
		}

		name_len = sys_le16_to_cpu(field_hdr.name_len_le);
		type = (zdb_doc_field_type_t)field_hdr.type;
		if ((name_len == 0U) || (name_len > CONFIG_ZDB_DOC_MAX_FIELD_NAME_LEN)) {
			ret = ZDB_ERR_CORRUPT;
			goto cleanup;
		}

		name = k_malloc(name_len + 1U);
		if (name == NULL) {
			ret = ZDB_ERR_NOMEM;
			goto cleanup;
		}

		ret = zdb_fs_read_exact(&file, name, name_len);
		if (ret != ZDB_OK) {
			k_free(name);
			goto cleanup;
		}
		name[name_len] = '\0';

		switch (type) {
		case ZDB_DOC_FIELD_INT64: {
			uint64_t tmp_le;
			int64_t tmp;
			ret = zdb_fs_read_exact(&file, &tmp_le, sizeof(tmp_le));
			if (ret != ZDB_OK) {
				k_free(name);
				goto cleanup;
			}
			tmp = (int64_t)sys_le64_to_cpu(tmp_le);
			rc = zdb_doc_field_set_i64(out_doc, name, tmp);
			break;
		}
		case ZDB_DOC_FIELD_DOUBLE: {
			uint64_t tmp_le;
			double tmp;
			ret = zdb_fs_read_exact(&file, &tmp_le, sizeof(tmp_le));
			if (ret != ZDB_OK) {
				k_free(name);
				goto cleanup;
			}
			tmp_le = sys_le64_to_cpu(tmp_le);
			(void)memcpy(&tmp, &tmp_le, sizeof(tmp));
			rc = zdb_doc_field_set_f64(out_doc, name, tmp);
			break;
		}
		case ZDB_DOC_FIELD_BOOL: {
			uint8_t tmp;
			ret = zdb_fs_read_exact(&file, &tmp, sizeof(tmp));
			if (ret != ZDB_OK) {
				k_free(name);
				goto cleanup;
			}
			rc = zdb_doc_field_set_bool(out_doc, name, (tmp != 0U));
			break;
		}
		case ZDB_DOC_FIELD_STRING: {
			uint32_t str_len_le;
			size_t str_len;
			char *str;
			ret = zdb_fs_read_exact(&file, &str_len_le, sizeof(str_len_le));
			if (ret != ZDB_OK) {
				k_free(name);
				goto cleanup;
			}
			str_len = sys_le32_to_cpu(str_len_le);
			if (str_len > CONFIG_ZDB_DOC_MAX_STRING_LEN) {
				k_free(name);
				ret = ZDB_ERR_CORRUPT;
				goto cleanup;
			}
			str = k_malloc(str_len + 1U);
			if (str == NULL) {
				k_free(name);
				ret = ZDB_ERR_NOMEM;
				goto cleanup;
			}
			ret = zdb_fs_read_exact(&file, str, str_len);
			if (ret != ZDB_OK) {
				k_free(str);
				k_free(name);
				goto cleanup;
			}
			str[str_len] = '\0';
			rc = zdb_doc_field_set_string(out_doc, name, str);
			k_free(str);
			break;
		}
		case ZDB_DOC_FIELD_BYTES: {
			uint32_t bytes_len_le;
			size_t bytes_len;
			void *buf;
			ret = zdb_fs_read_exact(&file, &bytes_len_le, sizeof(bytes_len_le));
			if (ret != ZDB_OK) {
				k_free(name);
				goto cleanup;
			}
			bytes_len = sys_le32_to_cpu(bytes_len_le);
			if (bytes_len > CONFIG_ZDB_DOC_MAX_BYTES_LEN) {
				k_free(name);
				ret = ZDB_ERR_CORRUPT;
				goto cleanup;
			}
			buf = NULL;
			if (bytes_len > 0U) {
				buf = k_malloc(bytes_len);
				if (buf == NULL) {
					k_free(name);
					ret = ZDB_ERR_NOMEM;
					goto cleanup;
				}
				ret = zdb_fs_read_exact(&file, buf, bytes_len);
				if (ret != ZDB_OK) {
					k_free(buf);
					k_free(name);
					goto cleanup;
				}
			}
			rc = zdb_doc_field_set_bytes(out_doc, name, buf, bytes_len);
			k_free(buf);
			break;
		}
		default:
			rc = ZDB_ERR_UNSUPPORTED;
			break;
		}

		k_free(name);
		if (rc != ZDB_OK) {
			ret = rc;
			goto cleanup;
		}
	}

cleanup:
	if (file_open) {
		(void)fs_close(&file);
	}

	if ((ret == ZDB_OK) && (out_doc != NULL)) {
		out_doc->created_ms = saved_created_ms;
		out_doc->updated_ms = saved_updated_ms;
	} else if (out_doc != NULL) {
		(void)zdb_doc_close(out_doc);
	}

	if (lock_held && (db != NULL)) {
		zdb_unlock_read(db);
	}

	return ret;
}

static void zdb_doc_field_payload_free(zdb_doc_field_t *field)
{
	if (field == NULL) {
		return;
	}

	if ((field->type == ZDB_DOC_FIELD_STRING) && (field->value.str != NULL)) {
		k_free((void *)field->value.str);
		field->value.str = NULL;
	}

	if ((field->type == ZDB_DOC_FIELD_BYTES) && (field->value.bytes.data != NULL)) {
		k_free((void *)field->value.bytes.data);
		field->value.bytes.data = NULL;
		field->value.bytes.len = 0U;
	}
}

static void zdb_doc_field_value_free(zdb_doc_field_t *field)
{
	if (field == NULL) {
		return;
	}

	if (field->name != NULL) {
		k_free((void *)field->name);
		field->name = NULL;
	}

	if ((field->type == ZDB_DOC_FIELD_STRING) && (field->value.str != NULL)) {
		k_free((void *)field->value.str);
		field->value.str = NULL;
	}

	if ((field->type == ZDB_DOC_FIELD_BYTES) && (field->value.bytes.data != NULL)) {
		k_free((void *)field->value.bytes.data);
		field->value.bytes.data = NULL;
		field->value.bytes.len = 0U;
	}
}

static void zdb_doc_fields_reset(zdb_doc_t *doc)
{
	size_t i;

	if ((doc == NULL) || (doc->fields == NULL)) {
		return;
	}

	for (i = 0U; i < doc->field_count; i++) {
		zdb_doc_field_value_free(&doc->fields[i]);
	}

	(void)memset(doc->fields, 0, sizeof(zdb_doc_field_t) * doc->max_fields);
	doc->field_count = 0U;
}

static int zdb_doc_build_root_path(const zdb_cfg_t *cfg, char *path, size_t path_len)
{
	int n;

	if ((cfg == NULL) || (cfg->lfs_mount_point == NULL) || (path == NULL) || (path_len == 0U)) {
		return -EINVAL;
	}

	n = snprintf(path, path_len, "%s/%s", cfg->lfs_mount_point, ZDB_DOC_DIRNAME);
	if ((n < 0) || ((size_t)n >= path_len)) {
		return -ENAMETOOLONG;
	}

	return 0;
}

static int zdb_doc_build_collection_path(const zdb_cfg_t *cfg, const char *collection, char *path,
						 size_t path_len)
{
	int n;

	if ((collection == NULL) || (collection[0] == '\0')) {
		return -EINVAL;
	}

	n = snprintf(path, path_len, "%s/%s/%s", cfg->lfs_mount_point, ZDB_DOC_DIRNAME, collection);
	if ((n < 0) || ((size_t)n >= path_len)) {
		return -ENAMETOOLONG;
	}

	return 0;
}

static int zdb_doc_build_doc_path(const zdb_cfg_t *cfg, const char *collection, const char *document_id,
					  char *path, size_t path_len)
{
	int n;

	if ((cfg == NULL) || (collection == NULL) || (document_id == NULL) || (path == NULL) ||
	    (path_len == 0U)) {
		return -EINVAL;
	}

	n = snprintf(path, path_len, "%s/%s/%s/%s%s", cfg->lfs_mount_point, ZDB_DOC_DIRNAME, collection,
		     document_id, ZDB_DOC_FILE_EXT);
	if ((n < 0) || ((size_t)n >= path_len)) {
		return -ENAMETOOLONG;
	}

	return 0;
}

static int zdb_doc_ensure_dirs(const zdb_cfg_t *cfg, const char *collection)
{
	char root_path[ZDB_DOC_PATH_MAX];
	char collection_path[ZDB_DOC_PATH_MAX];
	int rc;

	rc = zdb_doc_build_root_path(cfg, root_path, sizeof(root_path));
	if (rc < 0) {
		return rc;
	}

	rc = fs_mkdir(root_path);
	if ((rc < 0) && (rc != -EEXIST)) {
		return rc;
	}

	rc = zdb_doc_build_collection_path(cfg, collection, collection_path, sizeof(collection_path));
	if (rc < 0) {
		return rc;
	}

	rc = fs_mkdir(collection_path);
	if ((rc < 0) && (rc != -EEXIST)) {
		return rc;
	}

	return 0;
}

static bool zdb_doc_field_filter_match(const zdb_doc_field_t *field,
					       const zdb_doc_query_filter_t *filter)
{
	double ipart;

	if ((field == NULL) || (filter == NULL)) {
		return false;
	}

	if ((field->name == NULL) || (field->name[0] == '\0') ||
	    (filter->field_name == NULL) || (filter->field_name[0] == '\0')) {
		return false;
	}

	if (strcmp(field->name, filter->field_name) != 0) {
		return false;
	}

	if (field->type != filter->type) {
		return false;
	}

	switch (field->type) {
	case ZDB_DOC_FIELD_INT64:
		if ((filter->numeric_value < (double)INT64_MIN) ||
		    (filter->numeric_value > (double)INT64_MAX)) {
			return false;
		}
		return (modf(filter->numeric_value, &ipart) == 0.0) &&
		       (field->value.i64 == (int64_t)ipart);
	case ZDB_DOC_FIELD_DOUBLE:
		return field->value.f64 == filter->numeric_value;
	case ZDB_DOC_FIELD_BOOL:
		return ((field->value.b ? 1.0 : 0.0) == filter->numeric_value);
	case ZDB_DOC_FIELD_STRING:
		return (field->value.str != NULL) && (filter->string_value != NULL) &&
		       (strcmp(field->value.str, filter->string_value) == 0);
	case ZDB_DOC_FIELD_BYTES:
		if ((filter->string_value == NULL) || (field->value.bytes.data == NULL)) {
			return false;
		}
		return (field->value.bytes.len == strlen(filter->string_value)) &&
		       (memcmp(field->value.bytes.data, filter->string_value,
			       field->value.bytes.len) == 0);
	default:
		return false;
	}
}

static bool zdb_doc_matches_query(const zdb_doc_t *doc, const zdb_doc_query_t *query)
{
	size_t i;
	size_t j;
	bool matched;

	if ((doc == NULL) || (query == NULL)) {
		return false;
	}

	if ((query->from_ms != 0U) && (doc->updated_ms < query->from_ms)) {
		return false;
	}

	if ((query->to_ms != 0U) && (doc->updated_ms > query->to_ms)) {
		return false;
	}

	if ((query->filter_count > 0U) && (query->filters == NULL)) {
		return false;
	}

	for (i = 0U; i < query->filter_count; i++) {
		matched = false;
		for (j = 0U; j < doc->field_count; j++) {
			if (zdb_doc_field_filter_match(&doc->fields[j], &query->filters[i])) {
				matched = true;
				break;
			}
		}
		if (!matched) {
			return false;
		}
	}

	return true;
}

zdb_status_t zdb_doc_create(zdb_t *db, const char *collection_name,
			     const char *document_id, zdb_doc_t *out_doc)
{
	char *collection_dup;
	char *document_dup;
	if ((db == NULL) || (collection_name == NULL) || (document_id == NULL) ||
	    (out_doc == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if ((db->cfg == NULL) || (db->cfg->lfs_mount_point == NULL)) {
		return ZDB_ERR_INVAL;
	}

	/* Validate collection_name and document_id against CONFIG_ZDB_MAX_KEY_LEN */
	if ((strlen(collection_name) == 0U) || (strlen(collection_name) > CONFIG_ZDB_MAX_KEY_LEN) ||
	    (strlen(document_id) == 0U) || (strlen(document_id) > CONFIG_ZDB_MAX_KEY_LEN)) {
		return ZDB_ERR_INVAL;
	}

	collection_dup = zdb_strdup_local(collection_name);
	document_dup = zdb_strdup_local(document_id);
	if ((collection_dup == NULL) || (document_dup == NULL)) {
		k_free(collection_dup);
		k_free(document_dup);
		return ZDB_ERR_NOMEM;
	}

	/* Initialize document */
	out_doc->db = db;
	out_doc->collection_name = collection_dup;
	out_doc->document_id = document_dup;
	out_doc->fields = NULL;
	out_doc->field_count = 0;
	out_doc->max_fields = 0;
	out_doc->created_ms = k_uptime_get();
	out_doc->updated_ms = out_doc->created_ms;
	out_doc->valid = false;

	/* Allocate field array (stage 3: use slab) */
	out_doc->max_fields = CONFIG_ZDB_DOC_MAX_FIELD_COUNT;
	out_doc->fields = k_calloc(out_doc->max_fields, sizeof(zdb_doc_field_t));
	if (out_doc->fields == NULL) {
		k_free((void *)out_doc->collection_name);
		k_free((void *)out_doc->document_id);
		out_doc->db = NULL;
		out_doc->collection_name = NULL;
		out_doc->document_id = NULL;
		out_doc->fields = NULL;
		out_doc->field_count = 0U;
		out_doc->max_fields = 0U;
		out_doc->created_ms = 0U;
		out_doc->updated_ms = 0U;
		out_doc->valid = false;
		return ZDB_ERR_NOMEM;
	}

	out_doc->valid = true;
	return ZDB_OK;
}

zdb_status_t zdb_doc_open(zdb_t *db, const char *collection_name,
			   const char *document_id, zdb_doc_t *out_doc)
{
	return zdb_doc_open_impl(db, collection_name, document_id, out_doc, true);
}

zdb_status_t zdb_doc_save(zdb_doc_t *doc)
{
	char path[ZDB_DOC_PATH_MAX];
	struct fs_file_t file;
	struct zdb_doc_hdr_v1 hdr;
	struct zdb_doc_field_hdr_v1 fh;
	zdb_status_t lock_rc;
	int rc;
	ssize_t wr;
	size_t i;

	if ((doc == NULL) || (!doc->valid)) {
		return ZDB_ERR_INVAL;
	}

	doc->updated_ms = k_uptime_get();
	if (doc->db == NULL || doc->db->cfg == NULL) {
		return ZDB_ERR_INVAL;
	}

	lock_rc = zdb_lock_write(doc->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	rc = zdb_doc_ensure_dirs(doc->db->cfg, doc->collection_name);
	if (rc < 0) {
		zdb_unlock_write(doc->db);
		return zdb_status_from_errno(rc);
	}

	rc = zdb_doc_build_doc_path(doc->db->cfg, doc->collection_name, doc->document_id, path,
				    sizeof(path));
	if (rc < 0) {
		zdb_unlock_write(doc->db);
		return zdb_status_from_errno(rc);
	}

	hdr.magic_le = sys_cpu_to_le32(ZDB_DOC_MAGIC);
	hdr.version_le = sys_cpu_to_le16(ZDB_DOC_VERSION);
	hdr.field_count_le = sys_cpu_to_le16((uint16_t)doc->field_count);
	hdr.created_ms_le = sys_cpu_to_le64(doc->created_ms);
	hdr.updated_ms_le = sys_cpu_to_le64(doc->updated_ms);
	hdr.crc_le = sys_cpu_to_le32(0U);

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc < 0) {
		zdb_unlock_write(doc->db);
		return zdb_status_from_errno(rc);
	}

	wr = fs_write(&file, &hdr, sizeof(hdr));
	if (wr != (ssize_t)sizeof(hdr)) {
		(void)fs_close(&file);
		zdb_unlock_write(doc->db);
		return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
	}

	for (i = 0U; i < doc->field_count; i++) {
		const zdb_doc_field_t *field = &doc->fields[i];
		uint16_t name_len = (uint16_t)strlen(field->name);

		fh.name_len_le = sys_cpu_to_le16(name_len);
		fh.reserved_le = 0U;
		fh.type = (uint8_t)field->type;
		fh.reserved[0] = fh.reserved[1] = fh.reserved[2] = 0U;

		wr = fs_write(&file, &fh, sizeof(fh));
		if (wr != (ssize_t)sizeof(fh)) {
			(void)fs_close(&file);
			zdb_unlock_write(doc->db);
			return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
		}

		wr = fs_write(&file, field->name, name_len);
		if (wr != (ssize_t)name_len) {
			(void)fs_close(&file);
			zdb_unlock_write(doc->db);
			return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
		}

		switch (field->type) {
		case ZDB_DOC_FIELD_INT64: {
			uint64_t tmp_le = sys_cpu_to_le64((uint64_t)field->value.i64);
			wr = fs_write(&file, &tmp_le, sizeof(tmp_le));
			if (wr != (ssize_t)sizeof(tmp_le)) {
				(void)fs_close(&file);
				zdb_unlock_write(doc->db);
				return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
			}
			break;
		}
		case ZDB_DOC_FIELD_DOUBLE: {
			uint64_t tmp_le;
			(void)memcpy(&tmp_le, &field->value.f64, sizeof(tmp_le));
			tmp_le = sys_cpu_to_le64(tmp_le);
			wr = fs_write(&file, &tmp_le, sizeof(tmp_le));
			if (wr != (ssize_t)sizeof(tmp_le)) {
				(void)fs_close(&file);
				zdb_unlock_write(doc->db);
				return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
			}
			break;
		}
		case ZDB_DOC_FIELD_BOOL: {
			uint8_t b = field->value.b ? 1U : 0U;
			wr = fs_write(&file, &b, sizeof(b));
			if (wr != (ssize_t)sizeof(b)) {
				(void)fs_close(&file);
				zdb_unlock_write(doc->db);
				return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
			}
			break;
		}
		case ZDB_DOC_FIELD_STRING: {
			uint32_t str_len = (uint32_t)strlen(field->value.str != NULL ? field->value.str : "");
			uint32_t str_len_le = sys_cpu_to_le32(str_len);
			wr = fs_write(&file, &str_len_le, sizeof(str_len_le));
			if (wr != (ssize_t)sizeof(str_len_le)) {
				(void)fs_close(&file);
				zdb_unlock_write(doc->db);
				return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
			}
			if (str_len > 0U) {
				wr = fs_write(&file, field->value.str, str_len);
				if (wr != (ssize_t)str_len) {
					(void)fs_close(&file);
					zdb_unlock_write(doc->db);
					return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
				}
			}
			break;
		}
		case ZDB_DOC_FIELD_BYTES: {
			uint32_t bytes_len = (uint32_t)field->value.bytes.len;
			uint32_t bytes_len_le = sys_cpu_to_le32(bytes_len);
			wr = fs_write(&file, &bytes_len_le, sizeof(bytes_len_le));
			if (wr != (ssize_t)sizeof(bytes_len_le)) {
				(void)fs_close(&file);
				zdb_unlock_write(doc->db);
				return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
			}
			if (bytes_len > 0U) {
				wr = fs_write(&file, field->value.bytes.data, bytes_len);
				if (wr != (ssize_t)bytes_len) {
					(void)fs_close(&file);
					zdb_unlock_write(doc->db);
					return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
				}
			}
			break;
		}
		default:
			(void)fs_close(&file);
			zdb_unlock_write(doc->db);
			return ZDB_ERR_UNSUPPORTED;
		}
	}

	(void)fs_close(&file);
	zdb_unlock_write(doc->db);
	return ZDB_OK;

}

zdb_status_t zdb_doc_delete(zdb_t *db, const char *collection_name,
			     const char *document_id)
{
	char path[ZDB_DOC_PATH_MAX];
	zdb_status_t lock_rc;
	int rc;

	if ((db == NULL) || (collection_name == NULL) || (document_id == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if ((db->cfg == NULL) || (db->cfg->lfs_mount_point == NULL)) {
		return ZDB_ERR_INVAL;
	}

	/* Acquire write lock to prevent races with open/query */
	lock_rc = zdb_lock_write(db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	rc = zdb_doc_build_doc_path(db->cfg, collection_name, document_id, path, sizeof(path));
	if (rc < 0) {
		zdb_unlock_write(db);
		return zdb_status_from_errno(rc);
	}

	rc = fs_unlink(path);
	zdb_unlock_write(db);
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	return ZDB_OK;
}

zdb_status_t zdb_doc_close(zdb_doc_t *doc)
{
	if (doc == NULL) {
		return ZDB_ERR_INVAL;
	}

	zdb_doc_fields_reset(doc);

	if (doc->fields != NULL) {
		k_free(doc->fields);
		doc->fields = NULL;
	}

	if (doc->collection_name != NULL) {
		k_free((void *)doc->collection_name);
		doc->collection_name = NULL;
	}

	if (doc->document_id != NULL) {
		k_free((void *)doc->document_id);
		doc->document_id = NULL;
	}

	doc->valid = false;
	return ZDB_OK;
}

/*
 * Field setters
 */
static zdb_status_t zdb_doc_field_find_or_create(zdb_doc_t *doc,
						  const char *field_name,
						  zdb_doc_field_type_t type,
						  zdb_doc_field_t **out_field)
{
	size_t i;
	char *name_dup;
	zdb_doc_field_t *field = NULL;

	if ((doc == NULL) || (field_name == NULL) || (out_field == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (!doc->valid) {
		return ZDB_ERR_INVAL;
	}

	/* Validate field_name against CONFIG_ZDB_DOC_MAX_FIELD_NAME_LEN */
	if ((strlen(field_name) == 0U) || (strlen(field_name) > CONFIG_ZDB_DOC_MAX_FIELD_NAME_LEN)) {
		return ZDB_ERR_INVAL;
	}

	for (i = 0U; i < doc->field_count; i++) {
		if ((doc->fields[i].name != NULL) &&
		    (strcmp(doc->fields[i].name, field_name) == 0)) {
			field = &doc->fields[i];
			break;
		}
	}

	if (field == NULL) {
		if (doc->field_count >= doc->max_fields) {
			return ZDB_ERR_NOMEM;
		}

		name_dup = zdb_strdup_local(field_name);
		if (name_dup == NULL) {
			return ZDB_ERR_NOMEM;
		}

		field = &doc->fields[doc->field_count++];
		(void)memset(field, 0, sizeof(*field));
		field->name = name_dup;
	} else if (field->type != type) {
		zdb_doc_field_payload_free(field);
		(void)memset(&field->value, 0, sizeof(field->value));
	}

	field->type = type;
	*out_field = field;
	return ZDB_OK;
}

zdb_status_t zdb_doc_field_set_i64(zdb_doc_t *doc, const char *field_name,
				    int64_t value)
{
	zdb_doc_field_t *field = NULL;
	zdb_status_t rc = zdb_doc_field_find_or_create(doc, field_name,
						       ZDB_DOC_FIELD_INT64, &field);
	if (rc != ZDB_OK) {
		return rc;
	}

	field->value.i64 = value;
	doc->updated_ms = k_uptime_get();
	return ZDB_OK;
}

zdb_status_t zdb_doc_field_set_f64(zdb_doc_t *doc, const char *field_name,
				    double value)
{
	zdb_doc_field_t *field = NULL;
	zdb_status_t rc = zdb_doc_field_find_or_create(doc, field_name,
						       ZDB_DOC_FIELD_DOUBLE, &field);
	if (rc != ZDB_OK) {
		return rc;
	}

	field->value.f64 = value;
	doc->updated_ms = k_uptime_get();
	return ZDB_OK;
}

zdb_status_t zdb_doc_field_set_string(zdb_doc_t *doc, const char *field_name,
				       const char *value)
{
	zdb_doc_field_t *field = NULL;
	char *dup;

	if ((doc == NULL) || (field_name == NULL) || (value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (strlen(value) > (size_t)CONFIG_ZDB_DOC_MAX_STRING_LEN) {
		return ZDB_ERR_INVAL;
	}

	zdb_status_t rc = zdb_doc_field_find_or_create(doc, field_name,
						       ZDB_DOC_FIELD_STRING, &field);
	if (rc != ZDB_OK) {
		return rc;
	}

	dup = zdb_strdup_local(value);
	if (dup == NULL) {
		return ZDB_ERR_NOMEM;
	}

	if ((field->value.str != NULL) && (field->type == ZDB_DOC_FIELD_STRING)) {
		k_free((void *)field->value.str);
	}
	field->value.str = dup;
	doc->updated_ms = k_uptime_get();
	return ZDB_OK;
}

zdb_status_t zdb_doc_field_set_bool(zdb_doc_t *doc, const char *field_name,
				     bool value)
{
	zdb_doc_field_t *field = NULL;
	zdb_status_t rc = zdb_doc_field_find_or_create(doc, field_name,
						       ZDB_DOC_FIELD_BOOL, &field);
	if (rc != ZDB_OK) {
		return rc;
	}

	field->value.b = value;
	doc->updated_ms = k_uptime_get();
	return ZDB_OK;
}

zdb_status_t zdb_doc_field_set_bytes(zdb_doc_t *doc, const char *field_name,
				      const void *value, size_t len)
{
	zdb_doc_field_t *field = NULL;
	void *dup;

	if ((doc == NULL) || (field_name == NULL) || ((value == NULL) && (len > 0U))) {
		return ZDB_ERR_INVAL;
	}

	if (len > CONFIG_ZDB_DOC_MAX_BYTES_LEN) {
		return ZDB_ERR_INVAL;
	}

	zdb_status_t rc = zdb_doc_field_find_or_create(doc, field_name,
						       ZDB_DOC_FIELD_BYTES, &field);
	if (rc != ZDB_OK) {
		return rc;
	}

	dup = zdb_alloc_copy(value, len);
	if ((len > 0U) && (dup == NULL)) {
		return ZDB_ERR_NOMEM;
	}

	if ((field->value.bytes.data != NULL) && (field->type == ZDB_DOC_FIELD_BYTES)) {
		k_free((void *)field->value.bytes.data);
	}
	field->value.bytes.data = dup;
	field->value.bytes.len = len;
	doc->updated_ms = k_uptime_get();
	return ZDB_OK;
}

/*
 * Field getters
 */
zdb_status_t zdb_doc_field_get_i64(const zdb_doc_t *doc, const char *field_name,
				    int64_t *out_value)
{
	if ((doc == NULL) || (field_name == NULL) || (out_value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	for (size_t i = 0; i < doc->field_count; i++) {
		if (strcmp(doc->fields[i].name, field_name) == 0) {
			if (doc->fields[i].type != ZDB_DOC_FIELD_INT64) {
				return ZDB_ERR_INVAL;
			}
			*out_value = doc->fields[i].value.i64;
			return ZDB_OK;
		}
	}

	return ZDB_ERR_NOT_FOUND;
}

zdb_status_t zdb_doc_field_get_f64(const zdb_doc_t *doc, const char *field_name,
				    double *out_value)
{
	if ((doc == NULL) || (field_name == NULL) || (out_value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	for (size_t i = 0; i < doc->field_count; i++) {
		if (strcmp(doc->fields[i].name, field_name) == 0) {
			if (doc->fields[i].type != ZDB_DOC_FIELD_DOUBLE) {
				return ZDB_ERR_INVAL;
			}
			*out_value = doc->fields[i].value.f64;
			return ZDB_OK;
		}
	}

	return ZDB_ERR_NOT_FOUND;
}

zdb_status_t zdb_doc_field_get_string(const zdb_doc_t *doc, const char *field_name,
				       const char **out_value)
{
	if ((doc == NULL) || (field_name == NULL) || (out_value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	for (size_t i = 0; i < doc->field_count; i++) {
		if (strcmp(doc->fields[i].name, field_name) == 0) {
			if (doc->fields[i].type != ZDB_DOC_FIELD_STRING) {
				return ZDB_ERR_INVAL;
			}
			*out_value = doc->fields[i].value.str;
			return ZDB_OK;
		}
	}

	return ZDB_ERR_NOT_FOUND;
}

zdb_status_t zdb_doc_field_get_bool(const zdb_doc_t *doc, const char *field_name,
				     bool *out_value)
{
	if ((doc == NULL) || (field_name == NULL) || (out_value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	for (size_t i = 0; i < doc->field_count; i++) {
		if (strcmp(doc->fields[i].name, field_name) == 0) {
			if (doc->fields[i].type != ZDB_DOC_FIELD_BOOL) {
				return ZDB_ERR_INVAL;
			}
			*out_value = doc->fields[i].value.b;
			return ZDB_OK;
		}
	}

	return ZDB_ERR_NOT_FOUND;
}

zdb_status_t zdb_doc_field_get_bytes(const zdb_doc_t *doc, const char *field_name,
				      zdb_bytes_t *out_value)
{
	if ((doc == NULL) || (field_name == NULL) || (out_value == NULL)) {
		return ZDB_ERR_INVAL;
	}

	for (size_t i = 0; i < doc->field_count; i++) {
		if (strcmp(doc->fields[i].name, field_name) == 0) {
			if (doc->fields[i].type != ZDB_DOC_FIELD_BYTES) {
				return ZDB_ERR_INVAL;
			}
			*out_value = doc->fields[i].value.bytes;
			return ZDB_OK;
		}
	}

	return ZDB_ERR_NOT_FOUND;
}

/*
 * Query and export
 */
zdb_status_t zdb_doc_query(zdb_t *db, const zdb_doc_query_t *query,
			    zdb_doc_metadata_t *out_metadata, size_t *out_count)
{
	char root_path[ZDB_DOC_PATH_MAX];
	struct fs_dir_t dir_collection;
	struct fs_dir_t dir_docs;
	struct fs_dirent collection_ent;
	struct fs_dirent doc_ent;
	zdb_status_t lock_rc;
	int rc;
	size_t capacity;
	size_t matched = 0U;
	size_t written = 0U;
	bool stop_scan = false;
	uint32_t limit;

	if ((db == NULL) || (query == NULL) || (out_count == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if ((db->cfg == NULL) || (db->cfg->lfs_mount_point == NULL)) {
		return ZDB_ERR_INVAL;
	}

	capacity = *out_count;
	limit = (query->limit == 0U) ? UINT32_MAX : query->limit;

	lock_rc = zdb_lock_read(db);
	if (lock_rc != ZDB_OK) {
		*out_count = 0U;
		return lock_rc;
	}

	rc = zdb_doc_build_root_path(db->cfg, root_path, sizeof(root_path));
	if (rc < 0) {
		zdb_unlock_read(db);
		return zdb_status_from_errno(rc);
	}

	fs_dir_t_init(&dir_collection);
	rc = fs_opendir(&dir_collection, root_path);
	if (rc < 0) {
		*out_count = 0U;
		zdb_unlock_read(db);
		if (rc == -ENOENT) {
			return ZDB_OK;
		}
		return zdb_status_from_errno(rc);
	}

	while (!stop_scan && (matched < limit) &&
	       (fs_readdir(&dir_collection, &collection_ent) == 0) &&
	       (collection_ent.name[0] != '\0')) {
		char collection_path[ZDB_DOC_PATH_MAX];

		if (collection_ent.type != FS_DIR_ENTRY_DIR) {
			continue;
		}

		rc = zdb_doc_build_collection_path(db->cfg, collection_ent.name, collection_path,
						   sizeof(collection_path));
		if (rc < 0) {
			continue;
		}

		fs_dir_t_init(&dir_docs);
		rc = fs_opendir(&dir_docs, collection_path);
		if (rc < 0) {
			continue;
		}

		while (!stop_scan && (matched < limit) &&
		       (fs_readdir(&dir_docs, &doc_ent) == 0) &&
		       (doc_ent.name[0] != '\0')) {
			size_t name_len;
			char doc_id[CONFIG_ZDB_MAX_KEY_LEN + 1];
			zdb_doc_t doc;

			if (doc_ent.type != FS_DIR_ENTRY_FILE) {
				continue;
			}

			name_len = strlen(doc_ent.name);
			if ((name_len <= strlen(ZDB_DOC_FILE_EXT)) ||
			    (strcmp(doc_ent.name + name_len - strlen(ZDB_DOC_FILE_EXT), ZDB_DOC_FILE_EXT) != 0)) {
				continue;
			}

			if (name_len - strlen(ZDB_DOC_FILE_EXT) > CONFIG_ZDB_MAX_KEY_LEN) {
				continue;
			}

			(void)memcpy(doc_id, doc_ent.name, name_len - strlen(ZDB_DOC_FILE_EXT));
			doc_id[name_len - strlen(ZDB_DOC_FILE_EXT)] = '\0';

			rc = zdb_doc_open_impl(db, collection_ent.name, doc_id, &doc, false);
			if (rc != ZDB_OK) {
				continue;
			}

			if (zdb_doc_matches_query(&doc, query)) {
				if ((out_metadata != NULL) && (written < capacity)) {
					out_metadata[written].collection_name = zdb_strdup_local(collection_ent.name);
					if (out_metadata[written].collection_name == NULL) {
					/* Allocation failed; return partial results */
					(void)zdb_doc_close(&doc);
					(void)fs_closedir(&dir_docs);
					(void)fs_closedir(&dir_collection);
					zdb_unlock_read(db);
					*out_count = written;
					return ZDB_OK;
					}
					out_metadata[written].document_id = zdb_strdup_local(doc_id);
					if (out_metadata[written].document_id == NULL) {
					/* Allocation failed; free collection_name and return partial results */
					zdb_doc_metadata_entry_free(&out_metadata[written]);
					(void)zdb_doc_close(&doc);
					(void)fs_closedir(&dir_docs);
					(void)fs_closedir(&dir_collection);
					zdb_unlock_read(db);
					*out_count = written;
					return ZDB_OK;
					}

					out_metadata[written].created_ms = doc.created_ms;
					out_metadata[written].updated_ms = doc.updated_ms;
					out_metadata[written].field_count = (uint32_t)doc.field_count;
					written++;
					if (written >= capacity) {
						stop_scan = true;
					}
				}
				matched++;
			}

			(void)zdb_doc_close(&doc);
		}

		(void)fs_closedir(&dir_docs);
	}

	(void)fs_closedir(&dir_collection);
	zdb_unlock_read(db);
	*out_count = ((out_metadata == NULL) || (capacity == 0U)) ? matched : written;
	return ZDB_OK;
}

zdb_status_t zdb_doc_metadata_free(zdb_doc_metadata_t *metadata, size_t count)
{
	size_t i;

	if ((metadata == NULL) && (count > 0U)) {
		return ZDB_ERR_INVAL;
	}

	for (i = 0U; i < count; i++) {
		zdb_doc_metadata_entry_free(&metadata[i]);
	}

	return ZDB_OK;
}

zdb_status_t zdb_doc_export_flatbuffer(zdb_doc_t *doc, uint8_t *out_buf,
				       size_t out_capacity, size_t *out_len)
{
	if ((doc == NULL) || (out_len == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (!doc->valid) {
		return ZDB_ERR_INVAL;
	}

	/* Stage 3: Export document as FlatBuffer */
	/* For now, estimate size based on field count and types */
	size_t estimated_size = 64 + (doc->field_count * 32);

	if (out_buf == NULL) {
		*out_len = estimated_size;
		return ZDB_OK;
	}

	if (out_capacity < estimated_size) {
		return ZDB_ERR_INVAL;
	}

	/* Placeholder serialization */
	*out_len = estimated_size;
	return ZDB_OK;
}

#endif /* CONFIG_ZDB_DOC */

#endif /* CONFIG_ZDB_TS */
