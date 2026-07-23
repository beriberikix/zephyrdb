/**
 * @file zephyrdb_eventing_zbus.h
 * @brief zbus adapter for ZephyrDB events (@c CONFIG_ZDB_EVENTING_ZBUS).
 *
 * @defgroup zdb_eventing_zbus zbus adapter
 * @ingroup zdb_eventing
 * @brief Publishes ZephyrDB events on zbus channels.
 *
 * Channels: @c zdb_kv_event_chan (::zdb_kv_event_t),
 * @c zdb_ts_event_chan (::zdb_ts_event_t), and
 * @c zdb_doc_event_chan (::zdb_doc_event_t). Publication is best-effort
 * and never changes the originating operation's return value; the usual
 * pattern is to call these publish helpers from event listener callbacks
 * registered via ::zdb_cfg_t.
 * @{
 */

#ifndef ZEPHYRDB_EVENTING_ZBUS_H_
#define ZEPHYRDB_EVENTING_ZBUS_H_

#include "zephyrdb.h"

#if defined(CONFIG_ZDB_EVENTING_ZBUS) && (CONFIG_ZDB_EVENTING_ZBUS)
#include <zephyr/zbus/zbus.h>

ZBUS_CHAN_DECLARE(zdb_kv_event_chan);
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
ZBUS_CHAN_DECLARE(zdb_ts_event_chan);
#endif
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
ZBUS_CHAN_DECLARE(zdb_doc_event_chan);
#endif

/**
 * @brief Publish a KV event on @c zdb_kv_event_chan.
 *
 * @param event Event to publish.
 * @return 0 on success, otherwise a negative errno from zbus_chan_pub()
 *         (-EINVAL for a NULL @p event).
 */
int zdb_eventing_zbus_publish(const zdb_kv_event_t *event);
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
/**
 * @brief Publish a time-series event on @c zdb_ts_event_chan.
 *
 * @param event Event to publish.
 * @return 0 on success, otherwise a negative errno from zbus_chan_pub()
 *         (-EINVAL for a NULL @p event).
 */
int zdb_eventing_zbus_publish_ts(const zdb_ts_event_t *event);
#endif
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
/**
 * @brief Publish a document event on @c zdb_doc_event_chan.
 *
 * @param event Event to publish.
 * @return 0 on success, otherwise a negative errno from zbus_chan_pub()
 *         (-EINVAL for a NULL @p event).
 */
int zdb_eventing_zbus_publish_doc(const zdb_doc_event_t *event);
#endif

#endif /* CONFIG_ZDB_EVENTING_ZBUS */

/** @} */ /* zdb_eventing_zbus */

#endif /* ZEPHYRDB_EVENTING_ZBUS_H_ */
