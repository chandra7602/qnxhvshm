#ifndef HVSHM_MSGS_H
#define HVSHM_MSGS_H

#include <stdint.h>

enum hvshm_msg_type {
	HVSHM_MSG_TELEMETRY  = 1,   /* host  -> guest, periodic              */
	HVSHM_MSG_SET_PERIOD = 2,   /* guest -> host,  change telemetry rate */
	HVSHM_MSG_PERIOD_ACK = 3,   /* host  -> guest, reply to SET_PERIOD   */
};

/* Dummy sensor sample; replace with your real signal set. */
struct hvshm_telemetry {
	uint64_t seq;               /* increments by 1 per sample             */
	uint64_t host_time_ns;      /* host CLOCK_MONOTONIC (not comparable
	                             * with the guest clock)                  */
	int32_t  value[4];
};

struct hvshm_set_period {
	uint32_t period_ms;         /* host clamps to [10, 60000]             */
};

struct hvshm_period_ack {
	uint32_t period_ms;         /* value actually applied                 */
};

#endif // HVSHM_MSGS_H