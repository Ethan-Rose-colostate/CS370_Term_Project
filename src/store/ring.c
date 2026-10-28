#define _POSIX_C_SOURCE 200809L

#include "store/ring.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"
#include "store/crc32.h"

#define RING_FILE_SIZE ((off_t)RING_SLOTS * RING_REC_SIZE)
#define CRC_SPAN 12u

struct ring {
	int fd;
	char *name;		/* basename, for log lines */
	uint32_t next_slot;
	uint32_t next_seq;
};

enum slot_state { SLOT_FREE, SLOT_VALID, SLOT_BAD };

struct scan {
	size_t n_valid;
	size_t n_bad;
	int found;
	uint32_t max_seq;
	uint32_t max_slot;
};

static void put_le32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static uint32_t get_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
	       (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void encode(const struct ring_record *rec, unsigned char *out)
{
	put_le32(out, rec->seq);
	put_le32(out + 4, rec->ts);
	out[8] = (unsigned char)rec->reading;
	out[9] = (unsigned char)(rec->reading >> 8);
	out[10] = rec->fault;
	out[11] = 0;
	put_le32(out + 12, crc32(out, CRC_SPAN));
}

static enum slot_state decode(const unsigned char *in, struct ring_record *rec)
{
	unsigned i;

	for (i = 0; i < RING_REC_SIZE && in[i] == 0; i++)
		;
	if (i == RING_REC_SIZE)
		return SLOT_FREE;
	if (crc32(in, CRC_SPAN) != get_le32(in + 12))
		return SLOT_BAD;
	rec->seq = get_le32(in);
	rec->ts = get_le32(in + 4);
	rec->reading = (uint16_t)(in[8] | in[9] << 8);
	rec->fault = in[10];
	return SLOT_VALID;
}

static const char *base_name(const char *path)
{
	const char *slash = strrchr(path, '/');

	return slash ? slash + 1 : path;
}

/*
 * Decode nslots records from buf. Valid records go to out (if non-NULL) in
 * slot order. Sequence numbers are not expected to wrap (2^32 records at
 * one per 20 min is ~160,000 years).
 */
static void scan_slots(const unsigned char *buf, uint32_t nslots,
		       const char *name, struct ring_record *out,
		       struct scan *s)
{
	struct ring_record rec;

	memset(s, 0, sizeof *s);
	for (uint32_t slot = 0; slot < nslots; slot++) {
		switch (decode(buf + (size_t)slot * RING_REC_SIZE, &rec)) {
		case SLOT_FREE:
			break;
		case SLOT_BAD:
			log_msg(L_INFO, "store", "%s slot %u bad CRC, discarded",
				name, (unsigned)slot);
			s->n_bad++;
			break;
		case SLOT_VALID:
			if (out)
				out[s->n_valid] = rec;
			s->n_valid++;
			if (!s->found || rec.seq > s->max_seq) {
				s->found = 1;
				s->max_seq = rec.seq;
				s->max_slot = slot;
			}
			break;
		}
	}
}

static int read_full(int fd, unsigned char *buf, size_t len, const char *name)
{
	size_t done = 0;

	while (done < len) {
		ssize_t n = pread(fd, buf + done, len - done, (off_t)done);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			log_msg(L_ERROR, "store", "%s read failed (errno=%d)",
				name, errno);
			return -1;
		}
		if (n == 0) {
			log_msg(L_ERROR, "store", "%s unexpected EOF at byte %zu",
				name, done);
			return -1;
		}
		done += (size_t)n;
	}
	return 0;
}

/* fsync the directory holding path, so a newly created file survives. */
static int fsync_dir(const char *path, const char *name)
{
	char *dir = NULL;
	char *slash;
	const char *d;
	int fd = -1;
	int ret = -1;

	dir = strdup(path);
	if (!dir) {
		log_msg(L_ERROR, "store", "%s out of memory", name);
		goto out;
	}
	slash = strrchr(dir, '/');
	if (!slash) {
		d = ".";
	} else {
		if (slash == dir)
			slash[1] = '\0';
		else
			*slash = '\0';
		d = dir;
	}
	fd = open(d, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) {
		log_msg(L_ERROR, "store", "%s open dir %s failed (errno=%d)",
			name, d, errno);
		goto out;
	}
	if (fsync(fd) < 0) {
		log_msg(L_ERROR, "store", "%s fsync dir %s failed (errno=%d)",
			name, d, errno);
		goto out;
	}
	ret = 0;
out:
	if (fd >= 0 && close(fd) < 0) {
		log_msg(L_ERROR, "store", "%s close dir failed (errno=%d)",
			name, errno);
		ret = -1;
	}
	free(dir);
	return ret;
}

int ring_open(const char *path, struct ring **out)
{
	const char *name = base_name(path);
	struct ring *r = NULL;
	unsigned char *buf = NULL;
	struct stat st;
	struct scan s;
	int fd = -1;
	int ret = -1;

	*out = NULL;
	r = calloc(1, sizeof *r);
	if (!r) {
		log_msg(L_ERROR, "store", "%s out of memory", name);
		goto out;
	}
	r->fd = -1;
	r->name = strdup(name);
	if (!r->name) {
		log_msg(L_ERROR, "store", "%s out of memory", name);
		goto out;
	}

	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) {
		log_msg(L_ERROR, "store", "%s open %s failed (errno=%d)",
			name, path, errno);
		goto out;
	}
	if (fstat(fd, &st) < 0) {
		log_msg(L_ERROR, "store", "%s fstat failed (errno=%d)",
			name, errno);
		goto out;
	}
	if (st.st_size > RING_FILE_SIZE) {
		log_msg(L_ERROR, "store",
			"%s is %lld bytes, larger than a ring (%lld), refusing",
			name, (long long)st.st_size, (long long)RING_FILE_SIZE);
		goto out;
	}
	if (st.st_size < RING_FILE_SIZE) {
		if (st.st_size != 0)
			log_msg(L_WARN, "store", "%s short (%lld bytes), extending",
				name, (long long)st.st_size);
		if (ftruncate(fd, RING_FILE_SIZE) < 0) {
			log_msg(L_ERROR, "store", "%s ftruncate failed (errno=%d)",
				name, errno);
			goto out;
		}
		if (fsync(fd) < 0) {
			log_msg(L_ERROR, "store", "%s fsync failed (errno=%d)",
				name, errno);
			goto out;
		}
		if (fsync_dir(path, name) < 0)
			goto out;
	}

	buf = malloc((size_t)RING_FILE_SIZE);
	if (!buf) {
		log_msg(L_ERROR, "store", "%s out of memory", name);
		goto out;
	}
	if (read_full(fd, buf, (size_t)RING_FILE_SIZE, name) < 0)
		goto out;

	scan_slots(buf, RING_SLOTS, name, NULL, &s);
	r->fd = fd;
	fd = -1;
	r->next_slot = s.found ? (s.max_slot + 1) % RING_SLOTS : 0;
	r->next_seq = s.found ? s.max_seq + 1 : 1;
	log_msg(L_INFO, "store", "%s opened: %zu valid, %zu bad, next slot %u seq %u",
		name, s.n_valid, s.n_bad, (unsigned)r->next_slot,
		(unsigned)r->next_seq);
	*out = r;
	r = NULL;
	ret = 0;
out:
	free(buf);
	if (fd >= 0 && close(fd) < 0)
		log_msg(L_ERROR, "store", "%s close failed (errno=%d)",
			name, errno);
	if (r) {
		free(r->name);
		free(r);
	}
	return ret;
}

int ring_append(struct ring *r, uint32_t ts, uint16_t reading, uint8_t fault)
{
	struct ring_record rec = {
		.seq = r->next_seq, .ts = ts, .reading = reading, .fault = fault,
	};
	unsigned char buf[RING_REC_SIZE];
	off_t off = (off_t)r->next_slot * RING_REC_SIZE;
	size_t done = 0;

	encode(&rec, buf);
	while (done < sizeof buf) {
		ssize_t n = pwrite(r->fd, buf + done, sizeof buf - done,
				   off + (off_t)done);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			log_msg(L_ERROR, "store", "%s write slot %u failed (errno=%d)",
				r->name, (unsigned)r->next_slot, errno);
			return -1;
		}
		if (n == 0) {
			log_msg(L_ERROR, "store", "%s write slot %u wrote 0 bytes",
				r->name, (unsigned)r->next_slot);
			return -1;
		}
		done += (size_t)n;
	}
	if (fsync(r->fd) < 0) {
		log_msg(L_ERROR, "store", "%s fsync failed (errno=%d)",
			r->name, errno);
		return -1;
	}
	/* Only advance once durable; a failed write retries the same slot. */
	r->next_slot = (r->next_slot + 1) % RING_SLOTS;
	r->next_seq++;
	return 0;
}

int ring_close(struct ring *r)
{
	int ret = 0;

	if (!r)
		return 0;
	if (r->fd >= 0 && close(r->fd) < 0) {
		log_msg(L_ERROR, "store", "%s close failed (errno=%d)",
			r->name, errno);
		ret = -1;
	}
	free(r->name);
	free(r);
	return ret;
}

int ring_read_all(const char *path, struct ring_record *out,
		  size_t *n_valid, size_t *n_bad)
{
	const char *name = base_name(path);
	unsigned char *buf = NULL;
	struct stat st;
	struct scan s;
	uint32_t nslots;
	int fd = -1;
	int ret = -1;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		log_msg(L_ERROR, "store", "%s open %s failed (errno=%d)",
			name, path, errno);
		goto out;
	}
	if (fstat(fd, &st) < 0) {
		log_msg(L_ERROR, "store", "%s fstat failed (errno=%d)",
			name, errno);
		goto out;
	}
	if (st.st_size > RING_FILE_SIZE) {
		log_msg(L_ERROR, "store",
			"%s is %lld bytes, larger than a ring (%lld)",
			name, (long long)st.st_size, (long long)RING_FILE_SIZE);
		goto out;
	}
	if (st.st_size < RING_FILE_SIZE)
		log_msg(L_WARN, "store", "%s short (%lld bytes), reading whole slots only",
			name, (long long)st.st_size);
	nslots = (uint32_t)(st.st_size / RING_REC_SIZE);

	buf = malloc((size_t)RING_FILE_SIZE);
	if (!buf) {
		log_msg(L_ERROR, "store", "%s out of memory", name);
		goto out;
	}
	if (read_full(fd, buf, (size_t)nslots * RING_REC_SIZE, name) < 0)
		goto out;

	scan_slots(buf, nslots, name, out, &s);
	*n_valid = s.n_valid;
	*n_bad = s.n_bad;
	ret = 0;
out:
	free(buf);
	if (fd >= 0 && close(fd) < 0) {
		log_msg(L_ERROR, "store", "%s close failed (errno=%d)",
			name, errno);
		ret = -1;
	}
	return ret;
}
