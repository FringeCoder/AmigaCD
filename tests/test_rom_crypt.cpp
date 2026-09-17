// Amiga Forever ROM decryption.
//
// The scheme is a rotating-key XOR over the payload after an 11-byte header,
// and it has no integrity check of any kind. Nothing in the file says whether
// the key was right; a wrong key produces a ROM-sized block of plausible
// nonsense, and the only symptom is an Amiga that does not boot. So the
// arithmetic gets a test rather than a careful read.
//
// The interesting case is the wrap. Everything else is a loop.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../support/minimig/rom_crypt.h"

static int failures = 0;

static void check(const char *name, long got, long want)
{
	if (got == want) printf("PASS %s\n", name);
	else { printf("FAIL %s: got %ld, want %ld\n", name, got, want); failures++; }
}

static void check_bytes(const char *name, const uint8_t *got, const uint8_t *want, int n)
{
	for (int i = 0; i < n; i++)
	{
		if (got[i] != want[i])
		{
			printf("FAIL %s: byte %d is 0x%02X, want 0x%02X\n", name, i, got[i], want[i]);
			failures++;
			return;
		}
	}
	printf("PASS %s\n", name);
}

int main(void)
{
	// --- the size table -------------------------------------------------
	// Each is the plain size plus 11. These are the numbers a user reads off
	// their own files, so they are written out rather than computed.
	check("256 KB encrypted is 262155", rom_enc_plain_size(262155), 0x40000);
	check("512 KB encrypted is 524299", rom_enc_plain_size(524299), 0x80000);
	check("1 MB encrypted is 1048587",  rom_enc_plain_size(1048587), 0x100000);
	check("A1000 boot ROM encrypted is 8203", rom_enc_plain_size(8203), 0x2000);

	// A plain ROM is not an encrypted one. This is the distinction the whole
	// dispatch rests on, and getting it backwards would send a plain ROM
	// through the decryptor and turn a working Kickstart into noise.
	check("256 KB plain is not encrypted", rom_enc_plain_size(0x40000), 0);
	check("512 KB plain is not encrypted", rom_enc_plain_size(0x80000), 0);
	check("1 MB plain is not encrypted",   rom_enc_plain_size(0x100000), 0);
	check("a size 11 over nothing known",  rom_enc_plain_size(123456 + 11), 0);
	check("zero is not a ROM",             rom_enc_plain_size(0), 0);

	// --- the XOR --------------------------------------------------------
	// A key that does NOT divide the payload length, so the wrap lands
	// mid-buffer rather than on a boundary. 7 into 20 wraps twice and ends
	// part way through the key, which is the case a modulo-free counter can
	// get wrong.
	static const uint8_t key[7] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
	uint8_t plain[20];
	for (int i = 0; i < 20; i++) plain[i] = (uint8_t)(i * 13 + 5);

	uint8_t buf[20];
	memcpy(buf, plain, sizeof(buf));
	rom_decrypt(buf, 20, key, 7);

	// Spelled out rather than computed the same way the implementation does:
	// a check that repeats the code it checks agrees with any bug in it.
	uint8_t want[20];
	for (int i = 0; i < 20; i++) want[i] = (uint8_t)(plain[i] ^ key[i % 7]);
	check_bytes("key repeats from its start across the payload", buf, want, 20);

	// Its own inverse, which is what makes a round trip meaningful.
	rom_decrypt(buf, 20, key, 7);
	check_bytes("decrypting twice returns the input", buf, plain, 20);

	// --- edges ----------------------------------------------------------
	memcpy(buf, plain, sizeof(buf));
	rom_decrypt(buf, 20, key, 1);
	uint8_t want1[20];
	for (int i = 0; i < 20; i++) want1[i] = (uint8_t)(plain[i] ^ key[0]);
	check_bytes("a one byte key applies to every byte", buf, want1, 20);

	// A key longer than the payload never wraps at all.
	memcpy(buf, plain, sizeof(buf));
	rom_decrypt(buf, 3, key, 7);
	uint8_t want3[3];
	for (int i = 0; i < 3; i++) want3[i] = (uint8_t)(plain[i] ^ key[i]);
	check_bytes("a key longer than the payload does not wrap", buf, want3, 3);
	check("and leaves the rest alone", buf[3], plain[3]);

	// Refusals. A caller that never found ROM.KEY passes keysize 0, and must
	// get its ciphertext back untouched rather than a crash or a half-pass.
	memcpy(buf, plain, sizeof(buf));
	rom_decrypt(buf, 20, key, 0);
	check_bytes("no key leaves the buffer alone", buf, plain, 20);

	rom_decrypt(buf, 20, key, -1);
	check_bytes("a negative key size leaves the buffer alone", buf, plain, 20);

	rom_decrypt(buf, 0, key, 7);
	check_bytes("zero length leaves the buffer alone", buf, plain, 20);

	rom_decrypt(buf, -5, key, 7);
	check_bytes("a negative length leaves the buffer alone", buf, plain, 20);

	rom_decrypt(NULL, 20, key, 7);          // must not crash
	rom_decrypt(buf, 20, NULL, 7);          // must not crash
	check_bytes("a null key leaves the buffer alone", buf, plain, 20);

	if (failures) { printf("%d FAILURES\n", failures); return 1; }
	printf("ALL TESTS PASSED\n");
	return 0;
}
