#include "rom_crypt.h"

int rom_enc_plain_size(int file_size)
{
	// The sizes UploadKickstart knows how to place, each plus the header.
	// Listed rather than computed from "size - 11" because the point is to
	// recognise a ROM, not any file that happens to be 11 bytes over.
	switch (file_size)
	{
	case 0x2000   + ROM_ENC_HEADER_BYTES: return 0x2000;    // A1000 boot ROM
	case 0x40000  + ROM_ENC_HEADER_BYTES: return 0x40000;   // 256 KB
	case 0x80000  + ROM_ENC_HEADER_BYTES: return 0x80000;   // 512 KB
	case 0x100000 + ROM_ENC_HEADER_BYTES: return 0x100000;  // 1 MB
	}
	return 0;
}

void rom_decrypt(uint8_t *buf, int bytes, const uint8_t *key, int keysize)
{
	if (!buf || !key || keysize <= 0 || bytes <= 0) return;

	int k = 0;
	for (int i = 0; i < bytes; i++)
	{
		buf[i] ^= key[k++];
		// Subtract rather than modulo, which is the form SendFileV2 has always
		// used. Equivalent here only because k is stepped one at a time and so
		// can never overshoot by more than the one.
		if (k >= keysize) k -= keysize;
	}
}
