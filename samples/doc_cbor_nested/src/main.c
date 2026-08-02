/*
 * Copyright (c) 2026 ZephyrDB Contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * Storing a nested structure in a document field.
 *
 * Document fields hold scalars, strings, and raw bytes. When a value has
 * structure of its own — a sub-object, or a list — encode it and store the
 * result in a BYTES field. This sample uses CBOR via zcbor, which Zephyr
 * already ships, to store:
 *
 *   sensor: { model: "ACME-42", cal: [11, 22, 33] }
 *
 * alongside ordinary flat fields, then reads it back and decodes it.
 *
 * The tradeoff: the encoded field is opaque to zdb_doc_query(), so keep
 * anything you need to filter on as a flat field. Here "sensor_id" stays flat
 * for exactly that reason.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zcbor_encode.h>
#include <zcbor_decode.h>
#include <string.h>

#include "zephyrdb.h"

#define COLLECTION "devices"
#define DOCUMENT   "probe1"

#define CAL_POINTS 3

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

/* Encode { model: <text>, cal: [ ... ] } into @p out. */
static bool encode_sensor(uint8_t *out, size_t out_size, const char *model,
			  const int32_t *cal, size_t cal_count, size_t *out_len)
{
	ZCBOR_STATE_E(state, 2, out, out_size, 1);
	bool ok;

	ok = zcbor_map_start_encode(state, 2);
	ok = ok && zcbor_tstr_put_lit(state, "model");
	ok = ok && zcbor_tstr_put_term(state, model, 32U);
	ok = ok && zcbor_tstr_put_lit(state, "cal");
	ok = ok && zcbor_list_start_encode(state, cal_count);
	for (size_t i = 0U; ok && (i < cal_count); i++) {
		ok = zcbor_int32_put(state, cal[i]);
	}
	ok = ok && zcbor_list_end_encode(state, cal_count);
	ok = ok && zcbor_map_end_encode(state, 2);

	if (!ok) {
		return false;
	}

	*out_len = (size_t)(state->payload - out);
	return true;
}

/* Decode what encode_sensor() produced. */
static bool decode_sensor(const uint8_t *buf, size_t len, char *model, size_t model_size,
			  int32_t *cal, size_t cal_count)
{
	ZCBOR_STATE_D(state, 2, buf, len, 1, 0);
	struct zcbor_string model_str;
	bool ok;

	ok = zcbor_map_start_decode(state);
	ok = ok && zcbor_tstr_expect_lit(state, "model");
	ok = ok && zcbor_tstr_decode(state, &model_str);
	if (!ok || (model_str.len >= model_size)) {
		return false;
	}
	(void)memcpy(model, model_str.value, model_str.len);
	model[model_str.len] = '\0';

	ok = zcbor_tstr_expect_lit(state, "cal");
	ok = ok && zcbor_list_start_decode(state);
	for (size_t i = 0U; ok && (i < cal_count); i++) {
		ok = zcbor_int32_decode(state, &cal[i]);
	}
	ok = ok && zcbor_list_end_decode(state);
	ok = ok && zcbor_map_end_decode(state);

	return ok;
}

int main(void)
{
	zdb_doc_t doc;
	zdb_doc_t reopened;
	uint8_t cbor[64];
	size_t cbor_len = 0U;
	const int32_t cal[CAL_POINTS] = {11, 22, 33};
	char model[32];
	int32_t read_cal[CAL_POINTS] = {0};
	zdb_bytes_t stored = {0};
	int64_t sensor_id = 0;
	zdb_status_t rc;

	rc = zdb_init(&g_db, &g_cfg);
	if (rc != ZDB_OK) {
		printk("doc_cbor_nested: init failed rc=%d\n", (int)rc);
		return 1;
	}

	if (!encode_sensor(cbor, sizeof(cbor), "ACME-42", cal, CAL_POINTS, &cbor_len)) {
		printk("doc_cbor_nested: CBOR encode failed\n");
		(void)zdb_deinit(&g_db);
		return 1;
	}
	printk("doc_cbor_nested: encoded %u bytes of CBOR\n", (unsigned)cbor_len);

	rc = zdb_doc_create(&g_db, COLLECTION, DOCUMENT, &doc);
	if (rc != ZDB_OK) {
		printk("doc_cbor_nested: create failed rc=%d\n", (int)rc);
		(void)zdb_deinit(&g_db);
		return 1;
	}

	/* Keep queryable values flat; the nested part rides along as bytes. */
	rc = zdb_doc_field_set_i64(&doc, "sensor_id", 7);
	if (rc == ZDB_OK) {
		rc = zdb_doc_field_set_bytes(&doc, "sensor", cbor, cbor_len);
	}
	if (rc == ZDB_OK) {
		rc = zdb_doc_save(&doc);
	}
	(void)zdb_doc_close(&doc);

	if (rc != ZDB_OK) {
		printk("doc_cbor_nested: save failed rc=%d (needs a filesystem at %s)\n",
		       (int)rc, g_cfg.lfs_mount_point);
		(void)zdb_deinit(&g_db);
		return 1;
	}

	rc = zdb_doc_open(&g_db, COLLECTION, DOCUMENT, &reopened);
	if (rc != ZDB_OK) {
		printk("doc_cbor_nested: open failed rc=%d\n", (int)rc);
		(void)zdb_deinit(&g_db);
		return 1;
	}

	rc = zdb_doc_field_get_i64(&reopened, "sensor_id", &sensor_id);
	if (rc == ZDB_OK) {
		rc = zdb_doc_field_get_bytes(&reopened, "sensor", &stored);
	}
	if (rc != ZDB_OK) {
		printk("doc_cbor_nested: read back failed rc=%d\n", (int)rc);
		(void)zdb_doc_close(&reopened);
		(void)zdb_deinit(&g_db);
		return 1;
	}

	if (!decode_sensor(stored.data, stored.len, model, sizeof(model), read_cal,
			   CAL_POINTS)) {
		printk("doc_cbor_nested: CBOR decode failed\n");
		(void)zdb_doc_close(&reopened);
		(void)zdb_deinit(&g_db);
		return 1;
	}

	printk("doc_cbor_nested: sensor model=%s cal=[%d,%d,%d]\n", model, (int)read_cal[0],
	       (int)read_cal[1], (int)read_cal[2]);

	if ((sensor_id != 7) || (strcmp(model, "ACME-42") != 0) || (read_cal[0] != cal[0]) ||
	    (read_cal[1] != cal[1]) || (read_cal[2] != cal[2])) {
		printk("doc_cbor_nested: value mismatch after round-trip\n");
		(void)zdb_doc_close(&reopened);
		(void)zdb_deinit(&g_db);
		return 1;
	}

	printk("doc_cbor_nested: PASS\n");

	(void)zdb_doc_close(&reopened);
	(void)zdb_deinit(&g_db);
	return 0;
}
