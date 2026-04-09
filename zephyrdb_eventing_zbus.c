#include "zephyrdb_eventing_zbus.h"

#if defined(CONFIG_ZDB_EVENTING_ZBUS) && (CONFIG_ZDB_EVENTING_ZBUS)

#include <errno.h>
#include <zephyr/kernel.h>

ZBUS_CHAN_DEFINE(zdb_kv_event_chan, zdb_kv_event_t, NULL, NULL,
		 ZBUS_OBSERVERS_EMPTY, ZBUS_MSG_INIT(0));

int zdb_eventing_zbus_publish(const zdb_kv_event_t *event)
{
	if (event == NULL) {
		return -EINVAL;
	}

	return zbus_chan_pub(&zdb_kv_event_chan, event, K_NO_WAIT);
}

#endif /* CONFIG_ZDB_EVENTING_ZBUS */
