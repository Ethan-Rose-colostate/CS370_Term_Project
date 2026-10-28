/*
 * Light monitor: reads the digital output (DO) of a photoresistor module on
 * GPIO17 every interval and stores each reading in the light ring file.
 * Reading 1 = brighter than the module's threshold (set by its trimmer),
 * 0 = darker.
 *
 * Exit codes: 0 on SIGTERM/SIGINT, 1 on fatal error, 2 on bad usage.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <linux/gpio.h>

#include "log.h"
#include "store/ring.h"

#define GPIO_CHIP		"/dev/gpiochip0"
#define LIGHT_GPIO		17	/* BCM numbering: header pin 11 */
#define READ_ATTEMPTS		3
#define RETRY_MS		100
#define DEFAULT_INTERVAL_S	1200
#define HEARTBEAT_S		3600
#define DEFAULT_RING		"data/light.ring"

struct stats {
	unsigned long readings;
	unsigned long faults;
	unsigned long store_errors;
};

static void usage(FILE *f, const char *prog)
{
	fprintf(f, "usage: %s [-i seconds] [-f ringfile]\n"
		"  -i  read interval, 1..%d seconds (default %d)\n"
		"  -f  ring file (default %s)\n",
		prog, DEFAULT_INTERVAL_S, DEFAULT_INTERVAL_S, DEFAULT_RING);
}

static int parse_interval(const char *s, long *out)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || end == s || *end != '\0' || v < 1 || v > DEFAULT_INTERVAL_S)
		return -1;
	*out = v;
	return 0;
}

static void close_fd(int *fd, const char *what)
{
	if (*fd >= 0 && close(*fd) < 0)
		log_msg(L_WARN, "light", "close %s failed (errno=%d)", what, errno);
	*fd = -1;
}

static void sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };

	while (nanosleep(&ts, &ts) < 0) {
		if (errno != EINTR) {
			log_msg(L_WARN, "light", "nanosleep failed (errno=%d)", errno);
			return;
		}
	}
}

static uint32_t now_unix(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
		log_msg(L_ERROR, "light", "clock_gettime failed (errno=%d)", errno);
		return 0;
	}
	if (ts.tv_sec < 0 || (uint64_t)ts.tv_sec > UINT32_MAX) {
		log_msg(L_WARN, "light", "clock out of range (%lld)",
			(long long)ts.tv_sec);
		return 0;
	}
	return (uint32_t)ts.tv_sec;
}

/* Request GPIO17 as an input; returns the line fd or -1. */
static int bus_open(void)
{
	struct gpio_v2_line_request req;
	int chip = open(GPIO_CHIP, O_RDONLY | O_CLOEXEC);
	int err;

	if (chip < 0) {
		log_msg(L_WARN, "light", "open %s failed (errno=%d)", GPIO_CHIP, errno);
		return -1;
	}
	memset(&req, 0, sizeof req);
	req.offsets[0] = LIGHT_GPIO;
	req.num_lines = 1;
	req.config.flags = GPIO_V2_LINE_FLAG_INPUT;
	strncpy(req.consumer, "light_monitor", sizeof req.consumer - 1);
	if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req) < 0) {
		err = errno;
		log_msg(L_WARN, "light", "request GPIO%d failed (errno=%d)",
			LIGHT_GPIO, err);
		close_fd(&chip, GPIO_CHIP);
		errno = err;
		return -1;
	}
	close_fd(&chip, GPIO_CHIP);
	return req.fd;
}

/* The module pulls DO low when it is brighter than its threshold. */
static int light_measure(int fd, uint16_t *bright)
{
	struct gpio_v2_line_values vals = { .mask = 1 };

	if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &vals) < 0) {
		log_msg(L_WARN, "light", "gpio read failed (errno=%d)", errno);
		return -1;
	}
	*bright = (vals.bits & 1) ? 0 : 1;
	return 0;
}

/* Try up to READ_ATTEMPTS times, reopening the bus after any failure. */
static int read_light(int *bus, uint16_t *lux)
{
	int err = 0;

	for (int attempt = 1; attempt <= READ_ATTEMPTS; attempt++) {
		if (attempt > 1)
			sleep_ms(RETRY_MS);
		if (*bus < 0)
			*bus = bus_open();
		if (*bus >= 0 && light_measure(*bus, lux) == 0)
			return 0;
		err = errno;
		close_fd(bus, GPIO_CHIP);
	}
	log_msg(L_WARN, "light", "gpio read failed x%d (errno=%d), skipped",
		READ_ATTEMPTS, err);
	return -1;
}

static void take_reading(struct ring *ring, int *bus, struct stats *st)
{
	uint32_t ts = now_unix();
	uint16_t lux = 0;
	uint8_t fault = 0;

	if (read_light(bus, &lux) < 0) {
		lux = 0;
		fault = 1;
		st->faults++;
	}
	st->readings++;
	if (ring_append(ring, ts, lux, fault) < 0) {
		st->store_errors++;
		log_msg(L_WARN, "light", "reading not stored (bright=%u fault=%u)",
			(unsigned)lux, (unsigned)fault);
		return;
	}
	log_msg(L_INFO, "light", "bright=%u fault=%u", (unsigned)lux, (unsigned)fault);
}

static int make_timer(long secs, const char *what)
{
	struct itimerspec its = {
		.it_interval = { .tv_sec = secs },
		.it_value = { .tv_sec = secs },
	};
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

	if (fd < 0) {
		log_msg(L_ERROR, "light", "timerfd_create %s failed (errno=%d)",
			what, errno);
		return -1;
	}
	if (timerfd_settime(fd, 0, &its, NULL) < 0) {
		log_msg(L_ERROR, "light", "timerfd_settime %s failed (errno=%d)",
			what, errno);
		close_fd(&fd, what);
		return -1;
	}
	return fd;
}

static int read_timer(int fd, const char *what, uint64_t *expirations)
{
	ssize_t n;

	do
		n = read(fd, expirations, sizeof *expirations);
	while (n < 0 && errno == EINTR);
	if (n != (ssize_t)sizeof *expirations) {
		log_msg(L_ERROR, "light", "read %s timer failed (n=%zd errno=%d)",
			what, n, n < 0 ? errno : 0);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *path = DEFAULT_RING;
	long interval = DEFAULT_INTERVAL_S;
	struct ring *ring = NULL;
	struct stats st = { 0 };
	int bus = -1, sig_fd = -1, tick_fd = -1, hb_fd = -1;
	int ret = 1;
	sigset_t mask;
	int opt;

	while ((opt = getopt(argc, argv, "i:f:h")) != -1) {
		switch (opt) {
		case 'i':
			if (parse_interval(optarg, &interval) < 0) {
				fprintf(stderr, "%s: bad interval '%s'\n", argv[0], optarg);
				usage(stderr, argv[0]);
				return 2;
			}
			break;
		case 'f':
			path = optarg;
			break;
		case 'h':
			usage(stdout, argv[0]);
			return 0;
		default:
			usage(stderr, argv[0]);
			return 2;
		}
	}
	if (optind != argc) {
		usage(stderr, argv[0]);
		return 2;
	}

	/* Take SIGTERM/SIGINT through a signalfd so shutdown can't race poll(). */
	if (sigemptyset(&mask) < 0 || sigaddset(&mask, SIGTERM) < 0 ||
	    sigaddset(&mask, SIGINT) < 0) {
		log_msg(L_ERROR, "light", "sigset setup failed (errno=%d)", errno);
		goto out;
	}
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		log_msg(L_ERROR, "light", "sigprocmask failed (errno=%d)", errno);
		goto out;
	}
	sig_fd = signalfd(-1, &mask, SFD_CLOEXEC);
	if (sig_fd < 0) {
		log_msg(L_ERROR, "light", "signalfd failed (errno=%d)", errno);
		goto out;
	}

	if (ring_open(path, &ring) < 0)
		goto out;
	tick_fd = make_timer(interval, "read");
	if (tick_fd < 0)
		goto out;
	hb_fd = make_timer(HEARTBEAT_S, "heartbeat");
	if (hb_fd < 0)
		goto out;

	log_msg(L_INFO, "light", "heartbeat: started, interval %lds, ring %s",
		interval, path);
	take_reading(ring, &bus, &st);

	for (;;) {
		struct pollfd pfd[3] = {
			{ .fd = sig_fd, .events = POLLIN },
			{ .fd = tick_fd, .events = POLLIN },
			{ .fd = hb_fd, .events = POLLIN },
		};
		uint64_t exp;

		if (poll(pfd, 3, -1) < 0) {
			if (errno == EINTR)
				continue;
			log_msg(L_ERROR, "light", "poll failed (errno=%d)", errno);
			goto out;
		}
		for (int i = 0; i < 3; i++) {
			if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				log_msg(L_ERROR, "light", "poll fd %d revents 0x%x",
					pfd[i].fd, (unsigned)pfd[i].revents);
				goto out;
			}
		}
		if (pfd[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			ssize_t n = read(sig_fd, &si, sizeof si);

			if (n != (ssize_t)sizeof si) {
				log_msg(L_ERROR, "light", "signalfd read failed (n=%zd errno=%d)",
					n, n < 0 ? errno : 0);
				goto out;
			}
			log_msg(L_INFO, "light", "signal %u, exiting",
				(unsigned)si.ssi_signo);
			ret = 0;
			goto out;
		}
		if (pfd[1].revents & POLLIN) {
			if (read_timer(tick_fd, "read", &exp) < 0)
				goto out;
			if (exp > 1)
				log_msg(L_WARN, "light", "missed %llu read ticks",
					(unsigned long long)(exp - 1));
			take_reading(ring, &bus, &st);
		}
		if (pfd[2].revents & POLLIN) {
			if (read_timer(hb_fd, "heartbeat", &exp) < 0)
				goto out;
			log_msg(L_INFO, "light",
				"heartbeat: readings=%lu faults=%lu store_errors=%lu",
				st.readings, st.faults, st.store_errors);
		}
	}

out:
	close_fd(&hb_fd, "heartbeat timer");
	close_fd(&tick_fd, "read timer");
	close_fd(&bus, GPIO_CHIP);
	close_fd(&sig_fd, "signalfd");
	if (ring_close(ring) < 0)
		ret = 1;
	log_msg(L_INFO, "light", "exit %d", ret);
	return ret;
}