#ifndef STORE_RING_H
#define STORE_RING_H

#include <stddef.h>
#include <stdint.h>

/*
 * Fixed-size ring file of 16-byte little-endian records:
 *   0..3  sequence number
 *   4..7  timestamp (Unix time)
 *   8..9  reading
 *   10    fault flag
 *   11    unused (0)
 *   12..15 CRC-32 over bytes 0..11
 * An all-zero slot is free. Any other slot with a bad CRC is discarded.
 */
#define RING_SLOTS 2160u
#define RING_REC_SIZE 16u

struct ring_record {
	uint32_t seq;
	uint32_t ts;
	uint16_t reading;
	uint8_t fault;
};

struct ring;

/* Open (creating or extending if needed) and scan a ring for writing. */
int ring_open(const char *path, struct ring **out);

/* Overwrite the oldest slot with a new record, then fsync. */
int ring_append(struct ring *r, uint32_t ts, uint16_t reading, uint8_t fault);

int ring_close(struct ring *r);

/*
 * Read a ring file without modifying it. Valid records are stored in slot
 * order into out, which must hold RING_SLOTS entries.
 */
int ring_read_all(const char *path, struct ring_record *out,
		  size_t *n_valid, size_t *n_bad);

#endif
