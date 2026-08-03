/*
 * Document event delivery: create, save and delete, including the failure
 * statuses that previously never reached a listener.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <string.h>

#include "zephyrdb.h"

#define DOC_EVENTS_MAX 16U

static zdb_doc_event_t g_events[DOC_EVENTS_MAX];
static uint32_t g_event_count;
static uint32_t g_second_count;

static void doc_capture(const zdb_doc_event_t *event, void *user_ctx)
{
	ARG_UNUSED(user_ctx);

	if (event == NULL) {
		return;
	}

	if (g_event_count < DOC_EVENTS_MAX) {
		g_events[g_event_count] = *event;
	}
	g_event_count++;
}

static void doc_second(const zdb_doc_event_t *event, void *user_ctx)
{
	ARG_UNUSED(user_ctx);
	ARG_UNUSED(event);

	g_second_count++;
}

static const zdb_doc_event_listener_t g_listeners[] = {
	/* A null-notify slot must be skipped rather than dereferenced. */
	{ .notify = NULL,        .user_ctx = NULL },
	{ .notify = doc_capture, .user_ctx = NULL },
	{ .notify = doc_second,  .user_ctx = NULL },
};

static const zdb_cfg_t g_cfg = {
	.kv_backend_fs = NULL,
	.lfs_mount_point = "/lfs",
	.work_q = &k_sys_work_q,
	.doc_event_listeners = g_listeners,
	.doc_event_listener_count = ARRAY_SIZE(g_listeners),
};

ZDB_DEFINE_STATIC(g_db, g_cfg);

static const zdb_doc_event_t *last_of(zdb_doc_event_type_t type)
{
	uint32_t i = (g_event_count < DOC_EVENTS_MAX) ? g_event_count : DOC_EVENTS_MAX;

	while (i > 0U) {
		i--;
		if (g_events[i].type == type) {
			return &g_events[i];
		}
	}

	return NULL;
}

static void setup(void *fixture)
{
	ARG_UNUSED(fixture);

	(void)memset(g_events, 0, sizeof(g_events));
	g_event_count = 0U;
	g_second_count = 0U;
	zassert_equal(zdb_init(&g_db, &g_cfg), ZDB_OK, "zdb_init failed");
}

static void teardown(void *fixture)
{
	ARG_UNUSED(fixture);
	(void)zdb_deinit(&g_db);
}

ZTEST_SUITE(zephyrdb_doc_events, NULL, NULL, setup, teardown, NULL);

/* Creating a handle reports the collection and document it is bound to. */
ZTEST(zephyrdb_doc_events, test_create_emits_event)
{
	zdb_doc_t doc;

	zassert_equal(zdb_doc_create(&g_db, "people", "d1", &doc), ZDB_OK, "create failed");

	zassert_equal(g_event_count, 1U, "expected 1 event, got %u", g_event_count);
	zassert_equal(g_events[0].type, ZDB_DOC_EVENT_CREATE, "wrong event type");
	zassert_equal(g_events[0].status, ZDB_OK, "create event status not OK");
	zassert_equal(strcmp(g_events[0].collection_name, "people"), 0, "wrong collection");
	zassert_equal(strcmp(g_events[0].document_id, "d1"), 0, "wrong document id");
	zassert_equal(g_second_count, 1U, "second listener was not called");

	(void)zdb_doc_close(&doc);
}

/*
 * A save reports how much actually reached storage, which is the number a
 * listener watching space consumption needs.
 */
ZTEST(zephyrdb_doc_events, test_save_reports_size_and_field_count)
{
	zdb_doc_t doc;
	const zdb_doc_event_t *save;

	zassert_equal(zdb_doc_create(&g_db, "people", "d2", &doc), ZDB_OK, "create failed");
	zassert_equal(zdb_doc_field_set_string(&doc, "name", "Ada"), ZDB_OK, "set name failed");
	zassert_equal(zdb_doc_field_set_bool(&doc, "active", true), ZDB_OK, "set active failed");
	zassert_equal(zdb_doc_save(&doc), ZDB_OK, "save failed");

	save = last_of(ZDB_DOC_EVENT_SAVE);
	zassert_not_null(save, "no save event delivered");
	zassert_equal(save->status, ZDB_OK, "save event status not OK");
	zassert_equal(save->field_count, 2U, "wrong field count: %zu", save->field_count);
	zassert_true(save->serialized_bytes > 0U, "save reported no serialized bytes");
	zassert_equal(strcmp(save->document_id, "d2"), 0, "wrong document id");

	(void)zdb_doc_close(&doc);
	(void)zdb_doc_delete(&g_db, "people", "d2");
}

/* Deleting a stored document reports it. */
ZTEST(zephyrdb_doc_events, test_delete_emits_event)
{
	zdb_doc_t doc;
	const zdb_doc_event_t *del;

	zassert_equal(zdb_doc_create(&g_db, "people", "d3", &doc), ZDB_OK, "create failed");
	zassert_equal(zdb_doc_field_set_string(&doc, "name", "Grace"), ZDB_OK, "set failed");
	zassert_equal(zdb_doc_save(&doc), ZDB_OK, "save failed");
	(void)zdb_doc_close(&doc);

	zassert_equal(zdb_doc_delete(&g_db, "people", "d3"), ZDB_OK, "delete failed");

	del = last_of(ZDB_DOC_EVENT_DELETE);
	zassert_not_null(del, "no delete event delivered");
	zassert_equal(del->status, ZDB_OK, "delete event status not OK");
	zassert_equal(strcmp(del->document_id, "d3"), 0, "wrong document id");
	zassert_equal(del->serialized_bytes, 0U, "delete should report no size");
}

/*
 * The point of carrying a status: a delete that did not remove anything is
 * reported as such, rather than looking identical to a successful one.
 */
ZTEST(zephyrdb_doc_events, test_delete_reports_failure_status)
{
	const zdb_doc_event_t *del;
	zdb_status_t rc;

	rc = zdb_doc_delete(&g_db, "people", "never_stored");
	zassert_not_equal(rc, ZDB_OK, "deleting a missing document unexpectedly succeeded");

	del = last_of(ZDB_DOC_EVENT_DELETE);
	zassert_not_null(del, "a failed delete produced no event");
	zassert_equal(del->status, rc, "event status %d does not match the return value %d",
		      (int)del->status, (int)rc);
	zassert_equal(strcmp(del->document_id, "never_stored"), 0, "wrong document id");
}

/*
 * Rejected before anything is touched: names that fail validation never reach
 * the emit point, so they stay silent.
 */
ZTEST(zephyrdb_doc_events, test_no_event_on_rejected_name)
{
	zdb_doc_t doc;

	zassert_not_equal(zdb_doc_create(&g_db, "../escape", "d4", &doc), ZDB_OK,
			  "path traversal was accepted");
	zassert_equal(g_event_count, 0U, "expected no event, got %u", g_event_count);
}
