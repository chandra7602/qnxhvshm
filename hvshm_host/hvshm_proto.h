#ifndef HVSHM_PROTO_H
#define HVSHM_PROTO_H

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#else
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#endif

#define HVSHM_MAGIC             0x4D485648u
#define HVSHM_PROTO_VERSION     1u
#define HVSHM_HEADER_BYTES      4096u
#define HVSHM_CACHELINE         64u
#define HVSHM_MSG_ALIGN         8u
#define HVSHM_MSG_MAX_PAYLOAD   512u

#define HVSHM_RING_BYTES_MIN    4096u
#define HVSHM_RING_BYTES_MAX    (1u << 20)
#define HVSHM_NO_CONN           0xffffffffu

#define HVSHM_LOAD_ACQ(p)       __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define HVSHM_LOAD_RLX(p)       __atomic_load_n((p), __ATOMIC_RELAXED)
#define HVSHM_STORE_REL(p, v)   __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#define HVSHM_STORE_RLX(p, v)   __atomic_store_n((p), (v), __ATOMIC_RELAXED)

struct hvshm_ring_ctl {
	uint32_t head __attribute__((aligned(HVSHM_CACHELINE))); /* producer-owned */
	uint32_t tail __attribute__((aligned(HVSHM_CACHELINE))); /* consumer-owned */
};

struct hvshm_header {
	union {
		struct {
			uint32_t magic;       /* host writes last; 0 while not served */
			uint32_t version;
			uint32_t epoch;       /* +1 on every host (re)init            */
			uint32_t ring_bytes;
			uint32_t h2g_offset;
			uint32_t g2h_offset;
			uint32_t host_idx;    /* host's vdev connection index         */
			uint32_t reserved0;
			struct hvshm_ring_ctl h2g;
			struct hvshm_ring_ctl g2h;
		};
		uint8_t reserved[HVSHM_HEADER_BYTES];
	};
} __attribute__((aligned(HVSHM_CACHELINE)));

_Static_assert(sizeof(struct hvshm_header) == HVSHM_HEADER_BYTES, "hvshm_header must be exactly one 4 KiB page");

struct hvshm_msg_hdr {
	uint32_t len;
	uint32_t type;
};

struct hvshm_ring {
	struct hvshm_ring_ctl *ctl;
	uint8_t *data;
	uint32_t size;                  /* power of two */
};

struct hvshm_channel {
	struct hvshm_header *hdr;
	struct hvshm_ring tx;
	struct hvshm_ring rx;
	uint32_t epoch;
};

static inline uint32_t hvshm_align(uint32_t v) {
	return (v + (HVSHM_MSG_ALIGN - 1u)) & ~(HVSHM_MSG_ALIGN - 1u);
}

static inline void hvshm_copy_in(const struct hvshm_ring *r, uint32_t pos, const void *src, uint32_t n) {
	uint32_t off = pos & (r->size - 1u);
	uint32_t first = r->size - off;

	if (first > n) first = n;
	memcpy(r->data + off, src, first);
	memcpy(r->data, (const uint8_t *)src + first, n - first);
}

static inline void hvshm_copy_out(const struct hvshm_ring *r, uint32_t pos, void *dst, uint32_t n) {
	uint32_t off = pos & (r->size - 1u);
	uint32_t first = r->size - off;

	if (first > n) first = n;
	memcpy(dst, r->data + off, first);
	memcpy((uint8_t *)dst + first, r->data, n - first);
}

static inline uint32_t hvshm_pick_ring_bytes(size_t region_size) {
	size_t avail;
	uint32_t sz = HVSHM_RING_BYTES_MAX;

	if (region_size <= HVSHM_HEADER_BYTES) return 0;
	avail = (region_size - HVSHM_HEADER_BYTES) / 2u;
	while (sz && sz > avail) sz >>= 1;
	return sz >= HVSHM_RING_BYTES_MIN ? sz : 0;
}

static inline int hvshm_bind(void *base, size_t region_size, bool host, struct hvshm_channel *ch) {
    struct hvshm_header *hdr = (struct hvshm_header *)base;
    struct hvshm_ring h2g, g2h;
    uint32_t rb;

    if (region_size < HVSHM_HEADER_BYTES) return -EPROTO;
    if (HVSHM_LOAD_ACQ(&hdr->magic) != HVSHM_MAGIC) return -ENOTCONN;
    if (hdr->version != HVSHM_PROTO_VERSION) return -EPROTONOSUPPORT;

    rb = hdr->ring_bytes;
    if (rb < HVSHM_RING_BYTES_MIN || rb > HVSHM_RING_BYTES_MAX || (rb & (rb -1u))) return -EPROTO;
    if (hdr->h2g_offset != HVSHM_HEADER_BYTES || hdr->g2h_offset != HVSHM_HEADER_BYTES + rb) return -EPROTO;
    if ((size_t)HVSHM_HEADER_BYTES + 2u * (size_t)rb > region_size) return -EPROTO;

    h2g.ctl = &hdr->h2g;
    h2g.data = (uint8_t *)base + HVSHM_HEADER_BYTES;
    h2g.size = rb;
    g2h.ctl = &hdr->g2h;
    g2h.data = (uint8_t *)base + HVSHM_HEADER_BYTES + rb;
    g2h.size = rb;

    ch->hdr = hdr;
    ch->tx = host ? h2g : g2h;
    ch->rx = host ? g2h : h2g;
    ch->epoch = HVSHM_LOAD_RLX(&hdr->epoch);
    return 0;
}

static inline int hvshm_host_init(void *base, size_t region_size, uint32_t host_idx, struct hvshm_channel *ch) {
    struct hvshm_header *hdr = (struct hvshm_header *)base;
    uint32_t rb = hvshm_pick_ring_bytes(region_size);
    uint32_t epoch;

    if (!rb) return -ENOSPC;

    HVSHM_STORE_REL(&hdr->magic, 0);
    epoch = HVSHM_LOAD_RLX(&hdr->epoch) + 1u;

    memset(hdr->reserved, 0, sizeof(hdr->reserved));
    hdr->version = HVSHM_PROTO_VERSION;
    hdr->ring_bytes = rb;
    hdr->h2g_offset = HVSHM_HEADER_BYTES;
    hdr->g2h_offset = HVSHM_HEADER_BYTES + rb;
    hdr->host_idx = host_idx;
    HVSHM_STORE_RLX(&hdr->epoch, epoch);
    HVSHM_STORE_REL(&hdr->magic, HVSHM_MAGIC);
    return hvshm_bind(base, region_size, true, ch);
}

static inline void hvshm_host_shutdown(struct hvshm_channel *ch) {
	HVSHM_STORE_REL(&ch->hdr->magic, 0);
}

static inline int hvshm_guest_attach(void *base, size_t region_size, struct hvshm_channel *ch) {
	return hvshm_bind(base, region_size, false, ch);
}

static inline bool hvshm_channel_alive(const struct hvshm_channel *ch) {
	return HVSHM_LOAD_ACQ(&ch->hdr->magic) == HVSHM_MAGIC && HVSHM_LOAD_RLX(&ch->hdr->epoch) == ch->epoch;
}

static inline bool hvshm_rx_pending(const struct hvshm_channel *ch) {
	return HVSHM_LOAD_ACQ(&ch->rx.ctl->head) != HVSHM_LOAD_RLX(&ch->rx.ctl->tail);
}

static inline void hvshm_rx_flush(const struct hvshm_channel *ch) {
	HVSHM_STORE_REL(&ch->rx.ctl->tail, HVSHM_LOAD_ACQ(&ch->rx.ctl->head));
}

static inline int hvshm_send(const struct hvshm_channel *ch, uint32_t type, const void *payload, uint32_t len) {
    const struct hvshm_ring *r = &ch->tx;
    struct hvshm_msg_hdr h;
    uint32_t need, head, tail, used;

    if (len > HVSHM_MSG_MAX_PAYLOAD) return -EMSGSIZE;

    h.len = len;
    h.type = type;
    need = (uint32_t)sizeof(h) + hvshm_align(len);

    head = HVSHM_LOAD_RLX(&r->ctl->head);
    tail = HVSHM_LOAD_ACQ(&r->ctl->tail);
    used = head - tail;
    if (used > r->size) return -EPROTO;
    if ((r->size - used) < need) return -ENOSPC;

    hvshm_copy_in(r, head, &h, (uint32_t)sizeof(h));
    if (len) hvshm_copy_in(r, head + (uint32_t)sizeof(h), payload, len);
    HVSHM_STORE_REL(&r->ctl->head, head + need);
    return 0;
}

static inline int hvshm_recv(const struct hvshm_channel *ch, uint32_t *type, void *buf, uint32_t cap, uint32_t *out_len) {
    const struct hvshm_ring *r = &ch->rx;
    struct hvshm_msg_hdr h;
    uint32_t head, tail, used, need;

    tail = HVSHM_LOAD_RLX(&r->ctl->tail);
    head = HVSHM_LOAD_ACQ(&r->ctl->head);
    used = head - tail;

    if (used == 0) return -EAGAIN;
    if (used > r->size || used < sizeof(h)) return -EPROTO;

    hvshm_copy_out(r, tail, &h, (uint32_t)sizeof(h));
    if (h.len > HVSHM_MSG_MAX_PAYLOAD) return -EPROTO;
    need = (uint32_t)sizeof(h) + hvshm_align(h.len);
    if (need > used) return -EPROTO;
    if (h.len > cap) return -EMSGSIZE;

    if (h.len) hvshm_copy_out(r, tail + (uint32_t)sizeof(h), buf, h.len);
    HVSHM_STORE_REL(&r->ctl->tail, tail + need);
    *type = h.type;
    *out_len = h.len;
    return 0;
}

#endif // HVSHM_PROTO_H
