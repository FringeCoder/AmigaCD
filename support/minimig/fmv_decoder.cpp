// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CD32 FMV module's video decoder: libmpeg2 on its own thread. See
// fmv_decoder.h.

#include "fmv_decoder.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

extern "C" {
#include "mpeg2.h"
}

enum {
	RING_BYTES  = 1 << 20,   // about 6 s of a Video CD's video: Main never gets that far ahead
	CHUNK_BYTES = 16384,     // handed to libmpeg2 at a time
	STAGE_FLUSH = 4096,      // staged bytes handed over before the next poll
};

// What the thread is doing, for fmv_decoder_drain().
enum { T_RUNNING, T_WAIT_DATA, T_WAIT_BUF };

struct fmv_decoder {
	volatile uint8_t *fb;
	pthread_t        th;
	bool             th_ok;
	pthread_mutex_t  mu;
	pthread_cond_t   cv_data;   // bytes, a free buffer, a flush or stop
	pthread_cond_t   cv_idle;   // the thread waits (drain)

	// ---- under mu ----
	std::vector<uint8_t> ring;
	size_t           r_head, r_tail;   // bytes ever written / read
	uint32_t         gen;              // flush generation
	bool             stop;
	int              tstate;
	fmv_bufring      bufs;
	fmv_window       win;
	fmv_decoder_stats st;

	// ---- Main's thread only ----
	std::vector<uint8_t> stage;
};

static size_t ring_fill(const fmv_decoder *d) { return d->r_head - d->r_tail; }

// Copy a converted window into a frame buffer. The window is in 64-bit
// words: a row starts on a 1024-byte boundary and src_w is a multiple of 4,
// so the stores are aligned, as uncached device memory needs.
static void copy_out(volatile uint8_t *dst, const uint8_t *src, const fmv_window &w)
{
	const size_t words = (size_t)w.src_w / 4;
	for (int r = 0; r < w.src_h; r++) {
		volatile uint32_t *o = (volatile uint32_t *)(dst + (size_t)r * FMV_FB_STRIDE);
		const uint32_t *s = (const uint32_t *)(src + (size_t)r * FMV_FB_STRIDE);
		for (size_t k = 0; k < 2 * words; k++)
			o[k] = s[k];
	}
}

static void *thread_main(void *arg)
{
	fmv_decoder *d = (fmv_decoder *)arg;
	mpeg2dec_t *dec = mpeg2_init();
	const mpeg2_info_t *info = mpeg2_info(dec);
	std::vector<uint8_t> chunk(CHUNK_BYTES);
	std::vector<uint8_t> scratch((size_t)FMV_FB_MAX_H * FMV_FB_STRIDE);
	uint32_t my_gen;

	pthread_mutex_lock(&d->mu);
	my_gen = d->gen;
	pthread_mutex_unlock(&d->mu);

	for (;;) {
		// ---- more bytes ----
		size_t n = 0;
		pthread_mutex_lock(&d->mu);
		while (!d->stop && ring_fill(d) == 0 && d->gen == my_gen) {
			d->tstate = T_WAIT_DATA;
			pthread_cond_broadcast(&d->cv_idle);
			pthread_cond_wait(&d->cv_data, &d->mu);
		}
		d->tstate = T_RUNNING;
		if (d->stop) {
			pthread_mutex_unlock(&d->mu);
			break;
		}
		bool reset = d->gen != my_gen;
		my_gen = d->gen;
		if (!reset) {
			n = ring_fill(d);
			if (n > CHUNK_BYTES) n = CHUNK_BYTES;
			for (size_t i = 0; i < n; i++)
				chunk[i] = d->ring[(d->r_tail + i) % RING_BYTES];
			d->r_tail += n;
		}
		pthread_mutex_unlock(&d->mu);
		if (reset) {
			// Keep the sequence: a flush mid-stream is followed by the
			// next GOP, not necessarily a sequence header.
			mpeg2_reset(dec, 0);
			continue;
		}
		mpeg2_buffer(dec, chunk.data(), chunk.data() + n);

		// ---- decode until libmpeg2 wants more ----
		bool flushed = false;
		while (!flushed) {
			mpeg2_state_t s = mpeg2_parse(dec);
			if (s == STATE_BUFFER)
				break;
			if (s == STATE_SEQUENCE) {
				pthread_mutex_lock(&d->mu);
				d->st.sequences++;
				d->st.width = (uint16_t)info->sequence->width;
				d->st.height = (uint16_t)info->sequence->height;
				pthread_mutex_unlock(&d->mu);
				continue;
			}
			if (!((s == STATE_SLICE || s == STATE_END || s == STATE_INVALID_END) &&
			      info->display_fbuf))
				continue;

			// A picture to show: a buffer for it, waiting if all four are
			// shown, retiring or ready (the display tick frees them).
			int b;
			fmv_window w;
			pthread_mutex_lock(&d->mu);
			while ((b = fmv_bufring_acquire(&d->bufs)) < 0 && !d->stop && d->gen == my_gen) {
				d->tstate = T_WAIT_BUF;
				pthread_cond_broadcast(&d->cv_idle);
				pthread_cond_wait(&d->cv_data, &d->mu);
			}
			d->tstate = T_RUNNING;
			w = d->win;
			if (d->stop || d->gen != my_gen) {
				fmv_bufring_abort(&d->bufs, b);
				pthread_mutex_unlock(&d->mu);
				flushed = true;          // the rest of this chunk is stale
				break;
			}
			pthread_mutex_unlock(&d->mu);

			const mpeg2_sequence_t *q = info->sequence;
			fmv_yuv420_to_rgb565(info->display_fbuf->buf, (int)q->width, (int)q->height,
			                     w, scratch.data());
			copy_out(d->fb + (size_t)b * FMV_FB_BUF_BYTES, scratch.data(), w);
			__sync_synchronize();    // the pixels before the name (contract v5)

			pthread_mutex_lock(&d->mu);
			if (d->gen == my_gen) {
				fmv_bufring_ready(&d->bufs, b);
				d->st.pictures++;
			} else {
				fmv_bufring_abort(&d->bufs, b);
				flushed = true;
			}
			pthread_mutex_unlock(&d->mu);
		}
	}
	mpeg2_close(dec);
	return 0;
}

fmv_decoder *fmv_decoder_new(volatile uint8_t *fb, int cpu)
{
	fmv_decoder *d = new fmv_decoder();
	d->fb = fb;
	d->ring.resize(RING_BYTES);
	d->r_head = d->r_tail = 0;
	d->gen = 0;
	d->stop = false;
	d->tstate = T_RUNNING;
	fmv_bufring_init(&d->bufs);
	memset(&d->win, 0, sizeof d->win);
	memset(&d->st, 0, sizeof d->st);
	d->st.shown = -1;
	pthread_mutex_init(&d->mu, 0);
	pthread_cond_init(&d->cv_data, 0);
	pthread_cond_init(&d->cv_idle, 0);

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	if (cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		pthread_attr_setaffinity_np(&attr, sizeof set, &set);
	}
	d->th_ok = pthread_create(&d->th, &attr, thread_main, d) == 0;
	pthread_attr_destroy(&attr);
	if (!d->th_ok) {
		fmv_decoder_free(d);
		return 0;
	}
	return d;
}

void fmv_decoder_free(fmv_decoder *d)
{
	if (!d)
		return;
	if (d->th_ok) {
		pthread_mutex_lock(&d->mu);
		d->stop = true;
		pthread_cond_broadcast(&d->cv_data);
		pthread_mutex_unlock(&d->mu);
		pthread_join(d->th, 0);
	}
	pthread_cond_destroy(&d->cv_idle);
	pthread_cond_destroy(&d->cv_data);
	pthread_mutex_destroy(&d->mu);
	delete d;
}

void fmv_decoder_bytes(fmv_decoder *d, const uint8_t *p, size_t n)
{
	d->stage.insert(d->stage.end(), p, p + n);
	if (d->stage.size() >= STAGE_FLUSH)
		fmv_decoder_poll(d);
}

void fmv_decoder_poll(fmv_decoder *d)
{
	if (d->stage.empty())
		return;
	pthread_mutex_lock(&d->mu);
	size_t room = RING_BYTES - ring_fill(d);
	size_t n = d->stage.size();
	if (n > room) {
		// The decoder is more than a ring behind. What does not fit is lost;
		// libmpeg2 resynchronises at the next start code.
		d->st.dropped_bytes += (uint32_t)(n - room);
		n = room;
	}
	for (size_t i = 0; i < n; i++)
		d->ring[(d->r_head + i) % RING_BYTES] = d->stage[i];
	d->r_head += n;
	d->st.bytes_in += (uint32_t)n;
	if (d->tstate == T_WAIT_DATA && n)
		d->tstate = T_RUNNING;     // until the thread says otherwise (drain)
	pthread_cond_broadcast(&d->cv_data);
	pthread_mutex_unlock(&d->mu);
	d->stage.clear();
}

void fmv_decoder_flush(fmv_decoder *d)
{
	d->stage.clear();
	pthread_mutex_lock(&d->mu);
	d->r_tail = d->r_head;
	d->gen++;
	d->st.flushes++;
	fmv_bufring_flush(&d->bufs);
	if (d->tstate != T_RUNNING)
		d->tstate = T_RUNNING;     // it wakes to reset its decoder
	pthread_cond_broadcast(&d->cv_data);
	pthread_mutex_unlock(&d->mu);
}

void fmv_decoder_set_window(fmv_decoder *d, const fmv_window &w)
{
	pthread_mutex_lock(&d->mu);
	d->win = w;
	pthread_mutex_unlock(&d->mu);
}

int fmv_decoder_present(fmv_decoder *d)
{
	pthread_mutex_lock(&d->mu);
	int b = fmv_bufring_present(&d->bufs);
	if (b >= 0) {
		d->st.presented++;
		d->st.shown = b;
		if (d->tstate == T_WAIT_BUF && fmv_bufring_count(&d->bufs, FMV_BUF_FREE))
			d->tstate = T_RUNNING;
		pthread_cond_broadcast(&d->cv_data);   // a buffer may be free now
	} else {
		d->st.late++;
	}
	pthread_mutex_unlock(&d->mu);
	return b;
}

void fmv_decoder_get_stats(fmv_decoder *d, fmv_decoder_stats *s)
{
	pthread_mutex_lock(&d->mu);
	*s = d->st;
	pthread_mutex_unlock(&d->mu);
}

void fmv_decoder_drain(fmv_decoder *d)
{
	fmv_decoder_poll(d);
	pthread_mutex_lock(&d->mu);
	while (!(d->tstate == T_WAIT_BUF ||
	         (d->tstate == T_WAIT_DATA && ring_fill(d) == 0)))
		pthread_cond_wait(&d->cv_idle, &d->mu);
	pthread_mutex_unlock(&d->mu);
}
