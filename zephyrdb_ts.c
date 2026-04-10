/* TS module implementation */
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)

#include "zephyrdb_internal.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>

#include <zephyr/sys/crc.h>
#include <zephyr/kernel.h>

#if defined(CONFIG_ZDB_TS_BACKEND_LITTLEFS) && (CONFIG_ZDB_TS_BACKEND_LITTLEFS)
#include <zephyr/fs/fs.h>
#endif
#if defined(CONFIG_ZDB_TS_BACKEND_FCB) && (CONFIG_ZDB_TS_BACKEND_FCB)
#include <zephyr/fs/fcb.h>
#endif

#include <zephyr/sys/byteorder.h>

#if defined(CONFIG_ZDB_FLATBUFFERS) && (CONFIG_ZDB_FLATBUFFERS)
#include <flatcc/flatcc_builder.h>
#endif

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

zdb_status_t zdb_ts_recover_stream(zdb_ts_t *ts, size_t *out_truncated_bytes);

static bool zdb_ts_stream_name_valid(const char *stream_name)
{
	const char *p;
	size_t n;

	if ((stream_name == NULL) || ((*stream_name) == '\0')) {
		return false;
	}

	n = strlen(stream_name);
	if (n > (size_t)CONFIG_ZDB_TS_STREAM_NAME_MAX_LEN) {
		return false;
	}

	if ((strcmp(stream_name, ".") == 0) || (strcmp(stream_name, "..") == 0)) {
		return false;
	}

	for (p = stream_name; (*p) != '\0'; p++) {
		if (((*p) == '/') || ((*p) == '\\')) {
			return false;
		}
	}

	return true;
}

#if ZDB_TS_USE_FCB
static int zdb_ts_fcb_ensure_init(struct zdb_ts_core_ctx *ctx)
{
	uint32_t sector_count;
	int rc;

	if (ctx == NULL) {
		return -EINVAL;
	}

	if (ctx->fcb_initialized) {
		return 0;
	}

	(void)memset(&ctx->ts_fcb, 0, sizeof(ctx->ts_fcb));
	ctx->ts_fcb.f_magic = ZDB_TS_FCB_MAGIC;
	ctx->ts_fcb.f_version = ZDB_TS_FCB_VERSION;
	ctx->ts_fcb.f_scratch_cnt = 1U;
	ctx->ts_fcb.f_sectors = ctx->ts_fcb_sectors;

	sector_count = ARRAY_SIZE(ctx->ts_fcb_sectors);
	rc = flash_area_get_sectors(CONFIG_ZDB_TS_FCB_FLASH_AREA_ID, &sector_count,
				    ctx->ts_fcb_sectors);
	if (rc < 0) {
		return rc;
	}

	if (sector_count < 2U) {
		return -EINVAL;
	}

	ctx->ts_fcb.f_sector_cnt = (uint16_t)sector_count;
	rc = fcb_init(CONFIG_ZDB_TS_FCB_FLASH_AREA_ID, &ctx->ts_fcb);
	if (rc < 0) {
		return rc;
	}

	ctx->fcb_initialized = true;
	return 0;
}

static int zdb_ts_fcb_append_record(struct zdb_ts_core_ctx *ctx,
				    const struct zdb_ts_record_i64 *rec)
{
	struct fcb_entry loc;
	int rc;

	if ((ctx == NULL) || (rec == NULL)) {
		return -EINVAL;
	}

	rc = zdb_ts_fcb_ensure_init(ctx);
	if (rc < 0) {
		return rc;
	}

	rc = fcb_append(&ctx->ts_fcb, (uint16_t)sizeof(*rec), &loc);
	if (rc == -ENOSPC) {
		rc = fcb_rotate(&ctx->ts_fcb);
		if (rc < 0) {
			return rc;
		}
		rc = fcb_append(&ctx->ts_fcb, (uint16_t)sizeof(*rec), &loc);
	}
	if (rc < 0) {
		return rc;
	}

	rc = flash_area_write(ctx->ts_fcb.fap, FCB_ENTRY_FA_DATA_OFF(loc), rec, sizeof(*rec));
	if (rc < 0) {
		return rc;
	}

	return fcb_append_finish(&ctx->ts_fcb, &loc);
}

static zdb_status_t zdb_ts_fcb_cursor_read_record(struct zdb_ts_core_ctx *ctx,
					   struct zdb_ts_cursor_ctx *cctx,
					   zdb_bytes_t *out_record)
{
	int rc;

	if ((ctx == NULL) || (cctx == NULL) || (out_record == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (!cctx->fcb_started) {
		(void)memset(&cctx->fcb_loc, 0, sizeof(cctx->fcb_loc));
		cctx->fcb_loc.fe_sector = NULL;
		cctx->fcb_started = true;
	}

	rc = fcb_getnext(&ctx->ts_fcb, &cctx->fcb_loc);
	if (rc < 0) {
		if (rc == -ENOENT) {
			return ZDB_ERR_NOT_FOUND;
		}
		return zdb_status_from_errno(rc);
	}

	if (cctx->fcb_loc.fe_data_len != sizeof(cctx->cache)) {
		return ZDB_ERR_CORRUPT;
	}

	rc = flash_area_read(ctx->ts_fcb.fap, FCB_ENTRY_FA_DATA_OFF(cctx->fcb_loc),
			    &cctx->cache, sizeof(cctx->cache));
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	out_record->data = (const uint8_t *)&cctx->cache;
	out_record->len = sizeof(cctx->cache);
	return ZDB_OK;
}
#endif

#if ZDB_TS_USE_LITTLEFS
static int zdb_ts_ensure_stream_dir(const zdb_cfg_t *cfg, struct zdb_ts_core_ctx *ctx)
{
	char ts_dir[ZDB_TS_PATH_MAX];
	int n;
	int rc;

	if ((cfg == NULL) || (cfg->lfs_mount_point == NULL) || (ctx == NULL)) {
		return -EINVAL;
	}

	if (ctx->ts_dir_ready) {
		return 0;
	}

	n = snprintf(ts_dir, sizeof(ts_dir), "%s/%s", cfg->lfs_mount_point, CONFIG_ZDB_TS_DIRNAME);
	if ((n < 0) || ((size_t)n >= sizeof(ts_dir))) {
		return -ENAMETOOLONG;
	}

	rc = fs_mkdir(ts_dir);
	if ((rc < 0) && (rc != -EEXIST)) {
		return rc;
	}

	ctx->ts_dir_ready = true;
	return 0;
}
#endif

 #if ZDB_TS_USE_LITTLEFS
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
		ZDB_STAT_INC(db, corrupt_records);
		return ZDB_ERR_CORRUPT;
	}

	if (sys_le16_to_cpu(hdr->version_le) != ZDB_TS_STREAM_VERSION) {
		ZDB_STAT_INC(db, unsupported_versions);
		return ZDB_ERR_UNSUPPORTED;
	}

	expect_crc = crc32_ieee((const uint8_t *)hdr,
			       offsetof(struct zdb_ts_stream_header, crc_le));
	got_crc = sys_le32_to_cpu(hdr->crc_le);
	if (got_crc != expect_crc) {
		ZDB_STAT_INC(db, crc_failures);
		ZDB_STAT_INC(db, corrupt_records);
		return ZDB_ERR_CORRUPT;
	}

	stream_id = sys_le32_to_cpu(hdr->stream_id_le);
	if (stream_id != zdb_fnv1a32(stream_name)) {
		ZDB_STAT_INC(db, corrupt_records);
		return ZDB_ERR_CORRUPT;
	}

	return ZDB_OK;
}
#endif

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
		ZDB_STAT_INC(db, corrupt_records);
		zdb_health_check(db);
		return ZDB_ERR_CORRUPT;
	}

	if (sys_le16_to_cpu(rec->version_le) != ZDB_TS_REC_VERSION) {
		ZDB_STAT_INC(db, unsupported_versions);
		return ZDB_ERR_UNSUPPORTED;
	}

	expect_crc = crc32_ieee((const uint8_t *)rec, offsetof(struct zdb_ts_record_i64, crc_le));
	got_crc = sys_le32_to_cpu(rec->crc_le);
	if (got_crc != expect_crc) {
		ZDB_STAT_INC(db, crc_failures);
		ZDB_STAT_INC(db, corrupt_records);
		zdb_health_check(db);
		return ZDB_ERR_CORRUPT;
	}

	*out_ts_ms = sys_le64_to_cpu(rec->ts_ms_le);
	*out_value = (int64_t)sys_le64_to_cpu(rec->value_le);
	return ZDB_OK;
}

#if ZDB_TS_USE_LITTLEFS
static int zdb_ts_build_path(const zdb_cfg_t *cfg, const char *stream_name,
			     char *path, size_t path_len)
{
	int n;

	if ((cfg == NULL) || (cfg->lfs_mount_point == NULL) || (stream_name == NULL) ||
	    (path == NULL) || (path_len == 0U)) {
		return -EINVAL;
	}

	if (!zdb_ts_stream_name_valid(stream_name)) {
		return -EINVAL;
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
#endif

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

#if ZDB_TS_USE_LITTLEFS
static zdb_status_t zdb_ts_cursor_read_file_record(struct zdb_ts_cursor_ctx *cctx,
						    zdb_bytes_t *out_record)
{
	ssize_t rd;

	if ((cctx == NULL) || (cctx->db == NULL) || (out_record == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (!cctx->file_open) {
		return ZDB_ERR_NOT_FOUND;
	}

	rd = fs_read(&cctx->file, &cctx->cache, sizeof(cctx->cache));

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
#endif

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
	int rc = 0;
	size_t flushed_bytes = 0U;
	zdb_status_t status = ZDB_OK;

	if ((ctx == NULL) || (ctx->db == NULL)) {
		return;
	}

	if (zdb_lock_write(ctx->db) != ZDB_OK) {
		ctx->flush_pending = false;
		k_sem_give(&ctx->flush_done);
		return;
	}

#if ZDB_TS_USE_LITTLEFS
	flushed_bytes = ctx->ingest_used;
	rc = zdb_ts_flush_buffer_locked(ctx);
	if (rc < 0) {
		/* Keep data in buffer for retry by not mutating ingest_used on error. */
		status = zdb_status_from_errno(rc);
	}
#else
	ARG_UNUSED(rc);
#endif
	ctx->flush_pending = false;
	k_sem_give(&ctx->flush_done);
	zdb_unlock_write(ctx->db);

#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	if ((flushed_bytes > 0U) || (status != ZDB_OK)) {
		zdb_emit_ts_event(ctx->db, ZDB_TS_EVENT_FLUSH, ctx->active_stream,
				  0U, 0, flushed_bytes, 0U, status);
	}
#endif
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

#if ZDB_TS_USE_LITTLEFS
	ctx->ingest_capacity = MIN((size_t)CONFIG_ZDB_TS_INGEST_BUFFER_BYTES,
				   (size_t)CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_SIZE);
	if ((db->ts_ingest_slab == NULL) ||
	    (k_mem_slab_alloc(db->ts_ingest_slab, (void **)&ctx->ingest_buf, K_NO_WAIT) != 0)) {
		k_mem_slab_free(db->core_slab, ctx);
		return NULL;
	}
#else
	ctx->ingest_capacity = 0U;
	ctx->ingest_buf = NULL;
#endif

	k_work_init(&ctx->flush_work, zdb_ts_flush_work_handler);
	k_sem_init(&ctx->flush_done, 0, 1);
	db->ts_ctx = ctx;

	return ctx;
}

zdb_status_t zdb_ts_open(zdb_t *db, const char *stream_name, zdb_ts_t *ts)
{
	struct zdb_ts_core_ctx *ctx;
	int fcb_rc;
#if !ZDB_TS_USE_FCB
	zdb_status_t rc;
	size_t truncated = 0U;
#endif

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

#if ZDB_TS_USE_FCB
	fcb_rc = zdb_ts_fcb_ensure_init(ctx);
	if (fcb_rc < 0) {
		return zdb_status_from_errno(fcb_rc);
	}
#else
	ARG_UNUSED(fcb_rc);
	{
		int ensure_rc = zdb_ts_ensure_stream_dir(db->cfg, ctx);

		if (ensure_rc < 0) {
			return zdb_status_from_errno(ensure_rc);
		}
	}

	rc = zdb_ts_ensure_stream_header(db, stream_name);
	if (rc != ZDB_OK) {
		return rc;
	}
#endif

	if ((ctx->active_stream != NULL) && (strcmp(ctx->active_stream, stream_name) != 0)) {
		return ZDB_ERR_BUSY;
	}

	ts->db = db;
	ts->stream_name = stream_name;
	ctx->active_stream = stream_name;

#if defined(CONFIG_ZDB_TS_AUTO_RECOVER_ON_OPEN) && (CONFIG_ZDB_TS_AUTO_RECOVER_ON_OPEN)
	rc = zdb_ts_recover_stream(ts, &truncated);
	if (rc != ZDB_OK) {
		ZDB_STAT_INC(db, recover_failures);
		return rc;
	}
	if ((CONFIG_ZDB_TS_MAX_RECOVERY_TRUNCATE_BYTES > 0) &&
	    (truncated > (size_t)CONFIG_ZDB_TS_MAX_RECOVERY_TRUNCATE_BYTES)) {
		ZDB_STAT_INC(db, recover_failures);
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
	zdb_status_t status = ZDB_OK;
	int rc;
#if !ZDB_TS_USE_FCB
	bool need_async_flush = false;
#endif

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

#if ZDB_TS_USE_FCB
	zdb_ts_record_encode(sample, &rec);
	rc = zdb_ts_fcb_append_record(ctx, &rec);
	zdb_unlock_write(ts->db);

	status = (rc < 0) ? zdb_status_from_errno(rc) : ZDB_OK;
#else
	if ((ctx->ingest_capacity < sizeof(rec)) || (ctx->ingest_buf == NULL)) {
		zdb_unlock_write(ts->db);
		status = ZDB_ERR_NOMEM;
		goto out;
	}

	if ((ctx->ingest_used + sizeof(rec)) > ctx->ingest_capacity) {
		rc = zdb_ts_flush_buffer_locked(ctx);
		if (rc < 0) {
			zdb_unlock_write(ts->db);
			status = zdb_status_from_errno(rc);
			goto out;
		}
	}

	zdb_ts_record_encode(sample, &rec);
	(void)memcpy(&ctx->ingest_buf[ctx->ingest_used], &rec, sizeof(rec));
	ctx->ingest_used += sizeof(rec);
	need_async_flush = (ctx->ingest_used == ctx->ingest_capacity);
	zdb_unlock_write(ts->db);

	if (need_async_flush) {
		status = zdb_ts_flush_async(ts);
	}
#endif

out:
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	zdb_emit_ts_event(ts->db, ZDB_TS_EVENT_APPEND, ts->stream_name, sample->ts_ms,
			  sample->value, 0U, 0U, status);
#endif

	return status;
}

zdb_status_t zdb_ts_append_batch_i64(zdb_ts_t *ts, const zdb_ts_sample_i64_t *samples,
			      size_t sample_count)
{
	struct zdb_ts_core_ctx *ctx;
	struct zdb_ts_record_i64 rec;
	zdb_status_t lock_rc;
	zdb_status_t status = ZDB_OK;
	size_t i;
	int rc;

	if ((ts == NULL) || (ts->db == NULL) || (samples == NULL) || (sample_count == 0U)) {
		return ZDB_ERR_INVAL;
	}

	ctx = zdb_ts_ctx_get_or_alloc(ts->db);
	if (ctx == NULL) {
		return ZDB_ERR_INVAL;
	}

	lock_rc = zdb_lock_write(ts->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	if (ctx->active_stream == NULL) {
		zdb_unlock_write(ts->db);
		return ZDB_ERR_INVAL;
	}

	if (strcmp(ctx->active_stream, ts->stream_name) != 0) {
		zdb_unlock_write(ts->db);
		return ZDB_ERR_BUSY;
	}

	for (i = 0U; i < sample_count; i++) {
#if ZDB_TS_USE_FCB
		zdb_ts_record_encode(&samples[i], &rec);
		rc = zdb_ts_fcb_append_record(ctx, &rec);
		if (rc < 0) {
			status = zdb_status_from_errno(rc);
			break;
		}
#else
		if ((ctx->ingest_capacity < sizeof(rec)) || (ctx->ingest_buf == NULL)) {
			status = ZDB_ERR_NOMEM;
			break;
		}

		if ((ctx->ingest_used + sizeof(rec)) > ctx->ingest_capacity) {
			rc = zdb_ts_flush_buffer_locked(ctx);
			if (rc < 0) {
				status = zdb_status_from_errno(rc);
				break;
			}
		}

		zdb_ts_record_encode(&samples[i], &rec);
		(void)memcpy(&ctx->ingest_buf[ctx->ingest_used], &rec, sizeof(rec));
		ctx->ingest_used += sizeof(rec);
#endif
	}

	zdb_unlock_write(ts->db);

#if !ZDB_TS_USE_FCB
	if ((status == ZDB_OK) && (ctx->ingest_used == ctx->ingest_capacity)) {
		(void)zdb_ts_flush_async(ts);
	}
#endif

#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	zdb_emit_ts_event(ts->db, ZDB_TS_EVENT_APPEND, ts->stream_name,
			  samples[0].ts_ms, samples[0].value, 0U, 0U, status);
#endif

	return status;
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

#if ZDB_TS_USE_FCB
	ARG_UNUSED(ctx);
	ARG_UNUSED(lock_rc);
	ARG_UNUSED(rc);
	return ZDB_OK;
#endif

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
#if ZDB_TS_USE_FCB
	ARG_UNUSED(timeout);
	if ((ts == NULL) || (ts->db == NULL)) {
		return ZDB_ERR_INVAL;
	}
	return ZDB_OK;
#else
	zdb_status_t rc;
	struct zdb_ts_core_ctx *ctx;
	int sem_rc;

	if ((ts == NULL) || (ts->db == NULL)) {
		return ZDB_ERR_INVAL;
	}

	ctx = zdb_ts_ctx_get_or_alloc(ts->db);
	if (ctx == NULL) {
		return ZDB_ERR_INTERNAL;
	}

	/* Drain any stale token from a previous flush cycle. */
	(void)k_sem_take(&ctx->flush_done, K_NO_WAIT);

	rc = zdb_ts_flush_async(ts);
	if ((rc != ZDB_OK) && (rc != ZDB_ERR_BUSY)) {
		return rc;
	}

	if (!ctx->flush_pending) {
		return ZDB_OK;
	}

	sem_rc = k_sem_take(&ctx->flush_done, timeout);
	if (sem_rc == -EAGAIN) {
		return ZDB_ERR_TIMEOUT;
	}
	if (sem_rc != 0) {
		return zdb_status_from_errno(sem_rc);
	}

	return ctx->flush_pending ? ZDB_ERR_BUSY : ZDB_OK;
#endif
}

zdb_status_t zdb_ts_query_aggregate(zdb_ts_t *ts, zdb_ts_window_t window,
			    zdb_ts_agg_t agg, zdb_ts_agg_result_t *out_result)
{
#if ZDB_TS_USE_FCB
	ARG_UNUSED(window);
	ARG_UNUSED(agg);
	ARG_UNUSED(out_result);
	if ((ts == NULL) || (ts->db == NULL)) {
		return ZDB_ERR_INVAL;
	}
	return ZDB_ERR_UNSUPPORTED;
#else
	zdb_cursor_t cursor;
	zdb_bytes_t record;
	struct zdb_ts_record_i64 rec;
	uint32_t points = 0U;
	double acc = 0.0;
	zdb_status_t rc;

	if ((ts == NULL) || (ts->db == NULL) || (out_result == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (agg > ZDB_TS_AGG_COUNT) {
		return ZDB_ERR_INVAL;
	}

	rc = zdb_ts_cursor_open(ts, window, NULL, NULL, &cursor);
	if (rc != ZDB_OK) {
		return rc;
	}

	while (points < CONFIG_ZDB_TS_MAX_AGG_POINTS) {
		uint64_t ts_ms;
		int64_t val;
		zdb_status_t dec_rc;

		rc = zdb_cursor_next(&cursor, &record);
		if (rc == ZDB_ERR_NOT_FOUND) {
			break;
		}
		if (rc != ZDB_OK) {
			(void)zdb_cursor_close(&cursor);
			return rc;
		}

		(void)memcpy(&rec, record.data, sizeof(rec));
		dec_rc = zdb_ts_record_decode(ts->db, &rec, &ts_ms, &val);
		if (dec_rc == ZDB_ERR_UNSUPPORTED) {
			continue;
		}
		if (dec_rc != ZDB_OK) {
			(void)zdb_cursor_close(&cursor);
			return dec_rc;
		}

		if (!zdb_ts_agg_update(agg, (double)val, &points, &acc)) {
			(void)zdb_cursor_close(&cursor);
			return ZDB_ERR_INVAL;
		}
	}

	(void)zdb_cursor_close(&cursor);

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
#endif
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

#if ZDB_TS_USE_LITTLEFS
	rc = zdb_ts_ensure_stream_header(ts->db, ts->stream_name);
	if (rc != ZDB_OK) {
		return rc;
	}
#else
	rc = ZDB_OK;
#endif

	if (k_mem_slab_alloc(ts->db->cursor_slab, (void **)&ctx, K_NO_WAIT) != 0) {
		return ZDB_ERR_NOMEM;
	}

	ctx->db = ts->db;
	ctx->stream_name = ts->stream_name;
	ctx->window = window;
	ctx->file_offset = sizeof(struct zdb_ts_stream_header);
	ctx->ram_offset = 0U;
	ctx->file_done = false;
#if ZDB_TS_USE_LITTLEFS
	{
		char cursor_path[ZDB_TS_PATH_MAX];
		int open_rc;

		fs_file_t_init(&ctx->file);
		ctx->file_open = false;
		open_rc = zdb_ts_build_path(ts->db->cfg, ts->stream_name,
					    cursor_path, sizeof(cursor_path));
		if (open_rc < 0) {
			k_mem_slab_free(ts->db->cursor_slab, ctx);
			return zdb_status_from_errno(open_rc);
		}
		open_rc = fs_open(&ctx->file, cursor_path, FS_O_READ);
		if (open_rc != 0) {
			k_mem_slab_free(ts->db->cursor_slab, ctx);
			return zdb_status_from_errno(open_rc);
		}
		ctx->file_open = true;
		open_rc = fs_seek(&ctx->file,
				  (off_t)ctx->file_offset,
				  FS_SEEK_SET);
		if (open_rc != 0) {
			(void)fs_close(&ctx->file);
			k_mem_slab_free(ts->db->cursor_slab, ctx);
			return zdb_status_from_errno(open_rc);
		}
	}
#endif
#if ZDB_TS_USE_FCB
	ctx->fcb_started = false;
	(void)memset(&ctx->fcb_loc, 0, sizeof(ctx->fcb_loc));
	ctx->file_offset = 0U;
	ctx->file_done = true;
#endif

	out_cursor->model = ZDB_MODEL_TS;

#if ZDB_TS_USE_FCB
	out_cursor->backend = ZDB_BACKEND_FCB;
#else
	out_cursor->backend = ZDB_BACKEND_LFS;
#endif
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
#if ZDB_TS_USE_LITTLEFS
	zdb_status_t lock_rc;
#endif

	if ((cursor == NULL) || (out_record == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if ((cursor->model != ZDB_MODEL_TS) || (cursor->impl == NULL)) {
		return ZDB_ERR_UNSUPPORTED;
	}

	cctx = (struct zdb_ts_cursor_ctx *)cursor->impl;

#if ZDB_TS_USE_FCB
	if (cursor->backend == ZDB_BACKEND_FCB) {
		tctx = zdb_ts_ctx_get_or_alloc(cctx->db);
		if (tctx == NULL) {
			return ZDB_ERR_INVAL;
		}

		while (true) {
			uint64_t ts_ms;
			int64_t val;
			zdb_status_t dec_rc;
			zdb_status_t rc_fcb = zdb_ts_fcb_cursor_read_record(tctx, cctx, &candidate);

			if (rc_fcb == ZDB_ERR_NOT_FOUND) {
				out_record->data = NULL;
				out_record->len = 0U;
				return ZDB_ERR_NOT_FOUND;
			}
			if (rc_fcb != ZDB_OK) {
				return rc_fcb;
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
	}
#endif

#if ZDB_TS_USE_LITTLEFS
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
#else
	out_record->data = NULL;
	out_record->len = 0U;
	return ZDB_ERR_UNSUPPORTED;
#endif
}

zdb_status_t zdb_ts_recover_stream(zdb_ts_t *ts, size_t *out_truncated_bytes)
{
#if ZDB_TS_USE_FCB
	if ((ts == NULL) || (ts->db == NULL) || (ts->stream_name == NULL)) {
		return ZDB_ERR_INVAL;
	}
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	zdb_emit_ts_event(ts->db, ZDB_TS_EVENT_RECOVER, ts->stream_name, 0U, 0, 0U, 0U, ZDB_OK);
#endif
	if (out_truncated_bytes != NULL) {
		*out_truncated_bytes = 0U;
	}
	return ZDB_OK;
#else
	struct fs_file_t file;
	struct zdb_ts_stream_header hdr;
	struct zdb_ts_record_i64 rec;
	char path[ZDB_TS_PATH_MAX];
	size_t good_end;
	size_t truncated_bytes = 0U;
	int rc;
	ssize_t rd;

	if ((ts == NULL) || (ts->db == NULL) || (ts->stream_name == NULL)) {
		return ZDB_ERR_INVAL;
	}

	ZDB_STAT_INC(ts->db, recover_runs);

	if (out_truncated_bytes != NULL) {
		*out_truncated_bytes = 0U;
	}

	rc = zdb_ts_build_path(ts->db->cfg, ts->stream_name, path, sizeof(path));
	if (rc < 0) {
		ZDB_STAT_INC(ts->db, recover_failures);
		zdb_health_check(ts->db);
		return zdb_status_from_errno(rc);
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_RDWR);
	if (rc < 0) {
		ZDB_STAT_INC(ts->db, recover_failures);
		zdb_health_check(ts->db);
		return zdb_status_from_errno(rc);
	}

	rd = fs_read(&file, &hdr, sizeof(hdr));
	if (rd == 0) {
		zdb_ts_stream_header_encode(ts->stream_name, &hdr);
		rc = fs_seek(&file, 0, FS_SEEK_SET);
		if (rc < 0) {
			(void)fs_close(&file);
			ZDB_STAT_INC(ts->db, recover_failures);
			zdb_health_check(ts->db);
			return zdb_status_from_errno(rc);
		}
		rd = fs_write(&file, &hdr, sizeof(hdr));
		(void)fs_close(&file);
		if ((rd < 0) || ((size_t)rd != sizeof(hdr))) {
			ZDB_STAT_INC(ts->db, recover_failures);
			zdb_health_check(ts->db);
			return (rd < 0) ? zdb_status_from_errno((int)rd) : ZDB_ERR_IO;
		}
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
		zdb_emit_ts_event(ts->db, ZDB_TS_EVENT_RECOVER, ts->stream_name, 0U, 0, 0U, 0U,
				  ZDB_OK);
#endif
		return ZDB_OK;
	}

	if ((rd < 0) || ((size_t)rd != sizeof(hdr))) {
		(void)fs_close(&file);
		ZDB_STAT_INC(ts->db, recover_failures);
		zdb_health_check(ts->db);
		return (rd < 0) ? zdb_status_from_errno((int)rd) : ZDB_ERR_CORRUPT;
	}

	{
		zdb_status_t dec = zdb_ts_stream_header_decode(ts->db, &hdr, ts->stream_name);
		if (dec != ZDB_OK) {
			(void)fs_close(&file);
			ZDB_STAT_INC(ts->db, recover_failures);
			zdb_health_check(ts->db);
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
			ZDB_STAT_INC(ts->db, recover_failures);
			zdb_health_check(ts->db);
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
			ZDB_STAT_INC(ts->db, recover_failures);
			zdb_health_check(ts->db);
			return zdb_status_from_errno((int)end_pos);
		}

		if ((size_t)end_pos > good_end) {
			rc = fs_truncate(&file, (off_t)good_end);
			if (rc < 0) {
				(void)fs_close(&file);
				ZDB_STAT_INC(ts->db, recover_failures);
				zdb_health_check(ts->db);
				return zdb_status_from_errno(rc);
			}

			truncated_bytes = (size_t)end_pos - good_end;
			if (out_truncated_bytes != NULL) {
				*out_truncated_bytes = truncated_bytes;
			}
			ZDB_STAT_ADD(ts->db, recover_truncated_bytes, (uint64_t)truncated_bytes);
		}
	}

	(void)fs_close(&file);
#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	zdb_emit_ts_event(ts->db, ZDB_TS_EVENT_RECOVER, ts->stream_name, 0U, 0, 0U,
			  truncated_bytes, ZDB_OK);
#endif
	return ZDB_OK;
#endif
}

#endif /* CONFIG_ZDB_TS */
