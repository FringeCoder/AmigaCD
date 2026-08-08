#ifndef MINIMIG_CRC32_H
#define MINIMIG_CRC32_H

#include <stdint.h>

// Standard CRC-32: reflected polynomial 0xEDB88320, init 0xFFFFFFFF,
// final xor 0xFFFFFFFF. Implemented in minimig_a2065_crc32.cpp.
uint32_t crc32_compute(const uint8_t *data, int len);

#endif
