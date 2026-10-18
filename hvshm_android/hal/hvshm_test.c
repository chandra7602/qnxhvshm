#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "hvshm_msgs.h"
#include "hvshm_uapi.h"

#define DEV_PATH       "/dev/hvshm"
#define WAIT_MS        3000

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) {
	(void)sig;
	g_stop = 1;
}

static void install_signals(void) {
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;          /* no SA_RESTART: ioctl must return EINTR */
	sigemptyset(&sa.sa_mask);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

static void print_info(int fd) {
	struct hvshm_info info;

	if (ioctl(fd, HVSHM_IOC_GET_INFO, &info) < 0) {
		perror("HVSHM_IOC_GET_INFO");
		return;
	}
	printf("[info] my idx=%u attach_mask=0x%x host_idx=%d host_online=%u "
	       "region=%" PRIu64 " B ring=%u B\n",
	       info.conn_idx, info.attach_mask,
	       info.host_idx == HVSHM_NO_CONN ? -1 : (int)info.host_idx,
	       info.host_online, (uint64_t)info.region_bytes, info.ring_bytes);
}

static int send_set_period(int fd, uint32_t period_ms) {
	struct hvshm_send_arg a;
	struct hvshm_set_period req = { .period_ms = period_ms };

	memset(&a, 0, sizeof(a));
	a.msg_type = HVSHM_MSG_SET_PERIOD;
	a.len = sizeof(req);
	memcpy(a.payload, &req, sizeof(req));

	if (ioctl(fd, HVSHM_IOC_SEND, &a) < 0) {
		fprintf(stderr, "SEND SET_PERIOD failed: %s\n", strerror(errno));
		return -1;
	}
	printf("[tx] SET_PERIOD %u ms\n", period_ms);
	return 0;
}

int main(int argc, char **argv) {
	const uint32_t want_period = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 0) : 0;
	int period_pending = want_period != 0;
	int have_seq = 0;
	uint64_t last_seq = 0;
	int fd;

	install_signals();

	fd = open(DEV_PATH, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open " DEV_PATH);
		return EXIT_FAILURE;
	}
	print_info(fd);

	while (!g_stop) {
		struct hvshm_wait_arg wa;

		memset(&wa, 0, sizeof(wa));
		wa.timeout_ms = WAIT_MS;

		if (ioctl(fd, HVSHM_IOC_WAIT_EVENT, &wa) < 0) {
			if (errno == ETIMEDOUT) {
				printf("(no event for %d ms)\n", WAIT_MS);
				continue;
			}
			if (errno == EINTR) continue;
			if (errno == EPROTO) {
				fprintf(stderr, "host corrupted the ring, data dropped\n");
				have_seq = 0;
				continue;
			}
			perror("HVSHM_IOC_WAIT_EVENT");
			break;
		}

		switch (wa.ev.type) {
		case HVSHM_EV_PEER_CONNECTED:
			printf("[event] client %u CONNECTED\n", wa.ev.conn_idx);
			print_info(fd);
			break;

		case HVSHM_EV_PEER_DISCONNECTED:
			printf("[event] client %u DISCONNECTED\n", wa.ev.conn_idx);
			have_seq = 0;
			period_pending = want_period != 0;
			print_info(fd);
			break;

		case HVSHM_EV_MESSAGE:
			if (wa.ev.msg_type == HVSHM_MSG_TELEMETRY &&
			    wa.ev.len == sizeof(struct hvshm_telemetry)) {
				struct hvshm_telemetry t;

				memcpy(&t, wa.ev.payload, sizeof(t));
				if (have_seq && t.seq != last_seq + 1)
					printf("  !! sequence gap: %" PRIu64 " -> %" PRIu64 "\n",
					       last_seq, t.seq);
				have_seq = 1;
				last_seq = t.seq;
				printf("[telemetry] seq=%" PRIu64 " host_t=%" PRIu64 " ms "
				       "v=[%d %d %d %d]\n",
				       t.seq, t.host_time_ns / 1000000u,
				       t.value[0], t.value[1], t.value[2], t.value[3]);

				if (period_pending && send_set_period(fd, want_period) == 0)
					period_pending = 0;
			} else if (wa.ev.msg_type == HVSHM_MSG_PERIOD_ACK &&
				   wa.ev.len == sizeof(struct hvshm_period_ack)) {
				struct hvshm_period_ack ack;

				memcpy(&ack, wa.ev.payload, sizeof(ack));
				printf("[rx] PERIOD_ACK: host now uses %u ms\n", ack.period_ms);
			} else {
				printf("[rx] unknown message type=%u len=%u\n",
				       wa.ev.msg_type, wa.ev.len);
			}
			break;

		default:
			printf("[event] unknown type %u\n", wa.ev.type);
			break;
		}
		fflush(stdout);
	}

	close(fd);      /* detaches from the region */
	return EXIT_SUCCESS;
}