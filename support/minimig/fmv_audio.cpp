// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CD32 FMV module's sound: kjmp2 on its own thread. See fmv_audio.h.

#include "fmv_audio.h"

#include <pthread.h>
#include <sched.h>
#include <string.h>

#include <deque>
#include <vector>

extern "C" {
#include "kjmp2.h"
}

namespace {

// A frame between the model and the output: compressed until the thread has
// decoded it.
struct slot {
	uint32_t seq;
	uint32_t rate;
	std::vector<uint8_t> in;
	std::vector<int16_t> pcm;   // 2 * 1152 once decoded
	bool decoded;
	bool bad;
};

} // namespace

enum { MAX_SLOTS = 128 };          // the L64111's channel buffer is 64 Layer II frames

struct fmv_audio {
	pthread_t       th;
	bool            th_ok;
	pthread_mutex_t mu;
	pthread_cond_t  cv_work;       // a frame to decode, or stop
	pthread_cond_t  cv_done;       // a frame decoded (drain)

	// ---- under mu ----
	std::deque<slot> slots;        // in seq order; the front is played next
	uint32_t next_seq;             // the number the next frame handed over gets
	uint32_t gen;                  // flush generation
	bool     stop;
	fmv_audio_stats st;

	// ---- Main's thread only ----
	std::deque<int16_t> out;       // L R interleaved
};

static void *thread_main(void *arg)
{
	fmv_audio *a = (fmv_audio *)arg;
	kjmp2_context_t mp2;
	kjmp2_init(&mp2);
	std::vector<uint8_t> in;
	std::vector<int16_t> pcm(2 * KJMP2_SAMPLES_PER_FRAME);
	uint32_t my_gen = 0;

	pthread_mutex_lock(&a->mu);
	for (;;) {
		// the first frame not yet decoded
		slot *s = 0;
		for (auto &x : a->slots)
			if (!x.decoded) { s = &x; break; }
		if (!s) {
			if (a->stop) break;
			pthread_cond_wait(&a->cv_work, &a->mu);
			continue;
		}
		uint32_t seq = s->seq, gen = a->gen;
		in = s->in;
		pthread_mutex_unlock(&a->mu);
		if (gen != my_gen) {
			kjmp2_init(&mp2);       // a new stream: no synthesis history
			my_gen = gen;
		}

		// kjmp2 reads up to KJMP2_MAX_FRAME_SIZE bytes; pad what it may
		// read past a short frame.
		in.resize(in.size() + KJMP2_MAX_FRAME_SIZE, 0);
		unsigned long used = kjmp2_decode_frame(&mp2, in.data(), pcm.data());

		pthread_mutex_lock(&a->mu);
		if (gen != a->gen)
			continue;               // flushed while decoding: the slot is gone
		for (auto &x : a->slots) {
			if (x.seq != seq) continue;
			x.decoded = true;
			x.bad = used == 0;
			if (used) {
				x.pcm = pcm;
				a->st.decoded++;
			} else {
				a->st.bad++;
			}
			break;
		}
		pthread_cond_broadcast(&a->cv_done);
	}
	pthread_mutex_unlock(&a->mu);
	return 0;
}

fmv_audio *fmv_audio_new(int cpu)
{
	fmv_audio *a = new fmv_audio();
	a->next_seq = 0;
	a->gen = 0;
	a->stop = false;
	memset(&a->st, 0, sizeof a->st);
	pthread_mutex_init(&a->mu, 0);
	pthread_cond_init(&a->cv_work, 0);
	pthread_cond_init(&a->cv_done, 0);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	if (cpu >= 0) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(cpu, &set);
		pthread_attr_setaffinity_np(&attr, sizeof set, &set);
	}
	a->th_ok = pthread_create(&a->th, &attr, thread_main, a) == 0;
	pthread_attr_destroy(&attr);
	if (!a->th_ok) {
		fmv_audio_free(a);
		return 0;
	}
	return a;
}

void fmv_audio_free(fmv_audio *a)
{
	if (!a)
		return;
	if (a->th_ok) {
		pthread_mutex_lock(&a->mu);
		a->stop = true;
		a->slots.clear();
		pthread_cond_broadcast(&a->cv_work);
		pthread_mutex_unlock(&a->mu);
		pthread_join(a->th, 0);
	}
	pthread_cond_destroy(&a->cv_done);
	pthread_cond_destroy(&a->cv_work);
	pthread_mutex_destroy(&a->mu);
	delete a;
}

void fmv_audio_frame(fmv_audio *a, const uint8_t *p, size_t n, uint32_t sample_rate)
{
	pthread_mutex_lock(&a->mu);
	a->st.frames_in++;
	if (a->slots.size() >= MAX_SLOTS)
		a->slots.pop_front();   // the chip would have overflowed first; keep the newest
	slot s;
	s.seq = a->next_seq++;
	s.rate = sample_rate;
	s.in.assign(p, p + n);
	s.decoded = false;
	s.bad = false;
	a->slots.push_back(s);
	pthread_cond_broadcast(&a->cv_work);
	pthread_mutex_unlock(&a->mu);
}

static void push_silence(fmv_audio *a)
{
	a->out.insert(a->out.end(), 2 * FMV_AUDIO_FRAME_SAMPLES, (int16_t)0);
}

void fmv_audio_tick(fmv_audio *a, bool muted)
{
	pthread_mutex_lock(&a->mu);
	a->st.ticks++;
	if (a->slots.empty()) {
		// The model played a frame this side never received (a frame cut
		// short): keep the time with silence.
		a->st.late++;
		pthread_mutex_unlock(&a->mu);
		push_silence(a);
		return;
	}
	slot s = a->slots.front();
	a->slots.pop_front();
	pthread_mutex_unlock(&a->mu);

	if (muted) {
		a->st.muted++;
		push_silence(a);
	} else if (s.rate != FMV_AUDIO_RATE) {
		a->st.wrong_rate++;
		push_silence(a);
	} else if (!s.decoded || s.bad) {
		if (!s.decoded) a->st.late++;
		push_silence(a);
	} else {
		a->out.insert(a->out.end(), s.pcm.begin(), s.pcm.end());
	}
}

void fmv_audio_flush(fmv_audio *a)
{
	pthread_mutex_lock(&a->mu);
	a->slots.clear();
	a->gen++;
	a->st.flushes++;
	pthread_mutex_unlock(&a->mu);
}

bool fmv_audio_sector(fmv_audio *a, int16_t out[2 * FMV_AUDIO_SECTOR_FRAMES])
{
	if (a->out.size() < 2 * FMV_AUDIO_SECTOR_FRAMES)
		return false;
	for (int i = 0; i < 2 * FMV_AUDIO_SECTOR_FRAMES; i++) {
		out[i] = a->out.front();
		a->out.pop_front();
	}
	a->st.sectors++;
	return true;
}

size_t fmv_audio_pending(fmv_audio *a) { return a->out.size() / 2; }

void fmv_audio_drop_output(fmv_audio *a)
{
	a->st.dropped += (uint32_t)(a->out.size() / 2);
	a->out.clear();
}

void fmv_audio_get_stats(fmv_audio *a, fmv_audio_stats *s)
{
	pthread_mutex_lock(&a->mu);
	*s = a->st;
	pthread_mutex_unlock(&a->mu);
}

void fmv_audio_drain(fmv_audio *a)
{
	pthread_mutex_lock(&a->mu);
	for (;;) {
		bool all = true;
		for (auto &x : a->slots)
			if (!x.decoded) { all = false; break; }
		if (all) break;
		pthread_cond_wait(&a->cv_done, &a->mu);
	}
	pthread_mutex_unlock(&a->mu);
}
