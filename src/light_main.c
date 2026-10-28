/*
 * Light monitor: reads lux from a BH1750 on /dev/i2c-1 every interval and
 * stores each reading in the light ring file.
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
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <linux/i2c-dev.h>

#include "log.h"
#include "store/ring.h"

#define I2C_DEV			"/dev/i2c-1"
#define BH1750_ADDR		0x23
#define BH1750_POWER_ON		0x01
#define BH1750_ONE_TIME_HRES	0x20	/* measure once, then power down */
#define BH1750_MEAS_MS		180	/* max high-res measurement time */
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

static int bus_open(void)
{
	int fd = open(I2C_DEV, O_RDWR | O_CLOEXEC);
	int err;

	if (fd < 0) {
		log_msg(L_WARN, "light", "open %s failed (errno=%d)", I2C_DEV, errno);
		return -1;
	}
	if (ioctl(fd, I2C_SLAVE, BH1750_ADDR) < 0) {
		err = errno;
		log_msg(L_WARN, "light", "ioctl I2C_SLAVE 0x%02x failed (errno=%d)",
			BH1750_ADDR, err);
		close_fd(&fd, I2C_DEV);
		errno = err;
		return -1;
	}
	return fd;
}

static int bh1750_cmd(int fd, uint8_t cmd)
{
	ssize_t n = write(fd, &cmd, 1);

	if (n == 1)
		return 0;
	if (n >= 0)
		errno = EIO;
	log_msg(L_WARN, "light", "i2c write 0x%02x failed (errno=%d)", cmd, errno);
	return -1;
}

static int bh1750_measure(int fd, uint16_t *lux)
{
	uint8_t buf[2];
	uint32_t raw;
	ssize_t n;

	if (bh1750_cmd(fd, BH1750_POWER_ON) < 0 ||
	    bh1750_cmd(fd, BH1750_ONE_TIME_HRES) < 0)
		return -1;
	sleep_ms(BH1750_MEAS_MS);
	n = read(fd, buf, sizeof buf);
	if (n != (ssize_t)sizeof buf) {
		if (n >= 0)
			errno = EIO;
		log_msg(L_WARN, "light", "i2c read failed (errno=%d)", errno);
		return -1;
	}
	raw = (uint32_t)buf[0] << 8 | buf[1];
	*lux = (uint16_t)((raw * 5 + 3) / 6);	/* lux = raw / 1.2, rounded */
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
		if (*bus >= 0 && bh1750_measure(*bus, lux) == 0)
			return 0;
		err = errno;
		close_fd(bus, I2C_DEV);
	}
	log_msg(L_WARN, "light", "i2c read failed x%d (errno=%d), skipped",
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
		log_msg(L_WARN, "light", "reading not stored (lux=%u fault=%u)",
			(unsigned)lux, (unsigned)fault);
		return;
	}
	log_msg(L_INFO, "light", "lux=%u fault=%u", (unsigned)lux, (unsigned)fault);
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
	close_fd(&bus, I2C_DEV);
	close_fd(&sig_fd, "signalfd");
	if (ring_close(ring) < 0)
		ret = 1;
	log_msg(L_INFO, "light", "exit %d", ret);
	return ret;
}
