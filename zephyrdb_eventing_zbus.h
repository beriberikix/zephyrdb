#ifndef ZEPHYRDB_EVENTING_ZBUS_H_
#define ZEPHYRDB_EVENTING_ZBUS_H_

#include "zephyrdb.h"

#if defined(CONFIG_ZDB_EVENTING_ZBUS) && (CONFIG_ZDB_EVENTING_ZBUS)
#include <zephyr/zbus/zbus.h>

ZBUS_CHAN_DECLARE(zdb_kv_event_chan);

int zdb_eventing_zbus_publish(const zdb_kv_event_t *event);

#endif /* CONFIG_ZDB_EVENTING_ZBUS */

#endif /* ZEPHYRDB_EVENTING_ZBUS_H_ */
