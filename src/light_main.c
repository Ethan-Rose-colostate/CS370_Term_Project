/*
 * Light monitor: reads the digital output (DO) of a photoresistor module on
 * GPIO17 every interval and stores each reading in the light ring file.
 * Reading 1 = brighter than the module's threshold (set by its trimmer),
 * 0 = darker.
 *
 * It is also the parent process: at startup it forks and execs the moisture
 * monitor with the same interval, re-forks it with backoff if it dies, and on
 * shutdown it SIGTERMs and reaps it.
 *
 * Exit codes: 0 on SIGTERM/SIGINT, 1 on fatal error, 2 on bad usage.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/wait.h>
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
#ifndef HEARTBEAT_S			/* overridable for tests: -DHEARTBEAT_S=10 */
#define HEARTBEAT_S		3600
#endif
#define DEFAULT_RING		"data/light.ring"
#define DEFAULT_MOISTURE	"build/moisture"
/* Daytime read window [DAY_START_H, DAY_END_H), local time; overridable for tests. */
#ifndef DAY_START_H
#define DAY_START_H		8
#endif
#ifndef DAY_END_H
#define DAY_END_H		18
#endif
#define BACKOFF_MAX_S		60
#define BACKOFF_RESET_S		600	/* child uptime that resets the backoff */

enum { OPT_NO_RESTART = 256 };

struct stats {
	unsigned long readings;
	unsigned long faults;
	unsigned long store_errors;
};

/* Light decision state carried between readings. */
struct light_state {
	int paused;		/* outside the daytime window */
};

/* The moisture child and its restart policy. */
struct child_sup {
	const char *prog;
	long interval;
	const sigset_t *mask;
	int restart;		/* 0 with --no-restart */
	pid_t pid;		/* -1 while down */
	time_t started;		/* CLOCK_MONOTONIC seconds, -1 if unknown */
	long delay_s;		/* next backoff */
	unsigned long restarts;
};

static void usage(FILE *f, const char *prog)
{
	fprintf(f, "usage: %s [-i seconds] [-f ringfile] [-m moisture] [--no-restart]\n"
		"  -i  read interval, 1..%d seconds (default %d)\n"
		"  -f  ring file (default %s)\n"
		"  -m  moisture program to run as child (default %s)\n"
		"  --no-restart  leave the child dead if it exits\n",
		prog, DEFAULT_INTERVAL_S, DEFAULT_INTERVAL_S, DEFAULT_RING,
		DEFAULT_MOISTURE);
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

/* Local hour 0..23, or -1 if the clock can't be read. */
static int local_hour(void)
{
	time_t now = time(NULL);
	struct tm tm;

	if (now == (time_t)-1 || !localtime_r(&now, &tm)) {
		log_msg(L_WARN, "light", "local time unavailable (errno=%d)", errno);
		return -1;
	}
	return tm.tm_hour;
}

/*
 * True if a reading should be taken now. If the clock can't be read, read
 * anyway: a stray night reading beats a lost day of data.
 */
static int daytime(struct light_state *ls)
{
	int h = local_hour();
	int in = h < 0 || (h >= DAY_START_H && h < DAY_END_H);

	if (ls->paused == in) {
		ls->paused = !in;
		log_msg(L_INFO, "light", "%s %02d:00-%02d:00, readings %s",
			in ? "inside" : "outside", DAY_START_H, DAY_END_H,
			in ? "resumed" : "paused");
	}
	return in;
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

/*
 * Fork and exec the moisture monitor with the same interval. The caller has
 * SIGTERM/SIGINT/SIGCHLD blocked for its signalfd; the child unblocks them
 * before exec so the moisture program starts with a clean mask. Returns the
 * pid or -1.
 */
static pid_t start_child(const char *prog, long interval, const sigset_t *mask)
{
	char ival[24];
	char *args[4];
	pid_t pid;
	int n;

	n = snprintf(ival, sizeof ival, "%ld", interval);
	if (n < 0 || (size_t)n >= sizeof ival) {
		log_msg(L_ERROR, "parent", "interval format failed");
		return -1;
	}
	args[0] = (char *)prog;
	args[1] = "-i";
	args[2] = ival;
	args[3] = NULL;

	/* Don't let buffered stdio get written twice. */
	if (fflush(NULL) == EOF)
		log_msg(L_WARN, "parent", "fflush failed (errno=%d)", errno);

	pid = fork();
	if (pid < 0) {
		log_msg(L_ERROR, "parent", "fork failed (errno=%d)", errno);
		return -1;
	}
	if (pid == 0) {
		if (sigprocmask(SIG_UNBLOCK, mask, NULL) < 0) {
			log_msg(L_ERROR, "child", "sigprocmask unblock failed (errno=%d)",
				errno);
			_exit(1);
		}
		execv(prog, args);
		log_msg(L_ERROR, "child", "exec %s failed (errno=%d)", prog, errno);
		_exit(1);
	}
	log_msg(L_INFO, "parent", "started moisture child pid %ld", (long)pid);
	return pid;
}

/* SIGTERM the child and reap it so nothing outlives the parent. */
static void stop_child(pid_t pid)
{
	int status;
	pid_t r;

	if (pid <= 0)
		return;
	if (kill(pid, SIGTERM) < 0)
		log_msg(L_WARN, "parent", "kill child pid %ld failed (errno=%d)",
			(long)pid, errno);
	do
		r = waitpid(pid, &status, 0);
	while (r < 0 && errno == EINTR);
	if (r < 0) {
		log_msg(L_ERROR, "parent", "waitpid child pid %ld failed (errno=%d)",
			(long)pid, errno);
		return;
	}
	if (WIFEXITED(status))
		log_msg(L_INFO, "parent", "child pid %ld exited with status %d",
			(long)pid, WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		log_msg(L_INFO, "parent", "child pid %ld killed by signal %d",
			(long)pid, WTERMSIG(status));
}

static time_t mono_now(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
		log_msg(L_WARN, "parent", "clock_gettime failed (errno=%d)", errno);
		return -1;
	}
	return ts.tv_sec;
}

/* Start the child and note when, for the backoff reset. */
static void spawn_child(struct child_sup *cs)
{
	cs->pid = start_child(cs->prog, cs->interval, cs->mask);
	cs->started = cs->pid > 0 ? mono_now() : -1;
}

/* Arm the one-shot restart timer for the next backoff step. */
static int schedule_refork(struct child_sup *cs, int fd, unsigned long *n, long *secs)
{
	struct itimerspec its = { .it_value = { .tv_sec = cs->delay_s } };

	if (timerfd_settime(fd, 0, &its, NULL) < 0) {
		log_msg(L_ERROR, "parent", "timerfd_settime restart failed (errno=%d)",
			errno);
		return -1;
	}
	*n = ++cs->restarts;
	*secs = cs->delay_s;
	cs->delay_s = cs->delay_s * 2 > BACKOFF_MAX_S ? BACKOFF_MAX_S : cs->delay_s * 2;
	return 0;
}

/* Reap a dead child and schedule its re-fork. Returns -1 on fatal error. */
static int on_sigchld(struct child_sup *cs, int restart_fd)
{
	char why[32];
	unsigned long n;
	long secs;
	time_t now;
	int status;
	pid_t dead = cs->pid, r;

	if (dead <= 0)
		return 0;
	do
		r = waitpid(dead, &status, WNOHANG);
	while (r < 0 && errno == EINTR);
	if (r == 0)
		return 0;	/* stopped or continued, not dead */
	if (r < 0) {
		log_msg(L_ERROR, "parent", "waitpid child pid %ld failed (errno=%d)",
			(long)dead, errno);
		snprintf(why, sizeof why, "status unknown");
	} else if (WIFEXITED(status)) {
		snprintf(why, sizeof why, "exit %d", WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		snprintf(why, sizeof why, "signal %d", WTERMSIG(status));
	} else {
		snprintf(why, sizeof why, "status 0x%x", (unsigned)status);
	}
	cs->pid = -1;

	if (!cs->restart) {
		log_msg(L_ERROR, "parent", "child pid %ld died (%s), not restarting",
			(long)dead, why);
		return 0;
	}
	now = mono_now();
	if (cs->started >= 0 && now >= 0 && now - cs->started >= BACKOFF_RESET_S)
		cs->delay_s = 1;
	if (schedule_refork(cs, restart_fd, &n, &secs) < 0)
		return -1;
	log_msg(L_ERROR, "parent", "child pid %ld died (%s), re-fork #%lu in %ld s",
		(long)dead, why, n, secs);
	return 0;
}

/* Restart timer fired: re-fork, or back off again if the fork fails. */
static int on_restart_timer(struct child_sup *cs, int restart_fd)
{
	unsigned long n;
	long secs;

	spawn_child(cs);
	if (cs->pid > 0)
		return 0;
	if (schedule_refork(cs, restart_fd, &n, &secs) < 0)
		return -1;
	log_msg(L_ERROR, "parent", "re-fork failed, re-fork #%lu in %ld s", n, secs);
	return 0;
}

/* VmRSS of pid from /proc/<pid>/status. Returns -1 if unavailable (zombie). */
static int read_rss_kb(pid_t pid, unsigned long *kb)
{
	char path[64], line[128];
	int found = -1;
	FILE *f;
	int n;

	n = snprintf(path, sizeof path, "/proc/%ld/status", (long)pid);
	if (n < 0 || (size_t)n >= sizeof path) {
		log_msg(L_WARN, "parent", "status path format failed");
		return -1;
	}
	f = fopen(path, "r");
	if (!f) {
		log_msg(L_WARN, "parent", "open %s failed (errno=%d)", path, errno);
		return -1;
	}
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "VmRSS: %lu kB", kb) == 1) {
			found = 0;
			break;
		}
	}
	if (found < 0 && ferror(f))
		log_msg(L_WARN, "parent", "read %s failed (errno=%d)", path, errno);
	if (fclose(f) == EOF)
		log_msg(L_WARN, "parent", "close %s failed (errno=%d)", path, errno);
	return found;
}

static void heartbeat(const struct child_sup *cs, const struct stats *st)
{
	char prss[24] = "n/a", crss[24] = "n/a", alive[32] = "down";
	unsigned long kb;

	if (read_rss_kb(getpid(), &kb) == 0)
		snprintf(prss, sizeof prss, "%lu", kb);
	if (cs->pid > 0) {
		snprintf(alive, sizeof alive, "pid %ld alive", (long)cs->pid);
		if (read_rss_kb(cs->pid, &kb) == 0)
			snprintf(crss, sizeof crss, "%lu", kb);
	}
	log_msg(L_INFO, "parent",
		"heartbeat: child %s, rss parent=%s kB child=%s kB, "
		"light readings=%lu faults=%lu store_errors=%lu, restarts=%lu",
		alive, prss, crss, st->readings, st->faults, st->store_errors,
		cs->restarts);
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "no-restart", no_argument, NULL, OPT_NO_RESTART },
		{ NULL, 0, NULL, 0 },
	};
	const char *path = DEFAULT_RING;
	long interval = DEFAULT_INTERVAL_S;
	struct ring *ring = NULL;
	struct stats st = { 0 };
	struct light_state ls = { 0 };
	struct child_sup cs = {
		.prog = DEFAULT_MOISTURE, .restart = 1, .pid = -1,
		.started = -1, .delay_s = 1,
	};
	int bus = -1, sig_fd = -1, tick_fd = -1, hb_fd = -1, restart_fd = -1;
	int ret = 1;
	sigset_t mask;
	int opt;

	while ((opt = getopt_long(argc, argv, "i:f:m:h", longopts, NULL)) != -1) {
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
		case 'm':
			cs.prog = optarg;
			break;
		case OPT_NO_RESTART:
			cs.restart = 0;
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

	/*
	 * Take SIGTERM/SIGINT/SIGCHLD through a signalfd so shutdown and child
	 * death can't race poll(). They must be blocked before the first fork.
	 */
	if (sigemptyset(&mask) < 0 || sigaddset(&mask, SIGTERM) < 0 ||
	    sigaddset(&mask, SIGINT) < 0 || sigaddset(&mask, SIGCHLD) < 0) {
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

	/* Fork before opening the ring, timers and bus so the child inherits none. */
	cs.interval = interval;
	cs.mask = &mask;
	spawn_child(&cs);
	if (cs.pid < 0)
		goto out;

	if (ring_open(path, &ring) < 0)
		goto out;
	restart_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	if (restart_fd < 0) {
		log_msg(L_ERROR, "parent", "timerfd_create restart failed (errno=%d)",
			errno);
		goto out;
	}
	tick_fd = make_timer(interval, "read");
	if (tick_fd < 0)
		goto out;
	hb_fd = make_timer(HEARTBEAT_S, "heartbeat");
	if (hb_fd < 0)
		goto out;

	log_msg(L_INFO, "light", "heartbeat: started, interval %lds, ring %s",
		interval, path);
	if (daytime(&ls))
		take_reading(ring, &bus, &st);

	for (;;) {
		struct pollfd pfd[4] = {
			{ .fd = sig_fd, .events = POLLIN },
			{ .fd = tick_fd, .events = POLLIN },
			{ .fd = hb_fd, .events = POLLIN },
			{ .fd = restart_fd, .events = POLLIN },
		};
		uint64_t exp;

		if (poll(pfd, 4, -1) < 0) {
			if (errno == EINTR)
				continue;
			log_msg(L_ERROR, "light", "poll failed (errno=%d)", errno);
			goto out;
		}
		for (int i = 0; i < 4; i++) {
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
			if (si.ssi_signo == SIGCHLD) {
				if (on_sigchld(&cs, restart_fd) < 0)
					goto out;
			} else {
				log_msg(L_INFO, "light", "signal %u, exiting",
					(unsigned)si.ssi_signo);
				ret = 0;
				goto out;
			}
		}
		if (pfd[1].revents & POLLIN) {
			if (read_timer(tick_fd, "read", &exp) < 0)
				goto out;
			if (exp > 1)
				log_msg(L_WARN, "light", "missed %llu read ticks",
					(unsigned long long)(exp - 1));
			if (daytime(&ls))
				take_reading(ring, &bus, &st);
		}
		if (pfd[2].revents & POLLIN) {
			if (read_timer(hb_fd, "heartbeat", &exp) < 0)
				goto out;
			heartbeat(&cs, &st);
		}
		if (pfd[3].revents & POLLIN) {
			if (read_timer(restart_fd, "restart", &exp) < 0)
				goto out;
			if (on_restart_timer(&cs, restart_fd) < 0)
				goto out;
		}
	}

out:
	stop_child(cs.pid);
	close_fd(&restart_fd, "restart timer");
	close_fd(&hb_fd, "heartbeat timer");
	close_fd(&tick_fd, "read timer");
	close_fd(&bus, GPIO_CHIP);
	close_fd(&sig_fd, "signalfd");
	if (ring_close(ring) < 0)
		ret = 1;
	log_msg(L_INFO, "light", "exit %d", ret);
	return ret;
}