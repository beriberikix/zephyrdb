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

int zdb_eventing_zbus_publish(const zdb_kv_event_t *event);
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
int zdb_eventing_zbus_publish_ts(const zdb_ts_event_t *event);
#endif
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
int zdb_eventing_zbus_publish_doc(const zdb_doc_event_t *event);
#endif

#endif /* CONFIG_ZDB_EVENTING_ZBUS */

#endif /* ZEPHYRDB_EVENTING_ZBUS_H_ */
