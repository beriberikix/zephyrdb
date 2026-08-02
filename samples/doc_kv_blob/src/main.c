/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Storing a structured record as a single key-value blob.
 *
 * The document model needs a filesystem. On a board that only has NVS or ZMS,
 * a small fixed-shape record can live in KV instead: serialize it into one
 * value, and version the layout so a firmware update can still read what the
 * previous version wrote.
 *
 * This sample shows the whole pattern end to end:
 *
 *   1. A defaults table seeds the record on first boot, so application code
 *      never has to special-case "nothing stored yet".
 *   2. Reading accepts both the v1 and v2 layouts, upgrading v1 in RAM and
 *      writing it back, which is what a firmware update looks like in the
 *      field.
 *   3. zdb_kv_reset_namespace() implements factory reset: it clears the
 *      namespace and re-seeds it from the same defaults table.
 *
 * Choose this over the document model when there is no filesystem and the
 * record's shape is known at compile time. Prefer the document model when
 * fields are added ad hoc or need to be queried.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/zms.h>
#include <zephyr/storage/flash_map.h>
#include <string.h>

#include "zephyrdb.h"

#define SETTINGS_NS  "settings"
#define SETTINGS_KEY "device"

#define SETTINGS_V1 1U
#define SETTINGS_V2 2U

/*
 * v1 layout, kept only so the reader can still parse records written by an
 * older firmware. New writes always use v2.
 */
struct settings_v1 {
	uint16_t version;
	uint16_t sample_rate_hz;
	uint32_t baud;
} __packed;

/* v2 adds a field. Existing installs must not lose their v1 values. */
struct settings_v2 {
	uint16_t version;
	uint16_t sample_rate_hz;
	uint32_t baud;
	uint8_t log_level;
	uint8_t reserved[3];
} __packed;

#define DEFAULT_LOG_LEVEL 2U

static const struct settings_v2 g_factory_settings = {
	.version = SETTINGS_V2,
	.sample_rate_hz = 100U,
	.baud = 115200U,
	.log_level = DEFAULT_LOG_LEVEL,
	.reserved = {0U, 0U, 0U},
};

static const zdb_kv_default_t g_defaults[] = {
	{
		.namespace_name = SETTINGS_NS,
		.key = SETTINGS_KEY,
		.value = &g_factory_settings,
		.value_len = sizeof(g_factory_settings),
	},
};

static struct zms_fs g_zms;

static zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = NULL,
	.work_q = &k_sys_work_q,
	.kv_defaults = g_defaults,
	.kv_default_count = ARRAY_SIZE(g_defaults),
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static int mount_zms(struct zms_fs *fs)
{
	struct flash_pages_info info;
	int rc;

	fs->flash_device = PARTITION_DEVICE(storage_partition);
	if (!device_is_ready(fs->flash_device)) {
		printk("doc_kv_blob: storage device not ready\n");
		return -ENODEV;
	}

	fs->offset = PARTITION_OFFSET(storage_partition);
	rc = flash_get_page_info_by_offs(fs->flash_device, fs->offset, &info);
	if (rc != 0) {
		printk("doc_kv_blob: flash page info failed rc=%d\n", rc);
		return rc;
	}

	fs->sector_size = info.size;
	fs->sector_count = 3U;

	rc = zms_mount(fs);
	if (rc != 0) {
		printk("doc_kv_blob: zms mount failed rc=%d\n", rc);
	}

	return rc;
}

/*
 * Read the record, accepting either layout.
 *
 * The stored length is not trusted to identify the version: the leading
 * version field decides, and the length is checked against it. A record that
 * matches neither is reported rather than silently reinterpreted.
 */
static zdb_status_t settings_load(zdb_kv_t *kv, struct settings_v2 *out, bool *out_upgraded)
{
	uint8_t buf[sizeof(struct settings_v2)];
	size_t stored_len = 0U;
	uint16_t version;
	zdb_status_t rc;

	*out_upgraded = false;

	rc = zdb_kv_get(kv, SETTINGS_KEY, buf, sizeof(buf), &stored_len);
	if (rc != ZDB_OK) {
		return rc;
	}

	if (stored_len < sizeof(uint16_t)) {
		return ZDB_ERR_CORRUPT;
	}

	(void)memcpy(&version, buf, sizeof(version));

	switch (version) {
	case SETTINGS_V2:
		if (stored_len != sizeof(struct settings_v2)) {
			return ZDB_ERR_CORRUPT;
		}
		(void)memcpy(out, buf, sizeof(*out));
		return ZDB_OK;

	case SETTINGS_V1: {
		struct settings_v1 old;

		if (stored_len != sizeof(old)) {
			return ZDB_ERR_CORRUPT;
		}
		(void)memcpy(&old, buf, sizeof(old));

		/* Carry v1's values forward; fields it never had take defaults. */
		out->version = SETTINGS_V2;
		out->sample_rate_hz = old.sample_rate_hz;
		out->baud = old.baud;
		out->log_level = DEFAULT_LOG_LEVEL;
		(void)memset(out->reserved, 0, sizeof(out->reserved));
		*out_upgraded = true;
		return ZDB_OK;
	}

	default:
		return ZDB_ERR_UNSUPPORTED;
	}
}

static zdb_status_t settings_store(zdb_kv_t *kv, const struct settings_v2 *settings)
{
	return zdb_kv_set(kv, SETTINGS_KEY, settings, sizeof(*settings));
}

int main(void)
{
	zdb_kv_t kv;
	struct settings_v2 settings;
	bool upgraded = false;
	zdb_status_t rc;

	if (mount_zms(&g_zms) != 0) {
		return 1;
	}
	g_cfg.kv_backend_fs = &g_zms;

	/* The defaults table is applied here, so the record exists from now on. */
	rc = zdb_init(&g_db, &g_cfg);
	if (rc != ZDB_OK) {
		printk("doc_kv_blob: init failed rc=%d\n", (int)rc);
		return 1;
	}

	rc = zdb_kv_open(&g_db, SETTINGS_NS, &kv);
	if (rc != ZDB_OK) {
		printk("doc_kv_blob: open failed rc=%d\n", (int)rc);
		goto out;
	}

	rc = settings_load(&kv, &settings, &upgraded);
	if (rc != ZDB_OK) {
		printk("doc_kv_blob: load failed rc=%d\n", (int)rc);
		goto out_close;
	}
	printk("doc_kv_blob: seeded settings v%u (rate=%u baud=%u log=%u)\n",
	       (unsigned)settings.version, (unsigned)settings.sample_rate_hz,
	       (unsigned)settings.baud, (unsigned)settings.log_level);

	/* Simulate an install that still holds a v1 record. */
	{
		const struct settings_v1 legacy = {
			.version = SETTINGS_V1,
			.sample_rate_hz = 25U,
			.baud = 9600U,
		};

		rc = zdb_kv_set(&kv, SETTINGS_KEY, &legacy, sizeof(legacy));
		if (rc != ZDB_OK) {
			printk("doc_kv_blob: writing legacy record failed rc=%d\n", (int)rc);
			goto out_close;
		}
	}

	rc = settings_load(&kv, &settings, &upgraded);
	if ((rc != ZDB_OK) || !upgraded) {
		printk("doc_kv_blob: v1 upgrade failed rc=%d upgraded=%d\n", (int)rc,
		       (int)upgraded);
		rc = (rc == ZDB_OK) ? ZDB_ERR_CORRUPT : rc;
		goto out_close;
	}

	/* Persist the upgraded record so the next boot reads v2 directly. */
	rc = settings_store(&kv, &settings);
	if (rc != ZDB_OK) {
		printk("doc_kv_blob: writing upgraded record failed rc=%d\n", (int)rc);
		goto out_close;
	}
	printk("doc_kv_blob: upgraded v1 record to v2 (rate=%u baud=%u log=%u)\n",
	       (unsigned)settings.sample_rate_hz, (unsigned)settings.baud,
	       (unsigned)settings.log_level);

	/* Factory reset: clear the namespace and re-seed from the same table. */
	rc = zdb_kv_reset_namespace(&kv);
	if (rc != ZDB_OK) {
		printk("doc_kv_blob: reset failed rc=%d\n", (int)rc);
		goto out_close;
	}

	rc = settings_load(&kv, &settings, &upgraded);
	if ((rc != ZDB_OK) || (settings.baud != g_factory_settings.baud) ||
	    (settings.sample_rate_hz != g_factory_settings.sample_rate_hz)) {
		printk("doc_kv_blob: reset did not restore defaults rc=%d\n", (int)rc);
		rc = (rc == ZDB_OK) ? ZDB_ERR_CORRUPT : rc;
		goto out_close;
	}
	printk("doc_kv_blob: factory reset restored defaults (rate=%u baud=%u)\n",
	       (unsigned)settings.sample_rate_hz, (unsigned)settings.baud);

	printk("doc_kv_blob: PASS\n");

out_close:
	(void)zdb_kv_close(&kv);
out:
	(void)zdb_deinit(&g_db);
	return (rc == ZDB_OK) ? 0 : 1;
}
