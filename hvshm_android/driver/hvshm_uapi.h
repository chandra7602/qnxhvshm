#ifndef HVSHM_UAPI_H
#define HVSHM_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define HVSHM_EVENT_MAX_PAYLOAD 512
#define HVSHM_WAIT_FOREVER      0xffffffffu
#define HVSHM_NO_CONN           0xffffffffu

enum hvshm_event_type {
    HVSHM_EV_NONE              = 0,
    HVSHM_EV_PEER_CONNECTED    = 1,
    HVSHM_EV_PEER_DISCONNECTED = 2,
    HVSHM_EV_MESSAGE           = 3,
};

struct hvshm_event {
    __u32 type;
    __u32 conn_idx;
    __u32 msg_type;
    __u32 len;
    __u8 payload[HVSHM_EVENT_MAX_PAYLOAD];
};

struct hvshm_wait_arg {
    __u32 msg_type;
    __u32 reserved;
    struct hvshm_event ev;
};

struct hvshm_send_arg {
    __u32 msg_type;
    __u32 len;
    __u8 payload[HVSHM_EVENT_MAX_PAYLOAD];
};

struct hvshm_info {
    __u64 region_bytes;
    __u32 conn_idx;
    __u32 attach_mask;
    __u32 host_idx;
    __u32 host_online;
    __u32 ring_bytes;
    __u32 reserved;
};

#define HVSHM_IOC_MAGIC       'H'
#define HVSHM_IOC_GET_INFO    _IOR(HVSHM_IOC_MAGIC, 1, struct hvshm_info)
#define HVSHM_IOC_WAIT_EVENT  _IOWR(HVSHM_IOC_MAGIC, 2, struct hvshm_wait_arg)
#define HVSHM_IOC_SEND        _IOW (HVSHM_IOC_MAGIC, 3, struct hvshm_send_arg)

#endif // HVSHM_UAPI_H