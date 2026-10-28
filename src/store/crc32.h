#ifndef STORE_CRC32_H
#define STORE_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320), same as zlib's crc32(). */
uint32_t crc32(const void *buf, size_t len);

#endif
