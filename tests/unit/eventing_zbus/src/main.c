/*
 * The zbus adapter: enabling CONFIG_ZDB_EVENTING_ZBUS must put every event
 * ZephyrDB emits on its channel, with the payload intact.
 *
 * This is the first automated coverage of the adapter — it previously existed
 * only in a sample that Twister never built.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kvss/zms.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/zbus/zbus.h>
#include <string.h>

#include "zephyrdb.h"
#include "zephyrdb_eventing_zbus.h"

static struct zms_fs g_zms;
static bool g_zms_mounted;

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = &g_zms,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static void zms_backend_mount_once(void)
{
	struct flash_pages_info info;
	int rc;

	if (g_zms_mounted) {
		return;
	}

	g_zms.flash_device = PARTITION_DEVICE(storage_partition);
	zassert_true(device_is_ready(g_zms.flash_device), "storage device not ready");

	g_zms.offset = PARTITION_OFFSET(storage_partition);
	rc = flash_get_page_info_by_offs(g_zms.flash_device, g_zms.offset, &info);
	zassert_equal(rc, 0, "flash page info failed: %d", rc);

	g_zms.sector_size = info.size;
	g_zms.sector_count = 3U;

	rc = zms_mount(&g_zms);
	zassert_equal(rc, 0, "zms mount failed: %d", rc);
	g_zms_mounted = true;
}

static void setup(void *fixture)
{
	ARG_UNUSED(fixture);
	zms_backend_mount_once();
}

ZTEST_SUITE(zephyrdb_eventing_zbus, NULL, NULL, setup, NULL, NULL);

/*
 * Reading a channel returns its retained message, so after an operation the
 * channel holds that operation's event.
 */
ZTEST(zephyrdb_eventing_zbus, test_kv_events_reach_their_channel)
{
	zdb_kv_t kv;
	zdb_kv_event_t event;
	uint32_t value = 4242U;
	int rc;

	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "init failed");
	zassert_equal(zdb_kv_open(&g_db, "zb", &kv), ZDB_OK, "kv_open failed");

	zassert_equal(zdb_kv_set(&kv, "counter", &value, sizeof(value)), ZDB_OK, "set failed");

	rc = zbus_chan_read(&zdb_kv_event_chan, &event, K_MSEC(200));
	zassert_equal(rc, 0, "reading the KV channel failed: %d", rc);
	zassert_equal(event.type, ZDB_EVENT_KV_SET, "channel did not carry the set");
	zassert_equal(event.status, ZDB_OK, "wrong status on the channel");
	zassert_equal(event.value_len, sizeof(value), "wrong value length");
	zassert_equal(strcmp(event.namespace_name, "zb"), 0, "wrong namespace");
	zassert_equal(strcmp(event.key, "counter"), 0, "wrong key");

	zassert_equal(zdb_kv_delete(&kv, "counter"), ZDB_OK, "delete failed");

	rc = zbus_chan_read(&zdb_kv_event_chan, &event, K_MSEC(200));
	zassert_equal(rc, 0, "reading the KV channel failed: %d", rc);
	zassert_equal(event.type, ZDB_EVENT_KV_DELETE, "channel did not carry the delete");
	zassert_equal(event.value_len, 0U, "delete should report no value length");

	zdb_kv_close(&kv);
	zdb_deinit(&g_db);
}

ZTEST(zephyrdb_eventing_zbus, test_ts_events_reach_their_channel)
{
	zdb_ts_t ts;
	zdb_ts_event_t event;
	zdb_ts_sample_i64_t sample = { .ts_ms = 9000U, .value = 77 };
	int rc;

	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "init failed");
	zassert_equal(zdb_ts_open(&g_db, "zb_ts", &ts), ZDB_OK, "ts_open failed");

	zassert_equal(zdb_ts_append_i64(&ts, &sample), ZDB_OK, "append failed");

	rc = zbus_chan_read(&zdb_ts_event_chan, &event, K_MSEC(200));
	zassert_equal(rc, 0, "reading the TS channel failed: %d", rc);
	zassert_equal(event.type, ZDB_TS_EVENT_APPEND, "channel did not carry the append");
	zassert_equal(event.sample_ts_ms, sample.ts_ms, "wrong sample timestamp");
	zassert_equal(event.sample_value, sample.value, "wrong sample value");
	zassert_equal(strcmp(event.stream_name, "zb_ts"), 0, "wrong stream");

	zassert_equal(zdb_ts_watermark_set(&ts, 1234U), ZDB_OK, "watermark set failed");

	rc = zbus_chan_read(&zdb_ts_event_chan, &event, K_MSEC(200));
	zassert_equal(rc, 0, "reading the TS channel failed: %d", rc);
	zassert_equal(event.type, ZDB_TS_EVENT_WATERMARK, "channel did not carry the watermark");
	zassert_equal(event.sample_ts_ms, 1234U, "wrong consumed position");

	(void)zdb_ts_close(&ts);
	zdb_deinit(&g_db);
}

ZTEST(zephyrdb_eventing_zbus, test_doc_events_reach_their_channel)
{
	zdb_doc_t doc;
	zdb_doc_event_t event;
	int rc;

	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "init failed");

	zassert_equal(zdb_doc_create(&g_db, "zbdocs", "d1", &doc), ZDB_OK, "create failed");

	rc = zbus_chan_read(&zdb_doc_event_chan, &event, K_MSEC(200));
	zassert_equal(rc, 0, "reading the DOC channel failed: %d", rc);
	zassert_equal(event.type, ZDB_DOC_EVENT_CREATE, "channel did not carry the create");
	zassert_equal(strcmp(event.collection_name, "zbdocs"), 0, "wrong collection");
	zassert_equal(strcmp(event.document_id, "d1"), 0, "wrong document id");

	zassert_equal(zdb_doc_field_set_string(&doc, "name", "Ada"), ZDB_OK, "set failed");
	zassert_equal(zdb_doc_save(&doc), ZDB_OK, "save failed");

	rc = zbus_chan_read(&zdb_doc_event_chan, &event, K_MSEC(200));
	zassert_equal(rc, 0, "reading the DOC channel failed: %d", rc);
	zassert_equal(event.type, ZDB_DOC_EVENT_SAVE, "channel did not carry the save");
	zassert_equal(event.field_count, 1U, "wrong field count");
	zassert_true(event.serialized_bytes > 0U, "save reported no serialized bytes");

	(void)zdb_doc_close(&doc);
	(void)zdb_doc_delete(&g_db, "zbdocs", "d1");
	zdb_deinit(&g_db);
}

/* The core channel needs no data model, and carries lifecycle transitions. */
ZTEST(zephyrdb_eventing_zbus, test_core_events_reach_their_channel)
{
	zdb_core_event_t event;
	int rc;

	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "init failed");

	rc = zbus_chan_read(&zdb_core_event_chan, &event, K_MSEC(200));
	zassert_equal(rc, 0, "reading the core channel failed: %d", rc);
	zassert_equal(event.type, ZDB_CORE_EVENT_INIT, "channel did not carry the init");
	zassert_equal(event.status, ZDB_OK, "wrong status on the channel");
	zassert_equal(event.health, ZDB_HEALTH_OK, "fresh instance is not healthy");

	zassert_equal(zdb_deinit(&g_db), ZDB_OK, "deinit failed");

	rc = zbus_chan_read(&zdb_core_event_chan, &event, K_MSEC(200));
	zassert_equal(rc, 0, "reading the core channel failed: %d", rc);
	zassert_equal(event.type, ZDB_CORE_EVENT_DEINIT, "channel did not carry the deinit");
}

/*
 * Publication is best-effort by contract. Whatever the channel does, the
 * operation's own result must be unaffected.
 */
ZTEST(zephyrdb_eventing_zbus, test_publish_never_changes_operation_status)
{
	zdb_kv_t kv;
	uint32_t value = 1U;
	uint32_t read_back = 0U;
	size_t out_len = 0U;

	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "init failed");
	zassert_equal(zdb_kv_open(&g_db, "zb2", &kv), ZDB_OK, "kv_open failed");

	zassert_equal(zdb_kv_set(&kv, "k", &value, sizeof(value)), ZDB_OK,
		      "publishing changed the set result");
	zassert_equal(zdb_kv_get(&kv, "k", &read_back, sizeof(read_back), &out_len), ZDB_OK,
		      "value did not survive a published set");
	zassert_equal(read_back, value, "wrong value read back");

	/* A rejected operation still returns its own error, not a publish error. */
	zassert_equal(zdb_kv_get(&kv, "absent", &read_back, sizeof(read_back), &out_len),
		      ZDB_ERR_NOT_FOUND, "wrong status for a missing key");

	zdb_kv_close(&kv);
	zdb_deinit(&g_db);
}
