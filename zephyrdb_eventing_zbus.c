#include "zephyrdb_eventing_zbus.h"

#if defined(CONFIG_ZDB_EVENTING_ZBUS) && (CONFIG_ZDB_EVENTING_ZBUS)

#include <errno.h>
#include <zephyr/kernel.h>

ZBUS_CHAN_DEFINE(zdb_kv_event_chan, zdb_kv_event_t, NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));
#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
ZBUS_CHAN_DEFINE(zdb_ts_event_chan, zdb_ts_event_t, NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));
#endif
#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
ZBUS_CHAN_DEFINE(zdb_doc_event_chan, zdb_doc_event_t, NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));
#endif

int zdb_eventing_zbus_publish(const zdb_kv_event_t *event)
{
	if (event == NULL) {
		return -EINVAL;
	}

	return zbus_chan_pub(&zdb_kv_event_chan, event, K_NO_WAIT);
}

#if defined(CONFIG_ZDB_TS) && (CONFIG_ZDB_TS)
int zdb_eventing_zbus_publish_ts(const zdb_ts_event_t *event)
{
	if (event == NULL) {
		return -EINVAL;
	}

	return zbus_chan_pub(&zdb_ts_event_chan, event, K_NO_WAIT);
}
#endif

#if defined(CONFIG_ZDB_DOC) && (CONFIG_ZDB_DOC)
int zdb_eventing_zbus_publish_doc(const zdb_doc_event_t *event)
{
	if (event == NULL) {
		return -EINVAL;
	}

	return zbus_chan_pub(&zdb_doc_event_chan, event, K_NO_WAIT);
}
#endif

#endif /* CONFIG_ZDB_EVENTING_ZBUS */
