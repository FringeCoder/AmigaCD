// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CD32 FMV module's sound (docs/fmv-plan.md phase 4): kjmp2 on a thread
// of its own, pinned to ARM core 0 beside the video decoder. Phase 1
// measured about 2 ms of decoding per Layer II frame on the MiSTer, too much
// for Main's own thread at 38 frames a second.
//
// The L64111 model hands over each whole frame as it takes it into its
// channel buffer (fmv_audio_frame), numbered in stream order, and says when
// the chip puts the next one out (fmv_audio_tick). The tick is the clock:
// the game paces its CD reads on the L64111's interrupts, and those follow
// the same schedule. Each tick appends that frame's 1152 stereo samples to
// the output, or 1152 of silence if it is not decoded yet or the chip is
// muted, so the output never runs ahead of or behind the chip. Main takes
// the output a CD sector (588 frames) at a time into the core's CDDA FIFO.
//
// All calls are from Main's thread.

#ifndef FMV_AUDIO_H
#define FMV_AUDIO_H

#include <stddef.h>
#include <stdint.h>

enum {
	FMV_AUDIO_FRAME_SAMPLES = 1152,   // stereo samples per Layer II frame
	FMV_AUDIO_SECTOR_FRAMES = 588,    // stereo samples per CD audio sector
	FMV_AUDIO_RATE          = 44100,  // the CDDA FIFO's rate; others are not played
};

struct fmv_audio;

// cpu: the core to pin the decoding thread to, or -1.
fmv_audio *fmv_audio_new(int cpu);
void fmv_audio_free(fmv_audio *a);

// One whole MPEG audio frame, header included, in stream order.
void fmv_audio_frame(fmv_audio *a, const uint8_t *p, size_t n, uint32_t sample_rate);
// The chip puts out the next frame. muted: SOFT_MUTE or the board's bit.
void fmv_audio_tick(fmv_audio *a, bool muted);
// The channel buffer is emptied: frames given and not yet played are dropped.
// What is already in the output stays (it is "in the DAC").
void fmv_audio_flush(fmv_audio *a);

// One sector of output, L R interleaved in host order, if there is one.
bool fmv_audio_sector(fmv_audio *a, int16_t out[2 * FMV_AUDIO_SECTOR_FRAMES]);
// Stereo samples waiting in the output.
size_t fmv_audio_pending(fmv_audio *a);
// The output went unplayed for too long (CD audio owned the FIFO, or Main
// stalled): drop it, so the sound does not lag the picture.
void fmv_audio_drop_output(fmv_audio *a);

struct fmv_audio_stats {
	uint32_t frames_in;       // handed over
	uint32_t decoded;
	uint32_t bad;             // kjmp2 could not decode (Layer I, or damaged)
	uint32_t ticks;
	uint32_t late;            // ticks whose frame was not decoded yet: silence
	uint32_t muted;           // ticks played as silence for the mute
	uint32_t wrong_rate;      // frames not at 44.1 kHz: silence
	uint32_t flushes;
	uint32_t dropped;         // stereo samples dropped from the output
	uint32_t sectors;         // taken by fmv_audio_sector
};
void fmv_audio_get_stats(fmv_audio *a, fmv_audio_stats *s);

// Wait until every frame handed over is decoded (tests only).
void fmv_audio_drain(fmv_audio *a);

#endif // FMV_AUDIO_H
