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

/* Used for filesystem-backed stream paths, which FCB does not build. */
__maybe_unused static uint32_t zdb_fnv1a32(const char *s)
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

#if ZDB_TS_USE_LITTLEFS
#if !ZDB_TS_SEGMENTED
static zdb_status_t zdb_ts_ensure_header_at(zdb_t *db, const char *stream_name,
					    const char *path);
#endif
static int zdb_ts_active_path(zdb_t *db, struct zdb_ts_stream_ctx *slot, char *path,
			      size_t path_len);
#if ZDB_TS_SEGMENTED
static int zdb_ts_build_segment_path(const zdb_cfg_t *cfg, const char *stream_name,
				     uint32_t seq, char *path, size_t path_len);
static zdb_status_t zdb_ts_scan_segments(zdb_t *db, struct zdb_ts_stream_ctx *slot);
static int zdb_ts_roll_segment_if_full(zdb_t *db, struct zdb_ts_stream_ctx *slot);
#endif
#endif

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
#if ZDB_TS_SEGMENTED
		/* Discard the oldest sector to make room. */
		rc = fcb_rotate(&ctx->ts_fcb);
		if (rc < 0) {
			return rc;
		}
		rc = fcb_append(&ctx->ts_fcb, (uint16_t)sizeof(*rec), &loc);
#else
		/* Preserve what is stored and refuse the append. */
		return -ENOSPC;
#endif
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
#if ZDB_TS_USE_LITTLEFS
/*
 * Path of a stream's consumed-watermark sidecar.
 *
 * Kept beside the stream rather than inside it: the stream file is append-only
 * so that recovery can trust its tail, and a watermark is rewritten in place
 * every time a consumer acknowledges.
 */
static int zdb_ts_build_watermark_path(const zdb_cfg_t *cfg, const char *stream_name,
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

	n = snprintf(path, path_len, "%s/%s/%s.wmk", cfg->lfs_mount_point,
		     CONFIG_ZDB_TS_DIRNAME, stream_name);
	if ((n < 0) || ((size_t)n >= path_len)) {
		return -ENAMETOOLONG;
	}

	return 0;
}
#endif

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

#if ZDB_TS_USE_LITTLEFS
/* Record size of the segment a stream is appending to. */
static size_t zdb_ts_slot_rec_size(const struct zdb_ts_stream_ctx *slot)
{
#if ZDB_TS_SEGMENTED
	return (slot->seg_rec_size != 0U) ? slot->seg_rec_size
					  : sizeof(struct zdb_ts_record_i64);
#else
	ARG_UNUSED(slot);
	return sizeof(struct zdb_ts_record_i64);
#endif
}

static uint16_t zdb_ts_slot_version(const struct zdb_ts_stream_ctx *slot)
{
#if ZDB_TS_SEGMENTED
	return (slot->seg_version != 0U) ? slot->seg_version : (uint16_t)ZDB_TS_STREAM_VERSION;
#else
	ARG_UNUSED(slot);
	return (uint16_t)ZDB_TS_STREAM_VERSION;
#endif
}

static uint64_t zdb_ts_slot_base_ts(const struct zdb_ts_stream_ctx *slot)
{
#if ZDB_TS_SEGMENTED
	return slot->seg_base_ts_ms;
#else
	ARG_UNUSED(slot);
	return 0U;
#endif
}
#endif /* ZDB_TS_USE_LITTLEFS */

static int zdb_ts_flush_buffer_locked(struct zdb_ts_core_ctx *ctx,
				      struct zdb_ts_stream_ctx *slot)
{
	struct fs_file_t file;
	char path[ZDB_TS_PATH_MAX];
	ssize_t wr;
	int rc;

	if ((ctx == NULL) || (ctx->db == NULL) || (ctx->db->cfg == NULL)) {
		return -EINVAL;
	}

	if ((slot == NULL) || !slot->in_use || (slot->ingest_buf == NULL)) {
		return -EINVAL;
	}

	if (slot->ingest_used == 0U) {
		return 0;
	}

	rc = zdb_ts_active_path(ctx->db, slot, path, sizeof(path));
	if (rc < 0) {
		return rc;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (rc < 0) {
		return rc;
	}

	wr = fs_write(&file, slot->ingest_buf, slot->ingest_used);
	if ((wr < 0) || ((size_t)wr != slot->ingest_used)) {
		(void)fs_close(&file);
		return (wr < 0) ? (int)wr : -EIO;
	}

	rc = fs_close(&file);
	if (rc < 0) {
		return rc;
	}

#if ZDB_TS_SEGMENTED
	slot->cur_seg_bytes += slot->ingest_used;
#endif
	slot->ingest_used = 0U;

#if ZDB_TS_SEGMENTED
	/* Roll after writing, so a flush never spans two segments. */
	rc = zdb_ts_roll_segment_if_full(ctx->db, slot);
	if (rc < 0) {
		return rc;
	}
#endif

	return 0;
}

/*
 * Flush every stream holding buffered samples.
 *
 * Returns the first failure, having attempted the rest: one stream's write
 * error must not strand another stream's data in RAM. A failed stream keeps
 * its buffer for the next attempt.
 */
static int zdb_ts_flush_all_locked(struct zdb_ts_core_ctx *ctx, size_t *out_flushed_bytes,
				   const char **out_only_stream)
{
	int first_error = 0;
	size_t flushed_streams = 0U;
	size_t i;

	if (out_flushed_bytes != NULL) {
		*out_flushed_bytes = 0U;
	}
	if (out_only_stream != NULL) {
		*out_only_stream = NULL;
	}

	for (i = 0U; i < ARRAY_SIZE(ctx->streams); i++) {
		struct zdb_ts_stream_ctx *slot = &ctx->streams[i];
		size_t pending;
		int rc;

		if (!slot->in_use || (slot->ingest_used == 0U)) {
			continue;
		}

		pending = slot->ingest_used;
		rc = zdb_ts_flush_buffer_locked(ctx, slot);
		if (rc < 0) {
			if (first_error == 0) {
				first_error = rc;
			}
			continue;
		}

		if (out_flushed_bytes != NULL) {
			*out_flushed_bytes += pending;
		}

		/*
		 * Name the stream when exactly one was flushed, which is the
		 * usual case; a flush spanning several names none of them.
		 */
		flushed_streams++;
		if ((out_only_stream != NULL) && (flushed_streams == 1U)) {
			*out_only_stream = slot->name;
		} else if (out_only_stream != NULL) {
			*out_only_stream = NULL;
		}
	}

	return first_error;
}
#endif

#if ZDB_TS_USE_LITTLEFS
#if ZDB_TS_SEGMENTED
/* Bytes each supported record format occupies on storage. */
static size_t zdb_ts_rec_size_for(uint16_t version)
{
	return (version == ZDB_TS_STREAM_VERSION_V2) ? sizeof(struct zdb_ts_record_v2)
						     : sizeof(struct zdb_ts_record_i64);
}

/* Bytes each supported segment header occupies. */
static size_t zdb_ts_hdr_size_for(uint16_t version)
{
	return (version == ZDB_TS_STREAM_VERSION_V2) ? sizeof(struct zdb_ts_stream_header_v2)
						     : sizeof(struct zdb_ts_stream_header);
}
#endif /* ZDB_TS_SEGMENTED */

/*
 * Encode a sample in @p version's layout into @p out, which must have room for
 * zdb_ts_rec_size_for(version) bytes.
 *
 * A compact record stores the timestamp as an offset from @p base_ts_ms.
 * Reports false when the sample cannot be expressed that way — before the base,
 * or more than a 32-bit millisecond span past it — which tells the caller to
 * start a segment with a new base.
 */
static bool zdb_ts_record_encode_as(uint16_t version, uint64_t base_ts_ms,
				    const zdb_ts_sample_i64_t *sample, uint8_t *out)
{
	if (version != ZDB_TS_STREAM_VERSION_V2) {
		zdb_ts_record_encode(sample, (struct zdb_ts_record_i64 *)out);
		return true;
	}

	{
		struct zdb_ts_record_v2 rec;
		uint64_t delta;

		if (sample->ts_ms < base_ts_ms) {
			return false;
		}
		delta = sample->ts_ms - base_ts_ms;
		if (delta > UINT32_MAX) {
			return false;
		}

		rec.ts_delta_ms_le = sys_cpu_to_le32((uint32_t)delta);
		rec.value_le = sys_cpu_to_le64((uint64_t)sample->value);
		rec.crc_le = sys_cpu_to_le32(crc32_ieee(
			(const uint8_t *)&rec, offsetof(struct zdb_ts_record_v2, crc_le)));
		(void)memcpy(out, &rec, sizeof(rec));
	}

	return true;
}

/*
 * Decode a record of @p version. A compact record has no magic or version of
 * its own — the segment header carries both — so its CRC is the whole check.
 */
static zdb_status_t zdb_ts_record_decode_as(zdb_t *db, uint16_t version, uint64_t base_ts_ms,
					    const uint8_t *raw, uint64_t *out_ts_ms,
					    int64_t *out_value)
{
	if (version != ZDB_TS_STREAM_VERSION_V2) {
		struct zdb_ts_record_i64 rec;

		(void)memcpy(&rec, raw, sizeof(rec));
		return zdb_ts_record_decode(db, &rec, out_ts_ms, out_value);
	}

	{
		struct zdb_ts_record_v2 rec;
		uint32_t expect_crc;

		(void)memcpy(&rec, raw, sizeof(rec));
		expect_crc = crc32_ieee((const uint8_t *)&rec,
					offsetof(struct zdb_ts_record_v2, crc_le));
		if (sys_le32_to_cpu(rec.crc_le) != expect_crc) {
			ZDB_STAT_INC(db, crc_failures);
			ZDB_STAT_INC(db, corrupt_records);
			zdb_health_check(db);
			return ZDB_ERR_CORRUPT;
		}

		*out_ts_ms = base_ts_ms + (uint64_t)sys_le32_to_cpu(rec.ts_delta_ms_le);
		*out_value = (int64_t)sys_le64_to_cpu(rec.value_le);
	}

	return ZDB_OK;
}
#endif /* ZDB_TS_USE_LITTLEFS */

/* Only the aggregate scan uses this, and FCB has no aggregate scan. */
__maybe_unused static bool zdb_ts_agg_update(zdb_ts_agg_t agg, double sample, uint32_t *points, double *acc)
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
/*
 * Read a file's header to learn which record layout it holds, so the cursor
 * uses the right stride and timestamp base. Files of either format are read.
 */
static zdb_status_t zdb_ts_cursor_learn_layout(struct zdb_ts_cursor_ctx *cctx,
					       const char *path)
{
	struct fs_file_t hdr_file;
	uint8_t raw[ZDB_TS_HDR_MAX_SIZE];
	struct zdb_ts_stream_header hdr;
	uint16_t version;
	ssize_t rd;
	int rc;

	cctx->hdr_size = sizeof(struct zdb_ts_stream_header);
	cctx->rec_size = sizeof(struct zdb_ts_record_i64);
	cctx->base_ts_ms = 0U;

	fs_file_t_init(&hdr_file);
	rc = fs_open(&hdr_file, path, FS_O_READ);
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	rd = fs_read(&hdr_file, raw, sizeof(raw));
	(void)fs_close(&hdr_file);

	if (rd < (ssize_t)sizeof(struct zdb_ts_stream_header)) {
		/* Too short to hold a header; the walk finds no records. */
		return ZDB_OK;
	}

	(void)memcpy(&hdr, raw, sizeof(hdr));
	version = sys_le16_to_cpu(hdr.version_le);
	if (version != ZDB_TS_STREAM_VERSION_V2) {
		return ZDB_OK;
	}

	if (rd < (ssize_t)sizeof(struct zdb_ts_stream_header_v2)) {
		return ZDB_ERR_CORRUPT;
	}

	{
		struct zdb_ts_stream_header_v2 h2;

		(void)memcpy(&h2, raw, sizeof(h2));
		cctx->hdr_size = sizeof(h2);
		cctx->rec_size = sizeof(struct zdb_ts_record_v2);
		cctx->base_ts_ms = sys_le64_to_cpu(h2.base_ts_ms_le);
	}

	return ZDB_OK;
}
#endif /* ZDB_TS_USE_LITTLEFS */

#if ZDB_TS_USE_LITTLEFS
#if ZDB_TS_USE_LITTLEFS && ZDB_TS_SEGMENTED
/*
 * Point the cursor's file handle at one segment, positioned for the walk's
 * direction. Reports ZDB_ERR_NOT_FOUND when that segment is gone, which is
 * what a segment discarded mid-walk looks like.
 */
static zdb_status_t zdb_ts_cursor_open_segment(struct zdb_ts_cursor_ctx *cctx, uint32_t seq)
{
	char path[ZDB_TS_PATH_MAX];
	int rc;

	if (cctx->file_open) {
		(void)fs_close(&cctx->file);
		cctx->file_open = false;
	}

	rc = zdb_ts_build_segment_path(cctx->db->cfg, cctx->stream_name, seq, path, sizeof(path));
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	fs_file_t_init(&cctx->file);
	rc = fs_open(&cctx->file, path, FS_O_READ);
	if (rc < 0) {
		return (rc == -ENOENT) ? ZDB_ERR_NOT_FOUND : zdb_status_from_errno(rc);
	}
	cctx->file_open = true;
	cctx->cur_seg = seq;

	{
		zdb_status_t layout_rc = zdb_ts_cursor_learn_layout(cctx, path);

		if (layout_rc != ZDB_OK) {
			return layout_rc;
		}
	}

	if (cctx->descending) {
		off_t end = fs_seek(&cctx->file, 0, FS_SEEK_END);

		if (end < 0) {
			return zdb_status_from_errno((int)end);
		}
		cctx->file_size = (size_t)fs_tell(&cctx->file);
		/* Start at the last whole record, ignoring any partial tail. */
		cctx->file_offset = cctx->hdr_size +
				    (((cctx->file_size - cctx->hdr_size) / cctx->rec_size) *
				     cctx->rec_size);
	} else {
		cctx->file_offset = cctx->hdr_size;
	}

	rc = fs_seek(&cctx->file, (off_t)cctx->file_offset, FS_SEEK_SET);
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	return ZDB_OK;
}

/*
 * Move to the next segment in the walk's direction, skipping any that have
 * been discarded. Reports ZDB_ERR_NOT_FOUND once the window is exhausted.
 */
static zdb_status_t zdb_ts_cursor_next_segment(struct zdb_ts_cursor_ctx *cctx)
{
	for (;;) {
		uint32_t next;

		if (cctx->descending) {
			if (cctx->cur_seg == cctx->seg_lo) {
				return ZDB_ERR_NOT_FOUND;
			}
			next = cctx->cur_seg - 1U;
		} else {
			if (cctx->cur_seg >= cctx->seg_hi) {
				return ZDB_ERR_NOT_FOUND;
			}
			next = cctx->cur_seg + 1U;
		}

		if (zdb_ts_cursor_open_segment(cctx, next) == ZDB_OK) {
			return ZDB_OK;
		}

		/* Segment gone; keep stepping rather than ending the walk. */
		cctx->cur_seg = next;
	}
}
#endif /* ZDB_TS_USE_LITTLEFS && ZDB_TS_SEGMENTED */

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

	if (cctx->descending) {
		/*
		 * Step back one record and read it. Records are fixed size, so
		 * the previous record always starts exactly one stride back;
		 * the stream header bounds the walk.
		 */
		if (cctx->file_offset < (cctx->hdr_size + cctx->rec_size)) {
#if ZDB_TS_SEGMENTED
			if (zdb_ts_cursor_next_segment(cctx) == ZDB_OK) {
				return zdb_ts_cursor_read_file_record(cctx, out_record);
			}
#endif
			return ZDB_ERR_NOT_FOUND;
		}

		cctx->file_offset -= cctx->rec_size;
		if (fs_seek(&cctx->file, (off_t)cctx->file_offset, FS_SEEK_SET) < 0) {
			return ZDB_ERR_IO;
		}
	}

	rd = fs_read(&cctx->file, &cctx->cache, cctx->rec_size);

	if (rd == 0) {
#if ZDB_TS_SEGMENTED
		if (zdb_ts_cursor_next_segment(cctx) == ZDB_OK) {
			return zdb_ts_cursor_read_file_record(cctx, out_record);
		}
#endif
		return ZDB_ERR_NOT_FOUND;
	}
	if (rd < 0) {
		return zdb_status_from_errno((int)rd);
	}
	if ((size_t)rd != cctx->rec_size) {
		return ZDB_ERR_CORRUPT;
	}

	if (cctx->descending) {
		/* The read advanced the handle; leave the offset at this record. */
		if (fs_seek(&cctx->file, (off_t)cctx->file_offset, FS_SEEK_SET) < 0) {
			return ZDB_ERR_IO;
		}
	} else {
		cctx->file_offset += cctx->rec_size;
	}
	out_record->data = (const uint8_t *)&cctx->cache;
	out_record->len = cctx->rec_size;
	return ZDB_OK;
}

/*
 * Create or validate a segment header, reporting the format the file uses.
 *
 * A new file is written in the configured format. An existing one keeps
 * whatever it was created with, so records inside a file never change size.
 * @p out_version and @p out_base_ts may be NULL.
 */
static zdb_status_t zdb_ts_ensure_header_ex(zdb_t *db, const char *stream_name,
					    const char *path, uint64_t new_base_ts_ms,
					    uint16_t *out_version, uint64_t *out_base_ts)
{
	struct fs_file_t file;
	uint8_t raw[ZDB_TS_HDR_MAX_SIZE];
	struct zdb_ts_stream_header hdr;
	uint16_t version;
	uint64_t base_ts = 0U;
	ssize_t rd;
	int rc;

	if ((db == NULL) || (db->cfg == NULL) || (stream_name == NULL) || (path == NULL)) {
		return ZDB_ERR_INVAL;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_RDWR);
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	rd = fs_read(&file, raw, sizeof(raw));
	if (rd == 0) {
		ssize_t wr;
		size_t hdr_len;

#if ZDB_TS_DELTA_ENABLED
		{
			struct zdb_ts_stream_header_v2 h2;

			h2.magic_le = sys_cpu_to_le32(ZDB_TS_STREAM_MAGIC);
			h2.version_le = sys_cpu_to_le16(ZDB_TS_STREAM_VERSION_V2);
			h2.reserved_le = 0U;
			h2.stream_id_le = sys_cpu_to_le32(zdb_fnv1a32(stream_name));
			h2.base_ts_ms_le = sys_cpu_to_le64(new_base_ts_ms);
			h2.crc_le = sys_cpu_to_le32(crc32_ieee(
				(const uint8_t *)&h2,
				offsetof(struct zdb_ts_stream_header_v2, crc_le)));
			(void)memcpy(raw, &h2, sizeof(h2));
			hdr_len = sizeof(h2);
			version = ZDB_TS_STREAM_VERSION_V2;
			base_ts = new_base_ts_ms;
		}
#else
		ARG_UNUSED(new_base_ts_ms);
		zdb_ts_stream_header_encode(stream_name, &hdr);
		(void)memcpy(raw, &hdr, sizeof(hdr));
		hdr_len = sizeof(hdr);
		version = ZDB_TS_STREAM_VERSION;
#endif
		rc = fs_seek(&file, 0, FS_SEEK_SET);
		if (rc < 0) {
			(void)fs_close(&file);
			return zdb_status_from_errno(rc);
		}
		wr = fs_write(&file, raw, hdr_len);
		if ((wr < 0) || ((size_t)wr != hdr_len)) {
			(void)fs_close(&file);
			return (wr < 0) ? zdb_status_from_errno((int)wr) : ZDB_ERR_IO;
		}
		(void)fs_close(&file);
		if (out_version != NULL) {
			*out_version = version;
		}
		if (out_base_ts != NULL) {
			*out_base_ts = base_ts;
		}
		return ZDB_OK;
	}

	if (rd < 0) {
		(void)fs_close(&file);
		return zdb_status_from_errno((int)rd);
	}

	if ((size_t)rd < sizeof(struct zdb_ts_stream_header)) {
		(void)fs_close(&file);
		return ZDB_ERR_CORRUPT;
	}

	(void)fs_close(&file);

	/* Existing file: its own header says which layout it holds. */
	(void)memcpy(&hdr, raw, sizeof(hdr));
	version = sys_le16_to_cpu(hdr.version_le);

	if (version == ZDB_TS_STREAM_VERSION_V2) {
		struct zdb_ts_stream_header_v2 h2;
		uint32_t expect_crc;

		if ((size_t)rd < sizeof(h2)) {
			return ZDB_ERR_CORRUPT;
		}
		(void)memcpy(&h2, raw, sizeof(h2));

		if (sys_le32_to_cpu(h2.magic_le) != ZDB_TS_STREAM_MAGIC) {
			ZDB_STAT_INC(db, corrupt_records);
			return ZDB_ERR_CORRUPT;
		}
		expect_crc = crc32_ieee((const uint8_t *)&h2,
					offsetof(struct zdb_ts_stream_header_v2, crc_le));
		if (sys_le32_to_cpu(h2.crc_le) != expect_crc) {
			ZDB_STAT_INC(db, crc_failures);
			ZDB_STAT_INC(db, corrupt_records);
			return ZDB_ERR_CORRUPT;
		}
		if (sys_le32_to_cpu(h2.stream_id_le) != zdb_fnv1a32(stream_name)) {
			ZDB_STAT_INC(db, corrupt_records);
			return ZDB_ERR_CORRUPT;
		}

		base_ts = sys_le64_to_cpu(h2.base_ts_ms_le);
	} else {
		zdb_status_t decode_rc = zdb_ts_stream_header_decode(db, &hdr, stream_name);

		if (decode_rc != ZDB_OK) {
			return decode_rc;
		}
	}

	if (out_version != NULL) {
		*out_version = version;
	}
	if (out_base_ts != NULL) {
		*out_base_ts = base_ts;
	}
	return ZDB_OK;
}

#if !ZDB_TS_SEGMENTED
static zdb_status_t zdb_ts_ensure_header_at(zdb_t *db, const char *stream_name,
					    const char *path)
{
	return zdb_ts_ensure_header_ex(db, stream_name, path, 0U, NULL, NULL);
}
#endif

#if ZDB_TS_SEGMENTED
/*
 * Path of one segment of a stream.
 *
 * Segments are numbered monotonically, so their order is their name order and
 * discarding the oldest is a single unlink.
 */
static int zdb_ts_build_segment_path(const zdb_cfg_t *cfg, const char *stream_name,
				     uint32_t seq, char *path, size_t path_len)
{
	int n;

	if ((cfg == NULL) || (cfg->lfs_mount_point == NULL) || (stream_name == NULL) ||
	    (path == NULL) || (path_len == 0U)) {
		return -EINVAL;
	}

	if (!zdb_ts_stream_name_valid(stream_name)) {
		return -EINVAL;
	}

	n = snprintf(path, path_len, "%s/%s/%s.%04u.zts", cfg->lfs_mount_point,
		     CONFIG_ZDB_TS_DIRNAME, stream_name, (unsigned)seq);
	if ((n < 0) || ((size_t)n >= path_len)) {
		return -ENAMETOOLONG;
	}

	return 0;
}

/*
 * Learn a stream's segment window from storage.
 *
 * The lowest and highest segment numbers present bound the retained window. A
 * stream written as a single <stream>.zts by a build without rollover is
 * adopted as segment 0, so switching the option on does not strand its data.
 */
static zdb_status_t zdb_ts_scan_segments(zdb_t *db, struct zdb_ts_stream_ctx *slot)
{
	char dir_path[ZDB_TS_PATH_MAX];
	char seg_path[ZDB_TS_PATH_MAX];
	struct fs_dir_t dir;
	struct fs_dirent entry;
	struct fs_dirent info;
	size_t name_len;
	uint32_t lowest = UINT32_MAX;
	uint32_t highest = 0U;
	bool found = false;
	zdb_status_t hdr_rc;
	int rc;
	int n;

	if (slot->segs_scanned) {
		return ZDB_OK;
	}

	n = snprintf(dir_path, sizeof(dir_path), "%s/%s", db->cfg->lfs_mount_point,
		     CONFIG_ZDB_TS_DIRNAME);
	if ((n < 0) || ((size_t)n >= sizeof(dir_path))) {
		return ZDB_ERR_INVAL;
	}

	name_len = strlen(slot->name);

	fs_dir_t_init(&dir);
	rc = fs_opendir(&dir, dir_path);
	if (rc == 0) {
		for (;;) {
			unsigned int seq;

			rc = fs_readdir(&dir, &entry);
			if ((rc < 0) || (entry.name[0] == '\0')) {
				break;
			}
			if (entry.type != FS_DIR_ENTRY_FILE) {
				continue;
			}
			/* Match "<stream>.NNNN.zts" exactly. */
			if ((strncmp(entry.name, slot->name, name_len) != 0) ||
			    (entry.name[name_len] != '.')) {
				continue;
			}
			if (sscanf(&entry.name[name_len + 1U], "%4u.zts", &seq) != 1) {
				continue;
			}

			found = true;
			if ((uint32_t)seq < lowest) {
				lowest = (uint32_t)seq;
			}
			if ((uint32_t)seq > highest) {
				highest = (uint32_t)seq;
			}
		}
		(void)fs_closedir(&dir);
	}

	if (!found) {
		char legacy_path[ZDB_TS_PATH_MAX];

		rc = zdb_ts_build_path(db->cfg, slot->name, legacy_path, sizeof(legacy_path));
		if (rc < 0) {
			return zdb_status_from_errno(rc);
		}
		rc = zdb_ts_build_segment_path(db->cfg, slot->name, 0U, seg_path,
					       sizeof(seg_path));
		if (rc < 0) {
			return zdb_status_from_errno(rc);
		}

		if (fs_stat(legacy_path, &info) == 0) {
			rc = fs_rename(legacy_path, seg_path);
			if (rc < 0) {
				return zdb_status_from_errno(rc);
			}
		}

		lowest = 0U;
		highest = 0U;
	}

	slot->oldest_seg = lowest;
	slot->cur_seg = highest;

	rc = zdb_ts_build_segment_path(db->cfg, slot->name, slot->cur_seg, seg_path,
				       sizeof(seg_path));
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	{
		uint16_t seg_version = (uint16_t)ZDB_TS_STREAM_VERSION;
		uint64_t seg_base = 0U;

		hdr_rc = zdb_ts_ensure_header_ex(db, slot->name, seg_path, 0U, &seg_version,
						 &seg_base);
		if (hdr_rc != ZDB_OK) {
			return hdr_rc;
		}

		slot->seg_version = seg_version;
		slot->seg_hdr_size = zdb_ts_hdr_size_for(seg_version);
		slot->seg_rec_size = zdb_ts_rec_size_for(seg_version);
		slot->seg_base_ts_ms = seg_base;
	}

	slot->cur_seg_bytes = 0U;
	if (fs_stat(seg_path, &info) == 0) {
		if ((size_t)info.size > slot->seg_hdr_size) {
			slot->cur_seg_bytes = (size_t)info.size - slot->seg_hdr_size;
		}
	}

	slot->segs_scanned = true;
	return ZDB_OK;
}

/*
 * Start a new segment once the newest is full, discarding the oldest when the
 * stream already holds its full complement.
 *
 * Called after a write rather than before, so a flush is never split across
 * two files; a segment therefore overshoots its configured size by at most one
 * ingest buffer.
 */
/*
 * Begin a new segment, giving it @p base_ts_ms as the base its records offset
 * from, and drop the oldest once the stream holds its full complement.
 */
static int zdb_ts_start_segment(zdb_t *db, struct zdb_ts_stream_ctx *slot,
				uint64_t base_ts_ms)
{
	char seg_path[ZDB_TS_PATH_MAX];
	uint16_t seg_version = (uint16_t)ZDB_TS_STREAM_VERSION;
	uint64_t seg_base = 0U;
	zdb_status_t hdr_rc;
	int rc;

	slot->cur_seg++;
	slot->cur_seg_bytes = 0U;

	rc = zdb_ts_build_segment_path(db->cfg, slot->name, slot->cur_seg, seg_path,
				       sizeof(seg_path));
	if (rc < 0) {
		return rc;
	}

	hdr_rc = zdb_ts_ensure_header_ex(db, slot->name, seg_path, base_ts_ms, &seg_version,
					 &seg_base);
	if (hdr_rc != ZDB_OK) {
		return -EIO;
	}

	slot->seg_version = seg_version;
	slot->seg_hdr_size = zdb_ts_hdr_size_for(seg_version);
	slot->seg_rec_size = zdb_ts_rec_size_for(seg_version);
	slot->seg_base_ts_ms = seg_base;

#if ZDB_TS_ROLLOVER_ENABLED
	/* Discarding the oldest segment is rollover's job, not segmentation's. */
	while ((slot->cur_seg - slot->oldest_seg + 1U) >
	       (uint32_t)CONFIG_ZDB_TS_ROLLOVER_MAX_SEGMENTS) {
		rc = zdb_ts_build_segment_path(db->cfg, slot->name, slot->oldest_seg, seg_path,
					       sizeof(seg_path));
		if (rc < 0) {
			return rc;
		}
		rc = fs_unlink(seg_path);
		if ((rc < 0) && (rc != -ENOENT)) {
			return rc;
		}
		slot->oldest_seg++;
	}
#endif

	return 0;
}

static int zdb_ts_roll_segment_if_full(zdb_t *db, struct zdb_ts_stream_ctx *slot)
{
	if (slot->cur_seg_bytes < (size_t)CONFIG_ZDB_TS_ROLLOVER_SEGMENT_BYTES) {
		return 0;
	}

	/* Carry the base forward; an out-of-range sample rebases on its own. */
	return zdb_ts_start_segment(db, slot, slot->seg_base_ts_ms);
}
#endif /* ZDB_TS_SEGMENTED */

/*
 * Path a stream currently appends to: its newest segment when rollover bounds
 * the stream, otherwise its single file.
 */
static int zdb_ts_active_path(zdb_t *db, struct zdb_ts_stream_ctx *slot, char *path,
			      size_t path_len)
{
#if ZDB_TS_SEGMENTED
	if (zdb_ts_scan_segments(db, slot) != ZDB_OK) {
		return -EIO;
	}
	return zdb_ts_build_segment_path(db->cfg, slot->name, slot->cur_seg, path, path_len);
#else
	return zdb_ts_build_path(db->cfg, slot->name, path, path_len);
#endif
}

#if !ZDB_TS_SEGMENTED
/* Single-file layout wrapper; segmented streams write a header per segment. */
static zdb_status_t zdb_ts_ensure_stream_header(zdb_t *db, const char *stream_name)
{
	char path[ZDB_TS_PATH_MAX];
	int rc;

	if ((db == NULL) || (db->cfg == NULL)) {
		return ZDB_ERR_INVAL;
	}

	rc = zdb_ts_build_path(db->cfg, stream_name, path, sizeof(path));
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	return zdb_ts_ensure_header_at(db, stream_name, path);
}
#endif /* !ZDB_TS_SEGMENTED */
#endif

/*
 * True when the window admits every timestamp, matching the conventions
 * zdb_ts_window_match() accepts: an all-zero window, and an open-ended upper
 * bound expressed either as 0 or UINT64_MAX (ZDB_TS_WINDOW_ALL).
 */
/* Only the aggregate scan uses this, and FCB has no aggregate scan. */
__maybe_unused static bool zdb_ts_window_is_unbounded(zdb_ts_window_t window)
{
	return (window.from_ts_ms == 0U) &&
	       ((window.to_ts_ms == 0U) || (window.to_ts_ms == UINT64_MAX));
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
	int rc = 0;
	size_t flushed_bytes = 0U;
	const char *flushed_stream = NULL;
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
	/* One work item drains every stream; a failing stream keeps its data. */
	rc = zdb_ts_flush_all_locked(ctx, &flushed_bytes, &flushed_stream);
	if (rc < 0) {
		status = zdb_status_from_errno(rc);
	}
#else
	/*
	 * Flushing is a no-op on a write-through backend, so nothing below
	 * reads these and there is no flush event to emit.
	 */
	ARG_UNUSED(rc);
	ARG_UNUSED(flushed_bytes);
	ARG_UNUSED(flushed_stream);
	ARG_UNUSED(status);
#endif
	ctx->flush_pending = false;
	k_sem_give(&ctx->flush_done);
	zdb_unlock_write(ctx->db);

#if defined(CONFIG_ZDB_EVENTING) && (CONFIG_ZDB_EVENTING)
	if ((flushed_bytes > 0U) || (status != ZDB_OK)) {
		/*
		 * The flush covers every stream with pending samples, so it
		 * reports the total; it names the stream only when there was
		 * exactly one, which is the usual case.
		 */
		zdb_emit_ts_event(ctx->db, ZDB_TS_EVENT_FLUSH, flushed_stream,
				  0U, 0, flushed_bytes, 0U, status);
	}
#endif
}

/*
 * Find the slot holding an open stream, or NULL.
 */
static struct zdb_ts_stream_ctx *zdb_ts_slot_find(struct zdb_ts_core_ctx *ctx,
						  const char *stream_name)
{
	size_t i;

	if ((ctx == NULL) || (stream_name == NULL)) {
		return NULL;
	}

	for (i = 0U; i < ARRAY_SIZE(ctx->streams); i++) {
		if (ctx->streams[i].in_use && (strcmp(ctx->streams[i].name, stream_name) == 0)) {
			return &ctx->streams[i];
		}
	}

	return NULL;
}

/*
 * Claim a slot for a stream, giving it an ingest buffer.
 *
 * Re-opening an already-open stream returns its existing slot, so appends and
 * cursors keep seeing the same buffered samples.
 */
static struct zdb_ts_stream_ctx *zdb_ts_slot_claim(struct zdb_ts_core_ctx *ctx,
						   const char *stream_name)
{
	struct zdb_ts_stream_ctx *slot;
	size_t i;

	slot = zdb_ts_slot_find(ctx, stream_name);
	if (slot != NULL) {
		if (slot->open_count < UINT8_MAX) {
			slot->open_count++;
		}
		return slot;
	}

	for (i = 0U; i < ARRAY_SIZE(ctx->streams); i++) {
		if (ctx->streams[i].in_use) {
			continue;
		}

		slot = &ctx->streams[i];
		(void)memset(slot, 0, sizeof(*slot));
		(void)strncpy(slot->name, stream_name, sizeof(slot->name) - 1U);
		slot->name[sizeof(slot->name) - 1U] = '\0';

#if ZDB_TS_USE_LITTLEFS
		slot->ingest_capacity = MIN((size_t)CONFIG_ZDB_TS_INGEST_BUFFER_BYTES,
					    (size_t)CONFIG_ZDB_TS_INGEST_SLAB_BLOCK_SIZE);
		if ((ctx->db->ts_ingest_slab == NULL) ||
		    (k_mem_slab_alloc(ctx->db->ts_ingest_slab, (void **)&slot->ingest_buf,
				      K_NO_WAIT) != 0)) {
			return NULL;
		}
#else
		slot->ingest_capacity = 0U;
		slot->ingest_buf = NULL;
#endif
		slot->ingest_used = 0U;
		slot->open_count = 1U;
		slot->in_use = true;
		return slot;
	}

	return NULL;
}

/*
 * Drop one handle's claim on a slot, freeing it once the last one goes.
 * Returns true when the slot was actually released.
 */
static bool zdb_ts_slot_release(struct zdb_ts_core_ctx *ctx, struct zdb_ts_stream_ctx *slot)
{
	if ((ctx == NULL) || (slot == NULL) || !slot->in_use) {
		return false;
	}

	if (slot->open_count > 1U) {
		slot->open_count--;
		return false;
	}

#if ZDB_TS_USE_LITTLEFS
	if ((slot->ingest_buf != NULL) && (ctx->db->ts_ingest_slab != NULL)) {
		k_mem_slab_free(ctx->db->ts_ingest_slab, slot->ingest_buf);
	}
#endif
	(void)memset(slot, 0, sizeof(*slot));
	return true;
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
	/* A NULL cfg.work_q means "use the system work queue", as documented. */
	ctx->work_q = (db->cfg->work_q != NULL) ? db->cfg->work_q : &k_sys_work_q;
	ctx->db = db;

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

#if !ZDB_TS_SEGMENTED
	/* Segmented streams create a header per segment as segments appear. */
	rc = zdb_ts_ensure_stream_header(db, stream_name);
	if (rc != ZDB_OK) {
		return rc;
	}
#endif
#endif

	/*
	 * Claim a slot. Re-opening a stream that is already open returns the
	 * same slot, so buffered samples stay visible.
	 */
	if (zdb_ts_slot_claim(ctx, stream_name) == NULL) {
		return ZDB_ERR_BUSY;
	}

	ts->db = db;
	ts->stream_name = stream_name;

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
	struct zdb_ts_core_ctx *ctx;
	struct zdb_ts_stream_ctx *slot;
	zdb_status_t status = ZDB_OK;

	if (ts == NULL) {
		return ZDB_ERR_INVAL;
	}

	if ((ts->db != NULL) && (ts->stream_name != NULL)) {
		ctx = (struct zdb_ts_core_ctx *)ts->db->ts_ctx;
		if (ctx != NULL) {
			if (zdb_lock_write(ts->db) == ZDB_OK) {
				slot = zdb_ts_slot_find(ctx, ts->stream_name);
				if ((slot != NULL) && (slot->open_count <= 1U)) {
#if ZDB_TS_USE_LITTLEFS
					/*
					 * Persist what is buffered before the
					 * slot goes away, so closing a stream
					 * never silently drops samples.
					 */
					int rc = zdb_ts_flush_buffer_locked(ctx, slot);

					if (rc < 0) {
						status = zdb_status_from_errno(rc);
					}
#endif
				}
				if (slot != NULL) {
					(void)zdb_ts_slot_release(ctx, slot);
				}
				zdb_unlock_write(ts->db);
			}
		}
	}

	ts->db = NULL;
	ts->stream_name = NULL;
	return status;
}

zdb_status_t zdb_ts_append_i64(zdb_ts_t *ts, const zdb_ts_sample_i64_t *sample)
{
	struct zdb_ts_core_ctx *ctx;
	struct zdb_ts_stream_ctx *slot;
	zdb_status_t lock_rc;
	zdb_status_t status = ZDB_OK;
	int rc;
#if ZDB_TS_USE_FCB
	struct zdb_ts_record_i64 rec;
#else
	bool need_async_flush = false;
#endif

	if ((ts == NULL) || (ts->db == NULL) || (sample == NULL)) {
		return ZDB_ERR_INVAL;
	}

	ctx = zdb_ts_ctx_get_or_alloc(ts->db);
	if (ctx == NULL) {
		return ZDB_ERR_INVAL;
	}

	slot = zdb_ts_slot_find(ctx, ts->stream_name);
	if (slot == NULL) {
		return ZDB_ERR_INVAL;
	}

	lock_rc = zdb_lock_write(ts->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

#if ZDB_TS_USE_FCB
	ARG_UNUSED(slot);
	zdb_ts_record_encode(sample, &rec);
	rc = zdb_ts_fcb_append_record(ctx, &rec);
	zdb_unlock_write(ts->db);

	status = (rc < 0) ? zdb_status_from_errno(rc) : ZDB_OK;
#else
	{
		size_t rec_size = zdb_ts_slot_rec_size(slot);

		if ((slot->ingest_capacity < rec_size) || (slot->ingest_buf == NULL)) {
			zdb_unlock_write(ts->db);
			status = ZDB_ERR_NOMEM;
			goto out;
		}

		if ((slot->ingest_used + rec_size) > slot->ingest_capacity) {
			rc = zdb_ts_flush_buffer_locked(ctx, slot);
			if (rc < 0) {
				zdb_unlock_write(ts->db);
				status = zdb_status_from_errno(rc);
				goto out;
			}
		}

		if (!zdb_ts_record_encode_as(zdb_ts_slot_version(slot),
					     zdb_ts_slot_base_ts(slot), sample,
					     &slot->ingest_buf[slot->ingest_used])) {
#if ZDB_TS_SEGMENTED
			/*
			 * Out of range of this segment's timestamp base. Persist
			 * what is buffered, then start a segment based on this
			 * sample so it and its successors fit.
			 */
			rc = zdb_ts_flush_buffer_locked(ctx, slot);
			if (rc == 0) {
				rc = zdb_ts_start_segment(ctx->db, slot, sample->ts_ms);
			}
			if ((rc < 0) ||
			    !zdb_ts_record_encode_as(zdb_ts_slot_version(slot),
						     zdb_ts_slot_base_ts(slot), sample,
						     &slot->ingest_buf[slot->ingest_used])) {
				zdb_unlock_write(ts->db);
				status = (rc < 0) ? zdb_status_from_errno(rc) : ZDB_ERR_INVAL;
				goto out;
			}
			rec_size = zdb_ts_slot_rec_size(slot);
#else
			zdb_unlock_write(ts->db);
			status = ZDB_ERR_INVAL;
			goto out;
#endif
		}

		slot->ingest_used += rec_size;
		need_async_flush = (slot->ingest_used + rec_size) > slot->ingest_capacity;
	}
	zdb_unlock_write(ts->db);

	if (need_async_flush) {
		/*
		 * The sample is already buffered; this flush only starts
		 * draining early. ZDB_ERR_BUSY means a flush is in flight
		 * already, which is not an append failure.
		 */
		zdb_status_t flush_rc = zdb_ts_flush_async(ts);

		if (flush_rc != ZDB_ERR_BUSY) {
			status = flush_rc;
		}
	}
#endif

/* Every "goto out" above belongs to the buffered path, which FCB does not use. */
#if !ZDB_TS_USE_FCB
out:
#endif
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
	struct zdb_ts_stream_ctx *slot;
#if ZDB_TS_USE_FCB
	struct zdb_ts_record_i64 rec;
#endif
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

	slot = zdb_ts_slot_find(ctx, ts->stream_name);
	if (slot == NULL) {
		zdb_unlock_write(ts->db);
		return ZDB_ERR_INVAL;
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
		size_t rec_size = zdb_ts_slot_rec_size(slot);

		if ((slot->ingest_capacity < rec_size) || (slot->ingest_buf == NULL)) {
			status = ZDB_ERR_NOMEM;
			break;
		}

		if ((slot->ingest_used + rec_size) > slot->ingest_capacity) {
			rc = zdb_ts_flush_buffer_locked(ctx, slot);
			if (rc < 0) {
				status = zdb_status_from_errno(rc);
				break;
			}
		}

		if (!zdb_ts_record_encode_as(zdb_ts_slot_version(slot),
					     zdb_ts_slot_base_ts(slot), &samples[i],
					     &slot->ingest_buf[slot->ingest_used])) {
#if ZDB_TS_SEGMENTED
			rc = zdb_ts_flush_buffer_locked(ctx, slot);
			if (rc == 0) {
				rc = zdb_ts_start_segment(ctx->db, slot, samples[i].ts_ms);
			}
			if ((rc < 0) ||
			    !zdb_ts_record_encode_as(zdb_ts_slot_version(slot),
						     zdb_ts_slot_base_ts(slot), &samples[i],
						     &slot->ingest_buf[slot->ingest_used])) {
				status = (rc < 0) ? zdb_status_from_errno(rc) : ZDB_ERR_INVAL;
				break;
			}
			rec_size = zdb_ts_slot_rec_size(slot);
#else
			status = ZDB_ERR_INVAL;
			break;
#endif
		}

		slot->ingest_used += rec_size;
#endif
	}

	zdb_unlock_write(ts->db);

#if !ZDB_TS_USE_FCB
	if ((status == ZDB_OK) &&
	    ((slot->ingest_used + zdb_ts_slot_rec_size(slot)) > slot->ingest_capacity)) {
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
	if ((ts == NULL) || (ts->db == NULL) || (ts->db->cfg == NULL)) {
		return ZDB_ERR_INVAL;
	}

#if ZDB_TS_USE_FCB
	return ZDB_OK;
#else
	struct zdb_ts_core_ctx *ctx;
	zdb_status_t lock_rc;
	int rc;

	ctx = zdb_ts_ctx_get_or_alloc(ts->db);
	if (ctx == NULL) {
		return ZDB_ERR_NOMEM;
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
#endif
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

#if !ZDB_TS_USE_FCB
/*
 * Count every record in a stream without reading payloads.
 *
 * Records are fixed size and the file starts with a stream header, so the
 * flushed count is pure arithmetic on the file size; unflushed samples are
 * counted from the ingest buffer. Returns ZDB_ERR_UNSUPPORTED when the window
 * is not "everything", leaving the caller to scan.
 *
 * This counts frames rather than decoding them, so a record that would fail
 * its CRC still counts. That matches what a scan would report for a stream
 * whose corrupt tail was already truncated by recovery, and the alternative
 * (reading every payload) is exactly the cost this path exists to avoid.
 */
static zdb_status_t zdb_ts_count_all(zdb_ts_t *ts, uint32_t *out_count)
{
	char path[ZDB_TS_PATH_MAX];
	struct fs_dirent entry;
	struct zdb_ts_core_ctx *ctx;
	zdb_status_t lock_rc;
	size_t flushed = 0U;
	size_t buffered = 0U;
	int rc;

	lock_rc = zdb_lock_read(ts->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

#if ZDB_TS_SEGMENTED
	{
		struct zdb_ts_core_ctx *tctx = (struct zdb_ts_core_ctx *)ts->db->ts_ctx;
		struct zdb_ts_stream_ctx *seg_slot =
			(tctx != NULL) ? zdb_ts_slot_find(tctx, ts->stream_name) : NULL;
		uint32_t seq;

		if (seg_slot == NULL) {
			zdb_unlock_read(ts->db);
			return ZDB_ERR_INVAL;
		}

		/*
		 * Every retained segment contributes its whole-record count.
		 * Segments may differ in layout, so each one's own header says
		 * how to divide its size — still no payload reads.
		 */
		for (seq = seg_slot->oldest_seg; seq <= seg_slot->cur_seg; seq++) {
			struct zdb_ts_cursor_ctx probe;

			rc = zdb_ts_build_segment_path(ts->db->cfg, ts->stream_name, seq, path,
						       sizeof(path));
			if (rc < 0) {
				zdb_unlock_read(ts->db);
				return zdb_status_from_errno(rc);
			}
			if (fs_stat(path, &entry) != 0) {
				continue;
			}
			if (zdb_ts_cursor_learn_layout(&probe, path) != ZDB_OK) {
				continue;
			}
			if ((size_t)entry.size > probe.hdr_size) {
				flushed += ((size_t)entry.size - probe.hdr_size) /
					   probe.rec_size;
			}
		}
	}
#else
	rc = zdb_ts_build_path(ts->db->cfg, ts->stream_name, path, sizeof(path));
	if (rc < 0) {
		zdb_unlock_read(ts->db);
		return zdb_status_from_errno(rc);
	}

	rc = fs_stat(path, &entry);
	if (rc == 0) {
		struct zdb_ts_cursor_ctx probe;

		if (zdb_ts_cursor_learn_layout(&probe, path) == ZDB_OK) {
			if ((size_t)entry.size > probe.hdr_size) {
				flushed = ((size_t)entry.size - probe.hdr_size) /
					  probe.rec_size;
			}
		}
	} else if (rc != -ENOENT) {
		zdb_unlock_read(ts->db);
		return zdb_status_from_errno(rc);
	}
#endif

	ctx = (struct zdb_ts_core_ctx *)ts->db->ts_ctx;
	if (ctx != NULL) {
		struct zdb_ts_stream_ctx *slot = zdb_ts_slot_find(ctx, ts->stream_name);

		if (slot != NULL) {
			buffered = slot->ingest_used / zdb_ts_slot_rec_size(slot);
		}
	}

	zdb_unlock_read(ts->db);

	*out_count = (uint32_t)(flushed + buffered);
	return ZDB_OK;
}
#endif /* !ZDB_TS_USE_FCB */

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
	uint32_t points = 0U;
	double acc = 0.0;
	bool truncated = false;
	zdb_status_t rc;

	if ((ts == NULL) || (ts->db == NULL) || (out_result == NULL)) {
		return ZDB_ERR_INVAL;
	}

	if (agg > ZDB_TS_AGG_COUNT) {
		return ZDB_ERR_INVAL;
	}

	/*
	 * COUNT over the whole stream needs no payloads: every record is the
	 * same fixed size, so the flushed count follows from the file size and
	 * the rest is whatever is still buffered in RAM.
	 */
	if ((agg == ZDB_TS_AGG_COUNT) && zdb_ts_window_is_unbounded(window)) {
		uint32_t fast_count = 0U;

		rc = zdb_ts_count_all(ts, &fast_count);
		if (rc != ZDB_OK) {
			return rc;
		}

		out_result->agg = agg;
		out_result->points = fast_count;
		out_result->value = (double)fast_count;
		out_result->truncated = false;
		return ZDB_OK;
	}

	rc = zdb_ts_cursor_open(ts, window, NULL, NULL, &cursor);
	if (rc != ZDB_OK) {
		return rc;
	}

	/*
	 * COUNT is not capped: reporting a low number as if it were complete
	 * would be worse than the scan cost. The other aggregates keep the cap
	 * and now report that they hit it.
	 */
	while ((agg == ZDB_TS_AGG_COUNT) || (points < CONFIG_ZDB_TS_MAX_AGG_POINTS)) {
		zdb_ts_sample_i64_t sample;

		rc = zdb_ts_cursor_next_sample(&cursor, &sample);
		if (rc == ZDB_ERR_NOT_FOUND) {
			break;
		}
		if (rc != ZDB_OK) {
			(void)zdb_cursor_close(&cursor);
			return rc;
		}

		if (!zdb_ts_agg_update(agg, (double)sample.value, &points, &acc)) {
			(void)zdb_cursor_close(&cursor);
			return ZDB_ERR_INVAL;
		}
	}

	/*
	 * Distinguish "stopped at the cap" from "consumed the stream": peek one
	 * more matching record before declaring the result truncated.
	 */
	if ((agg != ZDB_TS_AGG_COUNT) && (points == CONFIG_ZDB_TS_MAX_AGG_POINTS)) {
		zdb_ts_sample_i64_t peek;

		if (zdb_ts_cursor_next_sample(&cursor, &peek) == ZDB_OK) {
			truncated = true;
		}
	}

	(void)zdb_cursor_close(&cursor);

	/*
	 * An empty window is a real answer for COUNT; the value-bearing
	 * aggregates have nothing to report and keep returning NOT_FOUND.
	 */
	if ((points == 0U) && (agg != ZDB_TS_AGG_COUNT)) {
		return ZDB_ERR_NOT_FOUND;
	}

	out_result->agg = agg;
	out_result->points = points;
	out_result->truncated = truncated;
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

static zdb_status_t zdb_ts_cursor_open_dir(zdb_ts_t *ts, zdb_ts_window_t window,
			zdb_predicate_fn predicate, void *predicate_ctx,
			bool descending, zdb_cursor_t *out_cursor)
{
	struct zdb_ts_cursor_ctx *ctx = NULL;
	zdb_status_t rc;

	if ((ts == NULL) || (ts->db == NULL) || (out_cursor == NULL)) {
		return ZDB_ERR_INVAL;
	}

#if ZDB_TS_USE_FCB
	if (descending) {
		/* fcb_getnext() only walks forward. */
		return ZDB_ERR_UNSUPPORTED;
	}
#endif

#if ZDB_TS_USE_LITTLEFS && !ZDB_TS_SEGMENTED
	rc = zdb_ts_ensure_stream_header(ts->db, ts->stream_name);
	if (rc != ZDB_OK) {
		return rc;
	}
#else
	/* Segment headers are written as segments are created. */
	rc = ZDB_OK;
	ARG_UNUSED(rc);
#endif

	if (k_mem_slab_alloc(ts->db->cursor_slab, (void **)&ctx, K_NO_WAIT) != 0) {
		return ZDB_ERR_NOMEM;
	}

	ctx->db = ts->db;
	ctx->stream_name = ts->stream_name;
	ctx->window = window;
	ctx->descending = descending;
	ctx->file_offset = sizeof(struct zdb_ts_stream_header);
	ctx->ram_offset = 0U;
	ctx->file_done = false;
#if ZDB_TS_USE_LITTLEFS
#if ZDB_TS_SEGMENTED
	{
		struct zdb_ts_core_ctx *tctx = zdb_ts_ctx_get_or_alloc(ts->db);
		struct zdb_ts_stream_ctx *slot =
			(tctx != NULL) ? zdb_ts_slot_find(tctx, ts->stream_name) : NULL;
		zdb_status_t seg_rc;

		if ((slot == NULL) || (zdb_ts_scan_segments(ts->db, slot) != ZDB_OK)) {
			k_mem_slab_free(ts->db->cursor_slab, ctx);
			return ZDB_ERR_INVAL;
		}

		fs_file_t_init(&ctx->file);
		ctx->file_open = false;
		ctx->seg_lo = slot->oldest_seg;
		ctx->seg_hi = slot->cur_seg;
		ctx->cur_seg = descending ? ctx->seg_hi : ctx->seg_lo;

		seg_rc = zdb_ts_cursor_open_segment(ctx, ctx->cur_seg);
		if (seg_rc != ZDB_OK) {
			if (ctx->file_open) {
				(void)fs_close(&ctx->file);
			}
			k_mem_slab_free(ts->db->cursor_slab, ctx);
			return seg_rc;
		}
	}
#else
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

		{
			zdb_status_t layout_rc = zdb_ts_cursor_learn_layout(ctx, cursor_path);

			if (layout_rc != ZDB_OK) {
				(void)fs_close(&ctx->file);
				k_mem_slab_free(ts->db->cursor_slab, ctx);
				return layout_rc;
			}
		}

		/*
		 * Record the size once: a descending walk starts at the end and
		 * steps back a record at a time, and reset needs the same
		 * anchor. Records appended after the cursor opened are not
		 * visible to it either way.
		 */
		{
			off_t end = fs_seek(&ctx->file, 0, FS_SEEK_END);

			if (end < 0) {
				(void)fs_close(&ctx->file);
				k_mem_slab_free(ts->db->cursor_slab, ctx);
				return zdb_status_from_errno((int)end);
			}
			ctx->file_size = (size_t)fs_tell(&ctx->file);
		}

		ctx->file_offset = ctx->hdr_size;
		if (descending) {
			ctx->file_offset = ctx->hdr_size +
					   (((ctx->file_size - ctx->hdr_size) / ctx->rec_size) *
					    ctx->rec_size);
		}

		open_rc = fs_seek(&ctx->file,
				  (off_t)ctx->file_offset,
				  FS_SEEK_SET);
		if (open_rc != 0) {
			(void)fs_close(&ctx->file);
			k_mem_slab_free(ts->db->cursor_slab, ctx);
			return zdb_status_from_errno(open_rc);
		}
	}
#endif /* ZDB_TS_SEGMENTED */
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

zdb_status_t zdb_ts_cursor_open(zdb_ts_t *ts, zdb_ts_window_t window,
			zdb_predicate_fn predicate, void *predicate_ctx,
			zdb_cursor_t *out_cursor)
{
	return zdb_ts_cursor_open_dir(ts, window, predicate, predicate_ctx, false, out_cursor);
}

zdb_status_t zdb_ts_cursor_open_desc(zdb_ts_t *ts, zdb_ts_window_t window,
			zdb_predicate_fn predicate, void *predicate_ctx,
			zdb_cursor_t *out_cursor)
{
	return zdb_ts_cursor_open_dir(ts, window, predicate, predicate_ctx, true, out_cursor);
}

#if ZDB_TS_USE_LITTLEFS
/*
 * Walk the unflushed ingest buffer.
 *
 * Ascending order visits it after the file (it holds the newest samples);
 * descending visits it first and steps backwards through it. Returns
 * ZDB_ERR_NOT_FOUND once the buffer is exhausted in the current direction.
 */
static zdb_status_t zdb_ts_cursor_next_ram(zdb_cursor_t *cursor,
					   struct zdb_ts_cursor_ctx *cctx,
					   zdb_bytes_t *out_record)
{
	struct zdb_ts_core_ctx *tctx;
	struct zdb_ts_stream_ctx *slot;
	zdb_bytes_t candidate;
	zdb_status_t lock_rc;

	tctx = zdb_ts_ctx_get_or_alloc(cctx->db);
	if (tctx == NULL) {
		return ZDB_ERR_INVAL;
	}

	/*
	 * A cursor over a stream that has since been closed simply sees no
	 * buffered samples; its stored records are still served from the file.
	 */
	slot = zdb_ts_slot_find(tctx, cctx->stream_name);
	if ((slot == NULL) || (slot->ingest_buf == NULL)) {
		out_record->data = NULL;
		out_record->len = 0U;
		return ZDB_ERR_NOT_FOUND;
	}

	lock_rc = zdb_lock_read(cctx->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	while ((cctx->ram_offset + zdb_ts_slot_rec_size(slot)) <= slot->ingest_used) {
		uint64_t ts_ms;
		int64_t val;
		zdb_status_t dec_rc;
		size_t buf_off;
		size_t rec_size = zdb_ts_slot_rec_size(slot);

		/*
		 * ram_offset counts bytes already consumed in the walk's own
		 * direction, so a descending walk maps it to an offset measured
		 * back from the end of the buffer.
		 */
		buf_off = cctx->descending
				  ? (slot->ingest_used - cctx->ram_offset - rec_size)
				  : cctx->ram_offset;

		candidate.data = &slot->ingest_buf[buf_off];
		candidate.len = rec_size;
		dec_rc = zdb_ts_record_decode_as(cctx->db, zdb_ts_slot_version(slot),
						 zdb_ts_slot_base_ts(slot), candidate.data,
						 &ts_ms, &val);
		if (dec_rc == ZDB_ERR_UNSUPPORTED) {
			cctx->ram_offset += rec_size;
			continue;
		}
		if (dec_rc != ZDB_OK) {
			zdb_unlock_read(cctx->db);
			return dec_rc;
		}
		ARG_UNUSED(val);
		cctx->ram_offset += rec_size;
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

		cctx->last_ts_ms = ts_ms;
		cctx->last_value = val;
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
#endif /* ZDB_TS_USE_LITTLEFS */

zdb_status_t zdb_cursor_next(zdb_cursor_t *cursor, zdb_bytes_t *out_record)
{
	struct zdb_ts_cursor_ctx *cctx;
	zdb_bytes_t candidate;
#if ZDB_TS_USE_FCB
	struct zdb_ts_core_ctx *tctx;
	struct zdb_ts_record_i64 rec;
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

			cctx->last_ts_ms = ts_ms;
			cctx->last_value = val;
			*out_record = candidate;
			cursor->current = candidate;
			return ZDB_OK;
		}
	}
#endif

#if ZDB_TS_USE_LITTLEFS
	if (cctx->descending) {
		/* The buffer holds the newest samples, so it leads a reverse walk. */
		zdb_status_t ram_rc = zdb_ts_cursor_next_ram(cursor, cctx, out_record);

		if (ram_rc != ZDB_ERR_NOT_FOUND) {
			return ram_rc;
		}
	}

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

		dec_rc = zdb_ts_record_decode_as(cctx->db, (cctx->rec_size ==
							    sizeof(struct zdb_ts_record_v2))
							   ? (uint16_t)ZDB_TS_STREAM_VERSION_V2
							   : (uint16_t)ZDB_TS_STREAM_VERSION,
						 cctx->base_ts_ms, candidate.data, &ts_ms,
						 &val);
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

		cctx->last_ts_ms = ts_ms;
		cctx->last_value = val;
		*out_record = candidate;
		cursor->current = candidate;
		return ZDB_OK;
	}

	if (!cctx->descending) {
		return zdb_ts_cursor_next_ram(cursor, cctx, out_record);
	}

	out_record->data = NULL;
	out_record->len = 0U;
	return ZDB_ERR_NOT_FOUND;
#else
	out_record->data = NULL;
	out_record->len = 0U;
	return ZDB_ERR_UNSUPPORTED;
#endif
}

zdb_status_t zdb_ts_cursor_next_sample(zdb_cursor_t *cursor, zdb_ts_sample_i64_t *out_sample)
{
	const struct zdb_ts_cursor_ctx *cctx;
	zdb_bytes_t record;
	zdb_status_t rc;

	if ((cursor == NULL) || (out_sample == NULL)) {
		return ZDB_ERR_INVAL;
	}

	rc = zdb_cursor_next(cursor, &record);
	if (rc != ZDB_OK) {
		return rc;
	}

	/*
	 * zdb_cursor_next() had to decode the record to apply the window and
	 * predicate; take its result rather than decoding the bytes again, which
	 * is also what keeps callers from needing to know the record format.
	 */
	cctx = (const struct zdb_ts_cursor_ctx *)cursor->impl;
	out_sample->ts_ms = cctx->last_ts_ms;
	out_sample->value = cctx->last_value;

	return ZDB_OK;
}

/*
 * Consumed watermark.
 *
 * A consumer that forwards samples somewhere else needs to remember how far it
 * got, so a restart resumes instead of replaying. The mark is a timestamp
 * rather than a record position, which stays meaningful even as older records
 * age out of the stream.
 *
 * The sidecar is rewritten whole on each update; littlefs commits a file
 * update atomically, and a torn or corrupt file fails its CRC and reads as
 * unset. That is the safe direction to fail: the consumer re-processes from
 * the last known-good mark rather than skipping samples it never handled.
 */
zdb_status_t zdb_ts_watermark_set(zdb_ts_t *ts, uint64_t consumed_ts_ms)
{
#if ZDB_TS_USE_FCB
	ARG_UNUSED(consumed_ts_ms);
	if ((ts == NULL) || (ts->db == NULL)) {
		return ZDB_ERR_INVAL;
	}
	return ZDB_ERR_UNSUPPORTED;
#else
	char path[ZDB_TS_PATH_MAX];
	struct zdb_ts_watermark_rec rec;
	struct fs_file_t file;
	struct zdb_ts_core_ctx *ctx;
	zdb_status_t lock_rc;
	ssize_t wr;
	int rc;

	if ((ts == NULL) || (ts->db == NULL) || (ts->db->cfg == NULL)) {
		return ZDB_ERR_INVAL;
	}

	ctx = zdb_ts_ctx_get_or_alloc(ts->db);
	if (ctx == NULL) {
		return ZDB_ERR_NOMEM;
	}

	rc = zdb_ts_ensure_stream_dir(ts->db->cfg, ctx);
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	rc = zdb_ts_build_watermark_path(ts->db->cfg, ts->stream_name, path, sizeof(path));
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	rec.magic_le = sys_cpu_to_le32(ZDB_TS_WMK_MAGIC);
	rec.version_le = sys_cpu_to_le16(ZDB_TS_WMK_VERSION);
	rec.reserved_le = 0U;
	rec.ts_ms_le = sys_cpu_to_le64(consumed_ts_ms);
	rec.crc_le = sys_cpu_to_le32(
		crc32_ieee((const uint8_t *)&rec, offsetof(struct zdb_ts_watermark_rec, crc_le)));

	lock_rc = zdb_lock_write(ts->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (rc < 0) {
		zdb_unlock_write(ts->db);
		return zdb_status_from_errno(rc);
	}

	wr = fs_write(&file, &rec, sizeof(rec));
	(void)fs_close(&file);
	zdb_unlock_write(ts->db);

	if (wr < 0) {
		return zdb_status_from_errno((int)wr);
	}

	return (wr == (ssize_t)sizeof(rec)) ? ZDB_OK : ZDB_ERR_IO;
#endif
}

zdb_status_t zdb_ts_watermark_get(zdb_ts_t *ts, uint64_t *out_consumed_ts_ms)
{
#if ZDB_TS_USE_FCB
	ARG_UNUSED(out_consumed_ts_ms);
	if ((ts == NULL) || (ts->db == NULL)) {
		return ZDB_ERR_INVAL;
	}
	return ZDB_ERR_UNSUPPORTED;
#else
	char path[ZDB_TS_PATH_MAX];
	struct zdb_ts_watermark_rec rec;
	struct fs_file_t file;
	zdb_status_t lock_rc;
	uint32_t expect_crc;
	ssize_t rd;
	int rc;

	if ((ts == NULL) || (ts->db == NULL) || (ts->db->cfg == NULL) ||
	    (out_consumed_ts_ms == NULL)) {
		return ZDB_ERR_INVAL;
	}

	rc = zdb_ts_build_watermark_path(ts->db->cfg, ts->stream_name, path, sizeof(path));
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	lock_rc = zdb_lock_read(ts->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	fs_file_t_init(&file);
	rc = fs_open(&file, path, FS_O_READ);
	if (rc < 0) {
		zdb_unlock_read(ts->db);
		return (rc == -ENOENT) ? ZDB_ERR_NOT_FOUND : zdb_status_from_errno(rc);
	}

	rd = fs_read(&file, &rec, sizeof(rec));
	(void)fs_close(&file);
	zdb_unlock_read(ts->db);

	if (rd < 0) {
		return zdb_status_from_errno((int)rd);
	}

	/* A short, stale, or corrupt mark reads as unset. */
	if (((size_t)rd != sizeof(rec)) ||
	    (sys_le32_to_cpu(rec.magic_le) != ZDB_TS_WMK_MAGIC) ||
	    (sys_le16_to_cpu(rec.version_le) != ZDB_TS_WMK_VERSION)) {
		return ZDB_ERR_NOT_FOUND;
	}

	expect_crc = crc32_ieee((const uint8_t *)&rec,
				offsetof(struct zdb_ts_watermark_rec, crc_le));
	if (sys_le32_to_cpu(rec.crc_le) != expect_crc) {
		return ZDB_ERR_NOT_FOUND;
	}

	*out_consumed_ts_ms = sys_le64_to_cpu(rec.ts_ms_le);
	return ZDB_OK;
#endif
}

zdb_status_t zdb_ts_watermark_clear(zdb_ts_t *ts)
{
#if ZDB_TS_USE_FCB
	if ((ts == NULL) || (ts->db == NULL)) {
		return ZDB_ERR_INVAL;
	}
	return ZDB_ERR_UNSUPPORTED;
#else
	char path[ZDB_TS_PATH_MAX];
	zdb_status_t lock_rc;
	int rc;

	if ((ts == NULL) || (ts->db == NULL) || (ts->db->cfg == NULL)) {
		return ZDB_ERR_INVAL;
	}

	rc = zdb_ts_build_watermark_path(ts->db->cfg, ts->stream_name, path, sizeof(path));
	if (rc < 0) {
		return zdb_status_from_errno(rc);
	}

	lock_rc = zdb_lock_write(ts->db);
	if (lock_rc != ZDB_OK) {
		return lock_rc;
	}

	rc = fs_unlink(path);
	zdb_unlock_write(ts->db);

	/* Clearing an unset watermark is the state the caller asked for. */
	if ((rc < 0) && (rc != -ENOENT)) {
		return zdb_status_from_errno(rc);
	}

	return ZDB_OK;
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
	uint8_t rec[ZDB_TS_HDR_MAX_SIZE];
	char path[ZDB_TS_PATH_MAX];
	size_t hdr_size;
	size_t rec_size;
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

#if ZDB_TS_SEGMENTED
	{
		/*
		 * Only the newest segment can hold a partial write; the rest
		 * were sealed when the stream rolled past them.
		 */
		struct zdb_ts_core_ctx *tctx = zdb_ts_ctx_get_or_alloc(ts->db);
		struct zdb_ts_stream_ctx *slot =
			(tctx != NULL) ? zdb_ts_slot_find(tctx, ts->stream_name) : NULL;

		if (slot == NULL) {
			return ZDB_ERR_INVAL;
		}
		rc = zdb_ts_active_path(ts->db, slot, path, sizeof(path));
	}
#else
	rc = zdb_ts_build_path(ts->db->cfg, ts->stream_name, path, sizeof(path));
#endif
	if (rc < 0) {
		ZDB_STAT_INC(ts->db, recover_failures);
		zdb_health_check(ts->db);
		return zdb_status_from_errno(rc);
	}

	/*
	 * A stream may hold either record layout, so learn this file's before
	 * walking it — the header size and record stride both depend on it.
	 */
	{
		struct zdb_ts_cursor_ctx probe;
		struct fs_dirent probe_info;

		hdr_size = sizeof(struct zdb_ts_stream_header);
		rec_size = sizeof(struct zdb_ts_record_i64);
		if ((fs_stat(path, &probe_info) == 0) &&
		    (zdb_ts_cursor_learn_layout(&probe, path) == ZDB_OK)) {
			hdr_size = probe.hdr_size;
			rec_size = probe.rec_size;
		}
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

	if (hdr_size == sizeof(struct zdb_ts_stream_header)) {
		zdb_status_t dec = zdb_ts_stream_header_decode(ts->db, &hdr, ts->stream_name);
		if (dec != ZDB_OK) {
			(void)fs_close(&file);
			ZDB_STAT_INC(ts->db, recover_failures);
			zdb_health_check(ts->db);
			return dec;
		}
	}

	/* Walk from just past this file's header, one record stride at a time. */
	good_end = hdr_size;
	rc = fs_seek(&file, (off_t)hdr_size, FS_SEEK_SET);
	if (rc < 0) {
		(void)fs_close(&file);
		ZDB_STAT_INC(ts->db, recover_failures);
		zdb_health_check(ts->db);
		return zdb_status_from_errno(rc);
	}

	while (true) {
		uint64_t ts_ms;
		int64_t val;
		zdb_status_t dec_rc;

		rd = fs_read(&file, rec, rec_size);
		if (rd == 0) {
			break;
		}
		if (rd < 0) {
			(void)fs_close(&file);
			ZDB_STAT_INC(ts->db, recover_failures);
			zdb_health_check(ts->db);
			return zdb_status_from_errno((int)rd);
		}
		if ((size_t)rd != rec_size) {
			break;
		}

		dec_rc = zdb_ts_record_decode_as(ts->db,
						 (rec_size == sizeof(struct zdb_ts_record_v2))
							 ? (uint16_t)ZDB_TS_STREAM_VERSION_V2
							 : (uint16_t)ZDB_TS_STREAM_VERSION,
						 0U, rec, &ts_ms, &val);
		if (dec_rc != ZDB_OK) {
			break;
		}

		good_end += rec_size;
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
