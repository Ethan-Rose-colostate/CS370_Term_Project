/*
 * Moisture monitor: reads the capacitive soil probe through an MCP3008 ADC
 * (channel 0) on /dev/spidev0.0 every interval and stores each raw 10-bit
 * reading in the moisture ring file.
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

#include <linux/spi/spidev.h>

#include "log.h"
#include "store/ring.h"

#define SPI_DEV			"/dev/spidev0.0"
#define SPI_SPEED_HZ		1000000u	/* MCP3008 max is 1.35 MHz at 3.3 V */
#define SPI_BITS		8
#define MCP3008_CHANNEL		0
#define RAIL_LOW_MAX		5
#define RAIL_HIGH_MIN		1018
#define RAIL_COUNT		3	/* consecutive at-rail reads = sensor fault */
#define DEFAULT_INTERVAL_S	1200
#define HEARTBEAT_S		3600
#define DEFAULT_RING		"data/moisture.ring"

struct stats {
	unsigned long readings;
	unsigned long faults;
	unsigned long store_errors;
	unsigned rail_run;	/* consecutive at-rail reads, capped at RAIL_COUNT */
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
		log_msg(L_WARN, "moisture", "close %s failed (errno=%d)", what, errno);
	*fd = -1;
}

static uint32_t now_unix(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
		log_msg(L_ERROR, "moisture", "clock_gettime failed (errno=%d)", errno);
		return 0;
	}
	if (ts.tv_sec < 0 || (uint64_t)ts.tv_sec > UINT32_MAX) {
		log_msg(L_WARN, "moisture", "clock out of range (%lld)",
			(long long)ts.tv_sec);
		return 0;
	}
	return (uint32_t)ts.tv_sec;
}

static int spi_open(void)
{
	uint8_t mode = SPI_MODE_0;
	uint8_t bits = SPI_BITS;
	uint32_t speed = SPI_SPEED_HZ;
	const char *step;
	int fd, err;

	fd = open(SPI_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		log_msg(L_WARN, "moisture", "spi open %s failed (errno=%d)",
			SPI_DEV, errno);
		return -1;
	}
	step = "set mode";
	if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0)
		goto fail;
	step = "set bits per word";
	if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0)
		goto fail;
	step = "set max speed";
	if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0)
		goto fail;
	return fd;
fail:
	err = errno;
	log_msg(L_WARN, "moisture", "spi %s failed (errno=%d)", step, err);
	close_fd(&fd, SPI_DEV);
	errno = err;
	return -1;
}

static int mcp3008_read(int fd, uint16_t *raw)
{
	/* start bit, then single-ended + channel, then a byte to clock out the result */
	uint8_t tx[3] = { 0x01, 0x80 | (MCP3008_CHANNEL << 4), 0x00 };
	uint8_t rx[3] = { 0 };
	struct spi_ioc_transfer xfer = {
		.tx_buf = (uintptr_t)tx,
		.rx_buf = (uintptr_t)rx,
		.len = sizeof tx,
		.speed_hz = SPI_SPEED_HZ,
		.bits_per_word = SPI_BITS,
	};
	int n = ioctl(fd, SPI_IOC_MESSAGE(1), &xfer);

	if (n < (int)sizeof tx) {
		if (n >= 0)
			errno = EIO;
		log_msg(L_WARN, "moisture", "spi transfer failed (errno=%d)", errno);
		return -1;
	}
	*raw = (uint16_t)((rx[1] & 0x03) << 8 | rx[2]);
	return 0;
}

/* One attempt; on failure close the device so the next cycle reopens it. */
static int read_moisture(int *dev, uint16_t *raw)
{
	if (*dev < 0)
		*dev = spi_open();
	if (*dev >= 0 && mcp3008_read(*dev, raw) == 0)
		return 0;
	close_fd(dev, SPI_DEV);
	return -1;
}

static int at_rail(uint16_t raw)
{
	return raw <= RAIL_LOW_MAX || raw >= RAIL_HIGH_MIN;
}

static void take_reading(struct ring *ring, int *dev, struct stats *st)
{
	uint32_t ts = now_unix();
	uint16_t raw = 0;
	uint8_t fault = 0;

	if (read_moisture(dev, &raw) < 0) {
		raw = 0;
		fault = 1;
		st->rail_run = 0;
	} else if (at_rail(raw)) {
		if (st->rail_run < RAIL_COUNT)
			st->rail_run++;
		if (st->rail_run >= RAIL_COUNT) {
			fault = 1;
			log_msg(L_WARN, "moisture", "raw=%u at rail x%d, sensor fault",
				(unsigned)raw, RAIL_COUNT);
		}
	} else {
		st->rail_run = 0;
	}
	if (fault)
		st->faults++;
	st->readings++;
	if (ring_append(ring, ts, raw, fault) < 0) {
		st->store_errors++;
		log_msg(L_WARN, "moisture", "reading not stored (raw=%u fault=%u)",
			(unsigned)raw, (unsigned)fault);
		return;
	}
	log_msg(L_INFO, "moisture", "raw=%u fault=%u", (unsigned)raw, (unsigned)fault);
}

static int make_timer(long secs, const char *what)
{
	struct itimerspec its = {
		.it_interval = { .tv_sec = secs },
		.it_value = { .tv_sec = secs },
	};
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

	if (fd < 0) {
		log_msg(L_ERROR, "moisture", "timerfd_create %s failed (errno=%d)",
			what, errno);
		return -1;
	}
	if (timerfd_settime(fd, 0, &its, NULL) < 0) {
		log_msg(L_ERROR, "moisture", "timerfd_settime %s failed (errno=%d)",
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
		log_msg(L_ERROR, "moisture", "read %s timer failed (n=%zd errno=%d)",
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
	int dev = -1, sig_fd = -1, tick_fd = -1, hb_fd = -1;
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
		log_msg(L_ERROR, "moisture", "sigset setup failed (errno=%d)", errno);
		goto out;
	}
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		log_msg(L_ERROR, "moisture", "sigprocmask failed (errno=%d)", errno);
		goto out;
	}
	sig_fd = signalfd(-1, &mask, SFD_CLOEXEC);
	if (sig_fd < 0) {
		log_msg(L_ERROR, "moisture", "signalfd failed (errno=%d)", errno);
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

	log_msg(L_INFO, "moisture", "heartbeat: started, interval %lds, ring %s",
		interval, path);
	take_reading(ring, &dev, &st);

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
			log_msg(L_ERROR, "moisture", "poll failed (errno=%d)", errno);
			goto out;
		}
		for (int i = 0; i < 3; i++) {
			if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				log_msg(L_ERROR, "moisture", "poll fd %d revents 0x%x",
					pfd[i].fd, (unsigned)pfd[i].revents);
				goto out;
			}
		}
		if (pfd[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			ssize_t n = read(sig_fd, &si, sizeof si);

			if (n != (ssize_t)sizeof si) {
				log_msg(L_ERROR, "moisture", "signalfd read failed (n=%zd errno=%d)",
					n, n < 0 ? errno : 0);
				goto out;
			}
			log_msg(L_INFO, "moisture", "signal %u, exiting",
				(unsigned)si.ssi_signo);
			ret = 0;
			goto out;
		}
		if (pfd[1].revents & POLLIN) {
			if (read_timer(tick_fd, "read", &exp) < 0)
				goto out;
			if (exp > 1)
				log_msg(L_WARN, "moisture", "missed %llu read ticks",
					(unsigned long long)(exp - 1));
			take_reading(ring, &dev, &st);
		}
		if (pfd[2].revents & POLLIN) {
			if (read_timer(hb_fd, "heartbeat", &exp) < 0)
				goto out;
			log_msg(L_INFO, "moisture",
				"heartbeat: readings=%lu faults=%lu store_errors=%lu",
				st.readings, st.faults, st.store_errors);
		}
	}

out:
	close_fd(&hb_fd, "heartbeat timer");
	close_fd(&tick_fd, "read timer");
	close_fd(&dev, SPI_DEV);
	close_fd(&sig_fd, "signalfd");
	if (ring_close(ring) < 0)
		ret = 1;
	log_msg(L_INFO, "moisture", "exit %d", ret);
	return ret;
}
