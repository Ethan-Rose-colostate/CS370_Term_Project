#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define LOG_LINE_MAX	512

static const char *const level_names[] = {
	[L_INFO] = "INFO",
	[L_WARN] = "WARN",
	[L_ERROR] = "ERROR",
};

/* Local time to the minute, or a placeholder if the clock can't be read. */
static void format_stamp(char *buf, size_t len)
{
	time_t now = time(NULL);
	struct tm tm;

	if (now == (time_t)-1 || !localtime_r(&now, &tm) ||
	    strftime(buf, len, "%Y-%m-%d %H:%M", &tm) == 0)
		snprintf(buf, len, "----------------");
}

void log_msg(enum log_level lvl, const char *tag, const char *fmt, ...)
{
	int saved = errno;
	char stamp[32];
	char line[LOG_LINE_MAX];
	size_t len, off = 0;
	va_list ap;
	int n;

	format_stamp(stamp, sizeof stamp);
	n = snprintf(line, sizeof line, "%s %s %s: ", stamp, level_names[lvl], tag);
	if (n < 0)
		goto out;
	len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
	va_start(ap, fmt);
	n = vsnprintf(line + len, sizeof line - len, fmt, ap);
	va_end(ap);
	if (n > 0)
		len += (size_t)n < sizeof line - len ? (size_t)n : sizeof line - len - 1;
	/* Truncated lines still end in a newline. */
	if (len > sizeof line - 2)
		len = sizeof line - 2;
	line[len++] = '\n';

	/*
	 * One write per line so lines from the parent and child don't interleave.
	 * stderr is our only log sink; if it fails there is nowhere to report it.
	 */
	while (off < len) {
		ssize_t w = write(STDERR_FILENO, line + off, len - off);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		off += (size_t)w;
	}
out:
	errno = saved;
}
