#include "log.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>

static const char *const level_names[] = {
	[L_INFO] = "INFO",
	[L_WARN] = "WARN",
	[L_ERROR] = "ERROR",
};

void log_msg(enum log_level lvl, const char *tag, const char *fmt, ...)
{
	int saved = errno;
	va_list ap;

	/* stderr is our only log sink; if it fails there is nowhere to report it */
	fprintf(stderr, "%s %s: ", level_names[lvl], tag);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	errno = saved;
}
