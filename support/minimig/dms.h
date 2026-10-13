#ifndef __MINIMIG_DMS_H__
#define __MINIMIG_DMS_H__

// DMS (Disk Masher System) archive -> ADF image, in memory.
//
// Dependency-free on purpose, so tests/ can build it on the host. A C++ port of
// the xdms-rs crate (MIT), itself a port of the public-domain xDMS by Andre
// Rodrigues de la Rocha and Heikki Orsila. See dms.cpp.

#include <stddef.h>
#include <stdint.h>
#include <vector>

enum dms_result
{
	DMS_OK = 0,
	DMS_ERR_NOT_DMS,        // no "DMS!" magic
	DMS_ERR_TRUNCATED,      // archive ends mid-header or mid-track
	DMS_ERR_HEADER_CRC,     // archive or track header CRC mismatch
	DMS_ERR_DATA_CRC,       // packed track data CRC mismatch
	DMS_ERR_CHECKSUM,       // unpacked track checksum mismatch
	DMS_ERR_BAD_DATA,       // compressed stream is corrupt
	DMS_ERR_UNKNOWN_MODE,   // compression mode byte not 0..6
	DMS_ERR_TOO_LARGE,      // a track declares more than 32000 bytes
	DMS_ERR_ENCRYPTED,      // needs a password, which there is no way to give
	DMS_ERR_FMS,            // an FMS file, not a disk image
	DMS_ERR_NOT_DD,         // a data track is not 11264 bytes (HD, or odd)
	DMS_ERR_NO_TRACKS,      // no data tracks at all
};

// Size of one DD cylinder (2 heads x 11 sectors x 512 bytes), which is what a
// DMS "track" holds, and of the full 80-cylinder image produced.
#define DMS_CYL_SIZE (2 * 11 * 512)
#define DMS_CYLINDERS 80
#define DMS_ADF_SIZE (DMS_CYLINDERS * DMS_CYL_SIZE)

// Unpacks `len` bytes of DMS archive into `adf`, which comes back
// DMS_ADF_SIZE bytes long on success. Each cylinder lands at its own track
// number's offset, so an archive covering only part of a disk still gives a
// correctly laid out image; cylinders it does not carry stay zero.
// `bad_track`, if given, is set to the track number an error was found on, or
// -1 when the error is not about one track.
dms_result dms_unpack(const uint8_t *dms, size_t len, std::vector<uint8_t> &adf, int *bad_track = 0);

const char *dms_strerror(dms_result r);

#endif
