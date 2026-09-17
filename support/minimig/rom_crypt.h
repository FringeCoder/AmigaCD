#ifndef ROM_CRYPT_H
#define ROM_CRYPT_H

#include <stdint.h>

// Amiga Forever ROM encryption.
//
// Cloanto ships Kickstart images encrypted: the file is the plain ROM with an
// 11-byte header in front of it, and the whole payload XORed with the bytes of
// ROM.KEY repeating. That is the entire scheme -- there is no block structure,
// no per-file nonce and no integrity check, which is why the only way to know
// a decryption went wrong is that the machine does not boot.
//
// The arithmetic already existed inside SendFileV2(), inline, on the way to the
// FPGA. It is pulled out here for two reasons. It is needed in a second place
// -- the ext-ROM composite path reads whole files into a buffer and could not
// use a streaming decryptor at all -- and a rotating-key XOR has exactly one
// interesting case, the wrap, which is worth a test rather than a re-reading.
//
// Kept free of MiSTer headers so it builds with the host compiler.

// The header Cloanto puts in front of the payload.
#define ROM_ENC_HEADER_BYTES 11

// The plain size an encrypted file of `file_size` bytes carries, or 0 when
// that is not an encrypted form of a ROM size this core knows.
//
// A size test is all anyone has: the header carries no magic to check, so an
// encrypted 512 KB ROM and any other 524,299-byte file are indistinguishable
// until the Amiga either boots or does not.
int rom_enc_plain_size(int file_size);

// XOR `bytes` of `buf` with `key`, repeating the key from its start. In place,
// and its own inverse: running it twice with the same key returns the input,
// which is what the round-trip test leans on.
//
// No-ops on a null buffer, a null key, a non-positive key size or a
// non-positive length, so a caller that never found ROM.KEY gets its
// ciphertext back unchanged rather than a crash.
void rom_decrypt(uint8_t *buf, int bytes, const uint8_t *key, int keysize);

#endif
