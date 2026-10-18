#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/iomgr.h>
#include <sys/neutrino.h>
#include <sys/siginfo.h>
#include <hyp_shm.h>

#include "hvshm_msgs.h"
#include "hvshm_proto.h"

#define PULSE_CODE_NOTIFY   0          /* vdev notification / attach change */
#define PULSE_CODE_TICK     1          /* telemetry timer                   */

#define DEFAULT_NAME        "qnx_android_shm"
#define DEFAULT_PAGES       240u
#define DEFAULT_PERIOD_MS   1000u
#define MIN_PERIOD_MS       10u
#define MAX_PERIOD_MS       60000u
#define PAGE_BYTES          0x1000u
#define CLIENT_MASK         ((1u << HYP_SHM_MAX_CLIENTS) - 1u)

struct host_ctx {
	struct hyp_shm *hsp;
	struct hvshm_channel ch;
	void *region;
	size_t region_bytes;
	unsigned my_idx;
	unsigned attach_list;          /* last attach bitset we processed */
	uint32_t period_ms;
	uint64_t seq;
	uint64_t dropped;
	timer_t timer;
};

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
	(void)sig;
	g_stop = 1;
}

static uint64_t now_ns(void) {
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static unsigned peers_mask(const struct host_ctx *c) {
	return c->attach_list & ~(1u << c->my_idx);
}

static int arm_timer(struct host_ctx *c) {
	struct itimerspec its;

	its.it_value.tv_sec     = c->period_ms / 1000u;
	its.it_value.tv_nsec    = (long)(c->period_ms % 1000u) * 1000000L;
	its.it_interval         = its.it_value;
	return timer_settime(c->timer, 0, &its, NULL);
}

static void disarm_timer(struct host_ctx *c) {
	struct itimerspec its;

	memset(&its, 0, sizeof(its));
	timer_settime(c->timer, 0, &its, NULL);
}

static int send_msg(struct host_ctx *c, uint32_t type, const void *p, uint32_t len) {
	int rc = hvshm_send(&c->ch, type, p, len);

	if (rc == -EPROTO) {
		fprintf(stderr, "tx ring corrupted by peer, re-initialising channel\n");
		hvshm_host_init(c->region, c->region_bytes, c->my_idx, &c->ch);
		rc = hvshm_send(&c->ch, type, p, len);
	}
	if (rc == 0)
		hyp_shm_poke(c->hsp, HYP_SHM_POKE_ALL_BUT_ME);
	return rc;
}

static void publish_telemetry(struct host_ctx *c) {
    struct hvshm_telemetry t;
    int rc;

    if (!peers_mask(c)) return;

    t.seq          = c->seq;
    t.host_time_ns = now_ns();
    t.value[0]     = (int32_t)((c->seq * 7u) % 1000u);
	t.value[1]     = (int32_t)(c->seq % 2u ? 1 : 0);
	t.value[2]     = (int32_t)(-500 + (int32_t)((c->seq * 13u) % 1000u));
	t.value[3]     = (int32_t)c->seq;

    rc = send_msg(c, HVSHM_MSG_TELEMETRY, &t, sizeof(t));
    if (rc == 0) c->seq++;
    else if (rc == -ENOSPC) {
        c->dropped++;
        c->seq++;
    } else {
        fprintf(stderr, "telemetry send failed: %d\n", rc);
    }
}

static void handle_rx(struct host_ctx *c) {
    uint8_t buf[HVSHM_MSG_MAX_PAYLOAD];
    uint32_t type, len;
    int rc;

    while ((rc = hvshm_recv(&c->ch, &type, buf, sizeof(buf), &len)) == 0) {
        if (type == HVSHM_MSG_SET_PERIOD && len == sizeof(struct hvshm_set_period)) {
            struct hvshm_set_period req;
            struct hvshm_period_ack ack;

            memcpy(&req, buf, sizeof(req));
            if (req.period_ms < MIN_PERIOD_MS) req.period_ms = MIN_PERIOD_MS;
            if (req.period_ms > MAX_PERIOD_MS) req.period_ms = MAX_PERIOD_MS;
            c->period_ms = req.period_ms;
            arm_timer(c);
            ack.period_ms = c->period_ms;
            printf("guest set period to %u ms\n", c->period_ms);
            send_msg(c, HVSHM_MSG_PERIOD_ACK, &ack, sizeof(ack));
        } else {
            printf("ignoring message type=%u len=%u\n", type, len);
        }
    }
    if (rc == -EPROTO) {
        fprintf(stderr, "rx ring corrupted by peer, re-initilising channel\n");
        hvshm_host_init(c->region, c->region_bytes, c->my_idx, &c->ch);
    }
}

static void handle_status(struct host_ctx *c) {
    const unsigned status = hyp_shm_status(c->hsp);
    const unsigned attach = status >> HYP_SHM_MAX_CLIENTS;
    const unsigned self = 1u << c->my_idx;
    const unsigned up = attach & ~c->attach_list & ~self;
    const unsigned down = c->attach_list & ~attach & ~self;
    const int had_peers = peers_mask(c) != 0;
    unsigned i;

    for (i = 0; i < HYP_SHM_MAX_CLIENTS; i++) {
        if (up & (1u << i)) printf("client %u connected\n", i);
        if (down & (1u << i)) printf("client %u disconnected\n", i);
    }
    c->attach_list = attach;

    if (status & CLIENT_MASK) handle_rx(c);

    if (!had_peers && peers_mask(c)) {
        c->seq = 0;
        publish_telemetry(c);
    } else if (had_peers && !peers_mask(c)) {
        printf("no client attached: telemetry paused\n");
    }
}

int main(int argc, char *argv[]) {
    const char *name = (argc > 1) ? argv[1] : DEFAULT_NAME;
    const unsigned pages = (argc > 2) ? (unsigned)strtoul(argv[2], NULL, 0) : DEFAULT_PAGES;
    struct host_ctx ctx;
    struct sigevent ev;
    int channel, coid, err;

    memset(&ctx, 0, sizeof(ctx));
    ctx.period_ms = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : DEFAULT_PERIOD_MS;
    if (ctx.period_ms < MIN_PERIOD_MS || ctx.period_ms > MAX_PERIOD_MS) {
        fprintf(stderr, "period_ms must be %u..%u\n", MIN_PERIOD_MS, MAX_PERIOD_MS);
        return EXIT_FAILURE;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    channel = ChannelCreate_r(_NTO_CHF_COID_DISCONNECT);
    if (channel < 0) {
        fprintf(stderr, "ChannelCreate failed: %d\n", -channel);
        return EXIT_FAILURE;
    }

    coid = ConnectAttach_r(0, 0, channel, _NTO_SIDE_CHANNEL, 0);
    if (coid < 0) {
        fprintf(stderr, "ConnectAttach failed: %d\n", -coid);
        return EXIT_FAILURE;
    }

    ctx.hsp = hyp_shm_create(0);
    if (ctx.hsp == NULL) {
        fprintf(stderr, "hyp_shm_create failed\n");
        return EXIT_FAILURE;
    }

    err = hyp_shm_attach(ctx.hsp, name, pages, channel, -1, PULSE_CODE_NOTIFY, NULL);
    if (err != EOK) {
        fprintf(stderr, "hyp_shm_attach('%s') failed: %d\n", name, err);
        return EXIT_FAILURE;
    }

    ctx.region = hyp_shm_data(ctx.hsp);
    ctx.region_bytes = (size_t)hyp_shm_size(ctx.hsp) * PAGE_BYTES;
    ctx.my_idx = hyp_shm_idx(ctx.hsp);
    ctx.attach_list = hyp_shm_status(ctx.hsp) >> HYP_SHM_MAX_CLIENTS;

    err = hvshm_host_init(ctx.region, ctx.region_bytes, ctx.my_idx, &ctx.ch);
    if (err) {
        fprintf(stderr, "hvshm_host_init failed: %d (region %zu B)\n", err, ctx.region_bytes);
        return EXIT_FAILURE;
    }
    printf("attached '%s': idx=%u region=%zu B ring=%u B period=%u ms\n",
        name, ctx.my_idx, ctx.region_bytes, ctx.ch.tx.size, ctx.period_ms);

    SIGEV_PULSE_INIT(&ev, coid, -1, PULSE_CODE_TICK, 0);
    if (timer_create(CLOCK_MONOTONIC, &ev, &ctx.timer) == -1 || arm_timer(&ctx) == -1) {
        perror("timer");
        return EXIT_FAILURE;
    }

    if (peers_mask(&ctx)) printf("client already attached: 0x%x\n", peers_mask(&ctx));

    while (!g_stop) {
        struct _pulse pulse;

        if (MsgReceivePulse(channel, &pulse, sizeof(pulse), NULL) == -1) {
            if (errno == EINTR)
                continue;
            perror("MsgReceivePulse");
            break;
        }

        switch (pulse.code) {
            case PULSE_CODE_TICK:
                publish_telemetry(&ctx);
                break;
            case _PULSE_CODE_COIDDEATH:
                if (hyp_shm_verify(ctx.hsp, ~0u) == 0) break;
            case PULSE_CODE_NOTIFY:
                handle_status(&ctx);
                break;
            default:
                fprintf(stderr, "unexpected pulse code %d\n", pulse.code);
                break;
        }
    }

    disarm_timer(&ctx);
    timer_delete(ctx.timer);
    hvshm_host_shutdown(&ctx.ch);
    hyp_shm_detach(ctx.hsp);
    printf("detached; send %" PRIu64 "samples, dropped %" PRIu64 "\n", ctx.seq, ctx.dropped);
    return EXIT_SUCCESS;
}
