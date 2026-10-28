#include "store/crc32.h"

uint32_t crc32(const void *buf, size_t len)
{
	const unsigned char *p = buf;
	uint32_t crc = 0xFFFFFFFFu;

	/* Bitwise rather than table-driven: we only checksum 12-byte records. */
	while (len--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
	}
	return crc ^ 0xFFFFFFFFu;
}
