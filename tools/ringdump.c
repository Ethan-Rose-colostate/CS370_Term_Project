/* ringdump: print every valid record in a ring file, oldest first. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "log.h"
#include "store/ring.h"

static int by_seq(const void *a, const void *b)
{
	const struct ring_record *x = a, *y = b;

	return (x->seq > y->seq) - (x->seq < y->seq);
}

int main(int argc, char **argv)
{
	struct ring_record *recs = NULL;
	size_t n_valid = 0, n_bad = 0;
	int ret = 1;

	if (argc != 2) {
		fprintf(stderr, "usage: %s RINGFILE\n", argv[0]);
		return 2;
	}
	recs = calloc(RING_SLOTS, sizeof *recs);
	if (!recs) {
		log_msg(L_ERROR, "ringdump", "out of memory");
		goto out;
	}
	if (ring_read_all(argv[1], recs, &n_valid, &n_bad) < 0)
		goto out;
	qsort(recs, n_valid, sizeof *recs, by_seq);

	printf("%10s  %-19s  %7s  %s\n", "seq", "time", "reading", "fault");
	for (size_t i = 0; i < n_valid; i++) {
		time_t t = (time_t)recs[i].ts;
		struct tm tm;
		char when[32];

		if (!localtime_r(&t, &tm) ||
		    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm) == 0)
			snprintf(when, sizeof when, "@%" PRIu32, recs[i].ts);
		printf("%10" PRIu32 "  %-19s  %7u  %u\n", recs[i].seq, when,
		       (unsigned)recs[i].reading, (unsigned)recs[i].fault);
	}
	if (fflush(stdout) == EOF || ferror(stdout)) {
		log_msg(L_ERROR, "ringdump", "write to stdout failed (errno=%d)", errno);
		goto out;
	}
	fprintf(stderr, "ringdump: %zu valid, %zu bad slots\n", n_valid, n_bad);
	ret = 0;
out:
	free(recs);
	return ret;
}
