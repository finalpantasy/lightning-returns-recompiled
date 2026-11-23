/* hle_bink.c - bink2w32.dll for the portable runtime: FF13's movies played from MP4s.
 *
 * The movies (white_data/movie/<name>.wmp: a 16-byte "WMP" header + a Bink 2 stream) are converted once per install by
 * tools/bink2mp4 to <name>.mp4 beside them (H.264 1280x720 + AAC 5.1). Here the MP4 is decoded in-process (host_movie.c:
 * FFmpeg's libraries, statically linked) to 4:2:0 frames, read ahead by a host thread, and written into the game's own
 * frame buffers. Without its MP4, or when FFmpeg cannot decode it, BinkOpen fails, as the original runtime's stub
 * did: the game skips the movie. The first BinkOpen also starts converting the movies that have no MP4 yet, in the
 * background (host_movie_convert.c: Windows), for the next time they play. Built with -DXRC_SKIP_MOVIES every
 * BinkOpen fails (the game skips every movie), the background conversion still runs; -DXRC_NO_AUDIO: no movie audio.
 *
 * How FF13 drives Bink (its player: FUN_010081b0 open, FUN_0100a100 per tick):
 *   - BinkSetIO(its reader) + BinkOpen("LMV", 0x3204400): the name means nothing; the reader's open (the BINKIO open
 *     proc, called here with a scratch BINKIO) opens the movie's .wmp through CreateFileA, which tells which movie it is
 *     (xrc_last_open_path). The reader is closed again (BINKIO slot 5); the MP4 replaces it.
 *   - It reads the BINK struct's Width +0, Height +4, Frames +8, FrameNum +0xc, FrameRate +0x14, FrameRateDiv +0x18,
 *     and from BinkGetRealtime ReadBufferSize / ReadBufferUsed (dwords 11 / 12: equal = preloaded, it may start).
 *   - BinkGetFrameBuffersInfo(BINKFRAMEBUFFERS at player+0x60): TotalFrames, YABufferWidth / Height, cRcBBufferWidth /
 *     Height, FrameNum (+0x14: the buffer index decoded last, the one it shows; -1 = none yet), then per buffer (+0x18, 0x30 bytes)
 *     four planes {Allocate, Buffer, BufferPitch}: Y, cR, cB, A. It creates L8 textures of those sizes, locks them and
 *     puts the pointers / pitches in, then BinkRegisterFrameBuffers.
 *   - BinkDoFrameAsync decodes frame FrameNum into the other buffer; BinkDoFrameAsyncWait completes it (FrameNum of
 *     the frame buffers = that buffer); it unlocks and shows it; BinkNextFrame advances. BinkWait paces (0 = the next
 *     frame is due), BinkShouldSkip says it is a frame behind. FrameNum == Frames ends the movie.
 *   - Its YUV -> RGB is BT.601 limited range (the 1.164384 / 1.596027 / ... matrix at .rdata 0x2267270): the MP4s
 *     are encoded in it, so the decoded planes go in unchanged.
 * Audio: a second decoder (its own host thread) decodes the MP4's AAC 5.1 (or a stereo downmix, if the output takes no 5.1) to PCM,
 * a host thread feeds it to host_audio (the host's output); while it plays, its play position is the movie's clock
 * (the video frames are due by it, so they cannot drift apart); without audio, or after it ends, the wall clock. It
 * starts with the playback (the first unpause after the game's preroll) and pauses with BinkPause. The game's speaker
 * volumes (BinkSetSpeakerVolumes) are not applied. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* (pthread_setname_np) */
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include "xrc_rt.h"
#include "guest_types.h"
#include "host_audio.h"
#include "host_movie.h"
#include "host_movie_convert.h"

#define A(i) xrc_arg(c, (i))
#define RET(v, n) do { c->eax = (u32)(v); xrc_stdcall_ret(c, (n)); return; } while (0)
#define RETV(n) do { xrc_stdcall_ret(c, (n)); return; } while (0)
#define S(a) ((char *)(xrc_mem + (u32)(a)))

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ============================================================================================ the decoder threads */
#define RING 4
typedef struct Movie {
	u32 g;                         /* the guest BINK struct (0: a free slot) */
	unsigned w, h, frames, num, den;
	u32 fb;                        /* the registered BINKFRAMEBUFFERS */
	int cur, pending;              /* buffer index decoded last / being decoded (-1: none) */
	int paused;
	double t0, paused_at;          /* wall clock: when frame 1 is (was) due */
	/* audio: a host thread opening the output and the decoder, feeding the decoded PCM, and closing them (AJob) */
	struct AJob *aj;
	int a_started, a_done, a_run;  /* a_run: the playback started while the audio was still being opened */
	double a_run_t;
	/* the decoder: a host thread decoding into the ring */
	HostVideo *vsrc;
	pthread_t th;
	int have_thread;
	pthread_mutex_t m;
	pthread_cond_t cv;
	unsigned char *ring[RING];
	unsigned ring_frame[RING];     /* the frame number (1-based) in each slot */
	int rhead, rcount, eof, stop;
	unsigned produced;             /* frames read so far */
	int warned;
	/* XRC_BINK_STATS: per 300 frames shown */
	unsigned st_shown, st_skips, st_late;
	double st_wait, st_wait_max, st_copy, st_gap_max, st_last;
} Movie;
static int stats = -1;
#define NMOV 2
static Movie movs[NMOV];

static unsigned fsize(const Movie *m) { return m->w * m->h * 3 / 2; }

static void *reader(void *p) {
#if !defined(_WIN32) && !defined(XRC_NO_TOOLS)
	pthread_setname_np(pthread_self(), "bink-read");
#endif
	Movie *m = (Movie *)p;
	unsigned n = fsize(m);
	unsigned char *tmp = (unsigned char *)malloc(n);
	for (;;) {
		int got = tmp && host_video_next(m->vsrc, tmp);
		pthread_mutex_lock(&m->m);
		if (!got || m->stop) {
			m->eof = 1;
			pthread_cond_broadcast(&m->cv);
			pthread_mutex_unlock(&m->m);
			break;
		}
		while (m->rcount == RING && !m->stop) pthread_cond_wait(&m->cv, &m->m);
		if (m->stop) { m->eof = 1; pthread_cond_broadcast(&m->cv); pthread_mutex_unlock(&m->m); break; }
		{
			int s = (m->rhead + m->rcount) % RING;
			memcpy(m->ring[s], tmp, n);
			m->ring_frame[s] = ++m->produced;
			m->rcount++;
		}
		pthread_cond_broadcast(&m->cv);
		pthread_mutex_unlock(&m->m);
	}
	free(tmp);
	return 0;
}

/* The movie audio, all on its own thread (opening the waveOut device and FFmpeg's decoder, and closing them, take
 * milliseconds: not on the game's main thread): the thread opens both (ready 1, or -1 when there is no audio), feeds
 * the PCM until the end or a stop, then waits for the stop (the main thread asks the output for its play position
 * until then) and closes them. Freed by the last of the two holders (refs). */
typedef struct AJob {
	HostAudio *au;
	HostAudioSrc *asrc;
	unsigned ch;
	int nth; /* (the audio stream: LR's language set; -1: the best) */
	volatile int ready, eof, stop, refs;
	pthread_mutex_t mu;
	pthread_cond_t cv;
	char *mp4;
} AJob;
static void aj_release(AJob *j) {
	if (__atomic_sub_fetch(&j->refs, 1, __ATOMIC_ACQ_REL)) return;
	pthread_mutex_destroy(&j->mu);
	pthread_cond_destroy(&j->cv);
	free(j->mp4);
	free(j);
}
static void *audio_thread(void *p) {
#if !defined(_WIN32) && !defined(XRC_NO_TOOLS)
	pthread_setname_np(pthread_self(), "bink-audio");
#endif
	AJob *j = (AJob *)p;
	unsigned n = 48000 / 25; /* (40 ms) */
	short *buf = 0;
	j->ch = 6;
	j->au = host_audio_open(48000, 6);
	if (!j->au) { j->ch = 2; j->au = host_audio_open(48000, 2); }
	if (j->au) {
		host_audio_pause(j->au, 1); /* (until the playback starts) */
		j->asrc = host_audio_src_open_nth(j->mp4, 48000, j->ch, j->nth);
	}
	if (!j->au || !j->asrc) { /* (no output or no audio track: the wall clock) */
		if (j->asrc) host_audio_src_close(j->asrc);
		if (j->au) host_audio_close(j->au);
		j->au = 0; j->asrc = 0;
		__atomic_store_n(&j->ready, -1, __ATOMIC_RELEASE);
		aj_release(j);
		return 0;
	}
	__atomic_store_n(&j->ready, 1, __ATOMIC_RELEASE);
	buf = (short *)malloc((size_t)n * j->ch * sizeof(short));
	while (!__atomic_load_n(&j->stop, __ATOMIC_ACQUIRE)) {
		unsigned got = buf ? host_audio_src_read(j->asrc, buf, n) : 0;
		if (!got || !host_audio_write(j->au, buf, got * j->ch * 2u)) break;
	}
	free(buf);
	__atomic_store_n(&j->eof, 1, __ATOMIC_RELEASE);
	pthread_mutex_lock(&j->mu);
	while (!j->stop) pthread_cond_wait(&j->cv, &j->mu);
	pthread_mutex_unlock(&j->mu);
	host_audio_src_close(j->asrc);
	host_audio_close(j->au);
	aj_release(j);
	return 0;
}
static int next_nth = -1; /* (the next movie's audio stream: BinkOpenWithOptions; -2: none) */
static void start_audio(Movie *m, const char *mp4) {
	AJob *j;
	pthread_t t;
	m->aj = 0;
#ifdef XRC_NO_AUDIO
	(void)mp4;
	return; /* built without audio: the movie on the wall clock, no audio thread */
#endif
	if (getenv("XRC_BINK_NO_AUDIO") || next_nth == -2) return;
	j = (AJob *)calloc(1, sizeof *j);
	j->mp4 = strdup(mp4);
	if (!j->mp4) { free(j); return; }
	j->nth = next_nth;
	pthread_mutex_init(&j->mu, 0);
	pthread_cond_init(&j->cv, 0);
	j->refs = 2;
	if (pthread_create(&t, 0, audio_thread, j) != 0) { j->refs = 1; aj_release(j); return; }
	pthread_detach(t);
	m->aj = j;
}
static void stop_audio(Movie *m) {
	AJob *j = m->aj;
	if (!j) return;
	m->aj = 0;
	if (__atomic_load_n(&j->ready, __ATOMIC_ACQUIRE) == 1) host_audio_abort(j->au); /* (a write waiting for the output) */
	pthread_mutex_lock(&j->mu);
	j->stop = 1;
	pthread_cond_signal(&j->cv);
	pthread_mutex_unlock(&j->mu);
	aj_release(j);
}
/* the audio output once open (0 while it is being opened, or without audio) */
static HostAudio *movie_au(Movie *m) {
	int r;
	if (!m->aj) return 0;
	r = __atomic_load_n(&m->aj->ready, __ATOMIC_ACQUIRE);
	if (r < 0) { stop_audio(m); return 0; } /* (none: the wall clock) */
	return r ? m->aj->au : 0;
}

/* 0: the MP4's video does not decode (nothing started) */
static int start_decoder(Movie *m, const char *mp4) {
	int k;
	m->vsrc = host_video_open(mp4, m->w, m->h); /* (the MP4 is in the game's own YUV: no conversion) */
	if (!m->vsrc) return 0;
	pthread_mutex_init(&m->m, 0);
	pthread_cond_init(&m->cv, 0);
	for (k = 0; k < RING; k++) m->ring[k] = (unsigned char *)malloc(fsize(m));
	m->rhead = m->rcount = m->eof = m->stop = 0;
	m->produced = 0;
	m->have_thread = pthread_create(&m->th, 0, reader, m) == 0;
	if (!m->have_thread) m->eof = 1;
	return 1;
}

static void stop_decoder(Movie *m) {
	int k;
	if (m->have_thread) {
		pthread_mutex_lock(&m->m);
		m->stop = 1;
		pthread_cond_broadcast(&m->cv);
		pthread_mutex_unlock(&m->m);
		pthread_join(m->th, 0);
		m->have_thread = 0;
	}
	host_video_close(m->vsrc);
	m->vsrc = 0;
	for (k = 0; k < RING; k++) { free(m->ring[k]); m->ring[k] = 0; }
	pthread_mutex_destroy(&m->m);
	pthread_cond_destroy(&m->cv);
}

/* frame f (1-based) of the movie into the game's frame buffer idx (black if the decoder has none) */
static unsigned buf_w(const Movie *m) { return (m->w + 31u) & ~31u; }
static unsigned buf_h(const Movie *m) { return (m->h + 31u) & ~31u; }
/* the padding rows below the picture (buf_h): its last row repeated (nothing dark bleeds in where it is filtered) */
static void pad_rows(Movie *m, u32 yb, u32 yp, u32 ub, u32 up, u32 vb, u32 vp) {
	unsigned w = m->w, h = m->h, y;
	for (y = h; y < buf_h(m); y++) memcpy(xrc_mem + yb + y * yp, xrc_mem + yb + (h - 1) * yp, w);
	for (y = h / 2; y < buf_h(m) / 2; y++) {
		memcpy(xrc_mem + ub + y * up, xrc_mem + ub + (h / 2 - 1) * up, w / 2);
		memcpy(xrc_mem + vb + y * vp, xrc_mem + vb + (h / 2 - 1) * vp, w / 2);
	}
}
/* where a plane's buffer pointer is in its 12-byte entry (the pitch is at +8 in both): FFXIII's Bink has
 * {Allocate, Buffer, Pitch}; LR's game writes the planes it allocates (0xcde790) at +0 and reads them back there
 * (0xcde410) */
#ifdef XRC_LR
#define PL_BUF 0x00u
#else
#define PL_BUF 0x04u
#endif
static void put_frame(Movie *m, unsigned f, int idx) {
	u32 fr = m->fb + 0x18 + 0x30 * (u32)idx;
	u32 yb = M32(fr + PL_BUF), yp = M32(fr + 0x08);               /* Y */
	u32 vb = M32(fr + 0x0c + PL_BUF), vp = M32(fr + 0x0c + 0x08); /* cR */
	u32 ub = M32(fr + 0x18 + PL_BUF), up = M32(fr + 0x18 + 0x08); /* cB */
	unsigned w = m->w, h = m->h, y;
	const unsigned char *src = 0;
	int slot = -1;
	{ /* XRC_BINK_TRACE: the first copies, with the buffers they go to, and how many find the buffers unset */
		static int tr = -1;
		static unsigned calls, unset;
		if (tr < 0) tr = xrc_tool_env("XRC_BINK_TRACE") != 0;
		calls++;
		if (!yb || !ub || !vb || yp < w || up < w / 2 || vp < w / 2) unset++;
		if (tr && (calls <= 12 || calls % 600 == 0))
			fprintf(stderr, "xrc: Bink trace: put #%u frame %u -> buffer %d (fb %08x: Y %08x/%u cR %08x/%u cB %08x/%u); %u unset so far\n",
			        calls, f, idx, m->fb, yb, yp, vb, vp, ub, up, unset);
	}
	if (!yb || !ub || !vb || yp < w || up < w / 2 || vp < w / 2) {
		if (!m->warned++) fprintf(stderr, "xrc: Bink: frame buffer %d is not set up (%08x/%u %08x/%u %08x/%u)\n", idx, yb, yp, ub, up, vb, vp);
		return;
	}
	double t0 = now_s(), t1;
	pthread_mutex_lock(&m->m);
	for (;;) {
		/* (frames before f: skipped) */
		while (m->rcount && m->ring_frame[m->rhead] < f) {
			m->rhead = (m->rhead + 1) % RING;
			m->rcount--;
			pthread_cond_broadcast(&m->cv);
		}
		if (m->rcount || m->eof) break;
		pthread_cond_wait(&m->cv, &m->m);
	}
	if (m->rcount && m->ring_frame[m->rhead] == f) { slot = m->rhead; src = m->ring[slot]; }
	pthread_mutex_unlock(&m->m);
	t1 = now_s();
	m->st_wait += t1 - t0;
	if (t1 - t0 > m->st_wait_max) m->st_wait_max = t1 - t0;
	if (src) {
		const unsigned char *u = src + w * h, *v = u + (w / 2) * (h / 2);
		for (y = 0; y < h; y++) memcpy(xrc_mem + yb + y * yp, src + y * w, w);
		for (y = 0; y < h / 2; y++) {
			memcpy(xrc_mem + ub + y * up, u + y * (w / 2), w / 2);
			memcpy(xrc_mem + vb + y * vp, v + y * (w / 2), w / 2);
		}
		pad_rows(m, yb, yp, ub, up, vb, vp);
		m->st_copy += now_s() - t1;
		pthread_mutex_lock(&m->m);
		m->rhead = (m->rhead + 1) % RING; /* (the slot is still the head: only this thread consumes) */
		m->rcount--;
		pthread_cond_broadcast(&m->cv);
		pthread_mutex_unlock(&m->m);
		/* XRC_BINK_DUMP=N: frame N as the game's buffers hold it -> bink_frameN.yuv (yuv420p w x h) */
		if (xrc_tool_env("XRC_BINK_DUMP") && (unsigned)atoi(xrc_tool_env("XRC_BINK_DUMP")) == f) {
			char nm[64];
			FILE *o;
			snprintf(nm, sizeof nm, "bink_frame%u.yuv", f);
			o = fopen(nm, "wb");
			if (o) {
				for (y = 0; y < h; y++) fwrite(xrc_mem + yb + y * yp, 1, w, o);
				for (y = 0; y < h / 2; y++) fwrite(xrc_mem + ub + y * up, 1, w / 2, o);
				for (y = 0; y < h / 2; y++) fwrite(xrc_mem + vb + y * vp, 1, w / 2, o);
				fclose(o);
				fprintf(stderr, "xrc: Bink: frame %u -> %s (%ux%u yuv420p)\n", f, nm, w, h);
			}
		}
	} else {
		if (!m->warned++) fprintf(stderr, "xrc: Bink: no decoded frame %u: black\n", f);
		for (y = 0; y < h; y++) memset(xrc_mem + yb + y * yp, 16, w);
		for (y = 0; y < h / 2; y++) { memset(xrc_mem + ub + y * up, 128, w / 2); memset(xrc_mem + vb + y * vp, 128, w / 2); }
		pad_rows(m, yb, yp, ub, up, vb, vp);
	}
}

/* ============================================================================================ the API */
static u32 io_open; /* BinkSetIO: the game's BINKIO open proc */

static Movie *mov_of(u32 g) {
	int k;
	for (k = 0; k < NMOV; k++) if (g && movs[k].g == g) return &movs[k];
	return 0;
}
#define FRAMENUM(m) GF(BinkHead, (m)->g, frame_num)
static double period(const Movie *m) { return (double)m->den / m->num; }
static double due(const Movie *m, unsigned f) { return (f ? f - 1 : 0) * period(m); }
/* the movie's clock: seconds since frame 1 was due. The audio's play position while it plays; the wall clock before,
 * without audio, and after it has played out (continuing from where it ended) */
static double movie_time(Movie *m) {
	HostAudio *au = movie_au(m);
	if (m->a_run && !m->a_started && !m->paused) { /* the playback began while the audio was being opened */
		if (au) { m->a_started = 1; host_audio_pause(au, 0); }
		else if (m->aj && now_s() - m->a_run_t < 0.5) { /* (held at the waiting frame until the audio can start with it) */
			m->t0 = now_s() - due(m, FRAMENUM(m));
			return due(m, FRAMENUM(m));
		} else { stop_audio(m); m->a_run = 0; } /* (too slow, or no audio: the wall clock) */
	}
	if (au && m->a_started && !m->a_done) {
		double t = host_audio_time(au);
		if (!(__atomic_load_n(&m->aj->eof, __ATOMIC_ACQUIRE) && host_audio_idle(au))) return t;
		m->a_done = 1; /* (the audio is over: the wall clock from here) */
		m->t0 = now_s() - t;
		if (m->paused) m->paused_at = now_s();
	}
	return (m->paused ? m->paused_at : now_s()) - m->t0;
}
/* the playback starts (or resumes) */
static void run_clock(Movie *m) {
	HostAudio *au = movie_au(m);
	if (m->a_done || !m->aj) return;
	if (au) { m->a_started = 1; host_audio_pause(au, 0); }
	else if (!m->a_started) { m->a_run = 1; m->a_run_t = now_s(); } /* (still being opened: started by movie_time) */
}
static void sync_head(Movie *m) {
	GF(BinkHead, m->g, width) = m->w;
	GF(BinkHead, m->g, height) = m->h;
	GF(BinkHead, m->g, frames) = m->frames;
	GF(BinkHead, m->g, frame_rate) = m->num;
	GF(BinkHead, m->g, frame_rate_div) = m->den;
}

static void hle_BinkSetMemory(XCtx *c) { RET(0, 2); }
static void hle_BinkSetSoundSystem(XCtx *c) { RET(0, 2); }
static void hle_BinkOpenXAudio2(XCtx *c) { RET(0, 1); }
static void hle_BinkSetSoundTrack(XCtx *c) { RETV(2); }
static void hle_BinkSetIO(XCtx *c) { io_open = A(0); RETV(1); }
static void hle_BinkSetIOSize(XCtx *c) { RETV(1); }
static void hle_RADTimerRead(XCtx *c) { RET((u32)(unsigned long long)(now_s() * 1000.0), 0); }
static int decorated_nargs(const char *n) { const char *at = strrchr(n, '@'); return at ? atoi(at + 1) / 4 : 0; }
static void hle_BinkAsyncThread(XCtx *c) { RET(1, decorated_nargs(xrc_cur_import)); } /* (per thread, not per movie) */

/* the movie in mp4, its Bink 2 header kb2 (frames and rate; the MP4 has its frames, cropped 736 -> 720 like
 * tools/bink2mp4): the BINK handle, 0 (the game skips the movie) */
static u32 open_mp4(const char *mp4_sd, const unsigned char *kb2) {
	FILE *f;
	Movie *m = 0;
	int k;
	/* an upscaled copy beside it, <name>.hd.mp4 (the launcher's Cinematics option makes them; XRC_MOVIE_HD=0: not
	 * used): played instead, at its own size when it is the movie's picture scaled up (the same aspect, up to 4096) --
	 * the game is given frame buffers that size and draws them full screen like the original */
	char hdp[1024];
	const char *mp4 = mp4_sd;
	unsigned vw = 0, vh = 0;
	size_t n = strlen(mp4_sd);
	if (!(getenv("XRC_MOVIE_HD") && !atoi(getenv("XRC_MOVIE_HD"))) && n > 4 && n + 4 < sizeof hdp && !strcmp(mp4_sd + n - 4, ".mp4")) {
		memcpy(hdp, mp4_sd, n - 4);
		strcpy(hdp + n - 4, ".hd.mp4");
		if ((f = fopen(hdp, "rb"))) { fclose(f); mp4 = hdp; }
	}
#define K32(o) ((u32)kb2[o] | (u32)kb2[(o) + 1] << 8 | (u32)kb2[(o) + 2] << 16 | (u32)kb2[(o) + 3] << 24)
	f = fopen(mp4, "rb");
	if (!f) {
		fprintf(stderr, "xrc: BinkOpen: no %s (not converted yet): fails, the game skips the movie\n", mp4);
		return 0;
	}
	fclose(f);
	for (k = 0; k < NMOV; k++) if (!movs[k].g) { m = &movs[k]; break; }
	if (!m) { fprintf(stderr, "xrc: BinkOpen: more than %d movies open: fails\n", NMOV); return 0; }
	{
		static u32 slots[NMOV];
		int i = (int)(m - movs);
		if (!slots[i]) slots[i] = xrc_alloc(0x400, 16); /* (BINK structs: kept for the run) */
		memset(m, 0, sizeof *m);
		m->g = slots[i];
	}
	m->frames = K32(8);
	m->w = K32(20);
	m->h = K32(24);
	m->num = K32(28);
	m->den = K32(32);
	if (m->h == 736) m->h = 720;
	if (!m->num || !m->den || !m->frames || m->w > 4096 || m->h > 4096) { fprintf(stderr, "xrc: BinkOpen: %s: bad header\n", mp4); m->g = 0; return 0; }
	if (mp4 == hdp) {
		if (host_video_size(mp4, &vw, &vh) && vw > m->w && vw <= 4096 && vh <= 4096 && !(vw & 1) && !(vh & 1) && (unsigned long long)vw * m->h == (unsigned long long)vh * m->w) {
			m->w = vw; m->h = vh;
		} else {
			fprintf(stderr, "xrc: BinkOpen: %s is %ux%u, not the %ux%u movie scaled up: the original is played\n", mp4, vw, vh, m->w, m->h);
			mp4 = mp4_sd;
		}
	}
	m->cur = -1; /* (no buffer decoded yet) */
	m->pending = -1;
	m->t0 = now_s();
	memset(xrc_mem + m->g, 0, 0x400);
#ifdef XRC_LR
	{ /* ColorSpace[16] at +0xd0, the YCrCb -> RGB the game's movie shader applies (0xcde53a passes it to 0xa994a0 as
	   * c2 / c1 / c3 / c0 of rgb = Y*c0 + Cb*c1 + Cr*c2 + c3): Cr's, Cb's coefficients, the offset, Y's -- BT.601
	   * limited range, as the decoder's planes are. Zeros (as they were) drew every movie black */
		static const float cs[16] = {
			1.596027f, -0.812968f, 0.0f, 0.0f,                         /* Cr */
			0.0f, -0.391762f, 2.017232f, 0.0f,                         /* Cb */
			-0.874202f, 0.531668f, -1.085631f, 0.0f,                   /* offset: -(16 Y + 128 Cr + 128 Cb) / 255 through the matrix */
			1.164384f, 1.164384f, 1.164384f, 0.0f};                    /* Y */
		memcpy(xrc_mem + m->g + 0xd0, cs, sizeof cs);
	}
#endif
	sync_head(m);
	FRAMENUM(m) = 1;
	if (!start_decoder(m, mp4)) {
		fprintf(stderr, "xrc: BinkOpen: %s does not decode: fails, the game skips the movie\n", mp4);
		m->g = 0;
		return 0;
	}
	start_audio(m, mp4);
	fprintf(stderr, "xrc: BinkOpen: %s, %ux%u, %u frames at %.3f fps, audio %s\n", mp4, m->w, m->h, m->frames, (double)m->num / m->den,
	        m->aj ? "opening (a thread)" : "none");
#undef K32
	return m->g;
}

/* BinkOpen(name, flags): which movie, from the file the game's reader opens */
static void hle_BinkOpen(XCtx *c) {
	u32 name = A(0), flags = A(1), sp = c->esp;
	char wmp[1024], mp4[1040];
	unsigned char hd[0x40];
	size_t n;
	FILE *f;
	if (!io_open) { fprintf(stderr, "xrc: BinkOpen without BinkSetIO: fails\n"); RET(0, 2); }
	/* the game's BINKIO open on a scratch BINKIO (0x120 bytes) below the stack, then its Close (slot 5) */
	xrc_last_open_path[0] = 0;
	c->esp = (c->esp - 0x200u) & ~15u;
	{
		u32 bio = c->esp, a[3];
		memset(xrc_mem + bio, 0, 0x120);
		c->esp -= 0x40;
		a[0] = bio; a[1] = name; a[2] = flags;
		if (xrc_call_guest(c, io_open, 3, a)) { a[0] = bio; xrc_call_guest(c, M32(bio + 20), 1, a); }
	}
	c->esp = sp;
	snprintf(wmp, sizeof wmp, "%s", xrc_last_open_path);
	n = strlen(wmp);
	if (n < 4 || (strcmp(wmp + n - 4, ".wmp") && strcmp(wmp + n - 4, ".WMP"))) {
		fprintf(stderr, "xrc: BinkOpen(\"%s\"): the game's reader opened no .wmp (%s): fails\n", S(name), wmp[0] ? wmp : "nothing");
		RET(0, 2);
	}
	snprintf(mp4, sizeof mp4, "%.*s.mp4", (int)(n - 4), wmp);
	{ /* the movies without an MP4: converted in the background from now on (once) */
		char dir[1024];
		char *sl;
		snprintf(dir, sizeof dir, "%s", wmp);
		sl = strrchr(dir, '/');
		if (!sl || (strrchr(dir, '\\') && strrchr(dir, '\\') > sl)) sl = strrchr(dir, '\\');
		if (sl) { *sl = 0; host_movie_convert_start(dir); }
	}
	/* the movie's own header: frames and rate (the MP4 has its frames, cropped 736 -> 720 like tools/bink2mp4) */
	f = fopen(wmp, "rb");
	n = f ? fread(hd, 1, sizeof hd, f) : 0;
	if (f) fclose(f);
#define H32(o) ((u32)hd[o] | (u32)hd[(o) + 1] << 8 | (u32)hd[(o) + 2] << 16 | (u32)hd[(o) + 3] << 24)
	if (n < 0x30 || memcmp(hd, "WMP", 3) || memcmp(hd + 16, "KB2", 3)) {
		fprintf(stderr, "xrc: BinkOpen: %s is no Bink 2 movie: fails\n", wmp);
		RET(0, 2);
	}
#ifdef XRC_SKIP_MOVIES
	fprintf(stderr, "xrc: BinkOpen: %s: skipped (built to skip the movies)\n", wmp);
	RET(0, 2);
#endif
	RET(open_mp4(mp4, hd + 16), 2);
#undef H32
}

static void hle_BinkClose(XCtx *c) {
	Movie *m = mov_of(A(0));
	if (m) { stop_audio(m); stop_decoder(m); m->g = 0; }
	RETV(1);
}

/* BinkGetRealtime(bink, BINKREALTIME *rt, flags) */
static void hle_BinkGetRealtime(XCtx *c) {
	Movie *m = mov_of(A(0));
	u32 rt = A(1);
	if (m && rt) {
		memset(xrc_mem + rt, 0, 0x38); /* BINKREALTIME: 14 dwords (FF13's is on its stack: not a byte more) */
		GF(BinkRealtime, rt, frame_num) = FRAMENUM(m);
		GF(BinkRealtime, rt, frame_rate) = m->num;
		GF(BinkRealtime, rt, frame_rate_div) = m->den;
		GF(BinkRealtime, rt, unk_0c) = 1;
		GF(BinkRealtime, rt, read_buffer_size) = 0x600000; /* ReadBufferSize */
		GF(BinkRealtime, rt, read_buffer_used) = 0x600000; /* ReadBufferUsed: full */
	}
	RETV(3);
}

static void hle_BinkGetFrameBuffersInfo(XCtx *c) {
	Movie *m = mov_of(A(0));
	u32 fb = A(1);
	if (m && fb) {
		memset(xrc_mem + fb, 0, 0x18 + 2 * 0x30);
		GF(BinkFrameBuffers, fb, total_frames) = 2;          /* TotalFrames */
		/* YABufferWidth / Height, cRcBBufferWidth / Height: the movie's size padded to 32 as Bink's own buffers are
		 * (the movies are 1280x736 Bink files, 1280x720 pictures: buffers of 1280x736). Not 1280x720: the game
		 * takes a 1280x720 surface for a full-screen one and makes it the screen's size (0xeb4090 / 0xec1e90) -- at
		 * another resolution (-Width= / -Height=) the movie's Y texture became e.g. 1920x1080 with the 1280x720
		 * picture in its corner, the cR / cB ones not */
		GF(BinkFrameBuffers, fb, y_buffer_width) = buf_w(m);
		GF(BinkFrameBuffers, fb, y_buffer_height) = buf_h(m);
		GF(BinkFrameBuffers, fb, cr_cb_buffer_width) = buf_w(m) / 2;
		GF(BinkFrameBuffers, fb, cr_cb_buffer_height) = buf_h(m) / 2;
		{ /* Allocate: the Y, cR and cB planes of both buffer sets are used (no alpha). LR allocates only the planes
		   * marked so (FFXIII allocates them all anyway); BINKFRAMEPLANESET = 4 planes of {Allocate, Buffer, Pitch} */
			u32 k, set;
#ifndef XRC_LR /* (LR: +0 is the buffer pointer, which the game fills in itself for every plane) */
			for (set = 0; set < 2; set++)
				for (k = 0; k < 3; k++) M32(fb + 0x18 + 0x30 * set + 0x0c * k) = 1;
#else
			(void)set; (void)k;
#endif
		}
		GF(BinkFrameBuffers, fb, frame_num) = 0xffffffffu; /* FrameNum: the buffer shown; none until a frame is decoded (the game draws
		                               * nothing while it is negative: 0x1008aa3 / 0x1008d1b. 0 drew buffer 0 before
		                               * anything was in it: a green flash) */
	}
	RETV(2);
}
static void hle_BinkRegisterFrameBuffers(XCtx *c) {
	Movie *m = mov_of(A(0));
	if (m) m->fb = A(1);
	if (m && xrc_tool_env("XRC_BINK_TRACE"))
		fprintf(stderr, "xrc: Bink trace: register fb %08x: Y %08x/%u cR %08x/%u cB %08x/%u\n", A(1), M32(A(1) + 0x18 + PL_BUF), M32(A(1) + 0x20),
		        M32(A(1) + 0x24 + PL_BUF), M32(A(1) + 0x2c), M32(A(1) + 0x30 + PL_BUF), M32(A(1) + 0x38));
	RETV(2);
}
/* BinkDoFrameAsync(bink, thread, second thread): decoded at once (the frame is read ahead) */
static void hle_BinkDoFrameAsync(XCtx *c) {
	Movie *m = mov_of(A(0));
	if (m && m->fb) {
		int total = (int)GF(BinkFrameBuffers, m->fb, total_frames);
		m->pending = m->cur < 0 ? 0 : (m->cur + 1) % (total > 0 ? total : 1);
		put_frame(m, FRAMENUM(m), m->pending);
	}
	RET(1, 3);
}
static void hle_BinkDoFrameAsyncWait(XCtx *c) {
	Movie *m = mov_of(A(0));
	if (m && m->pending >= 0) {
		m->cur = m->pending;
		m->pending = -1;
		if (m->fb) GF(BinkFrameBuffers, m->fb, frame_num) = (u32)m->cur;
	}
	RET(1, 2);
}
static void hle_BinkNextFrame(XCtx *c) {
	Movie *m = mov_of(A(0));
	if (stats < 0) stats = xrc_tool_env("XRC_BINK_STATS") != 0;
	if (m && stats && !m->paused) {
		double t = now_s();
		if (m->st_last > 0 && t - m->st_last > m->st_gap_max) m->st_gap_max = t - m->st_last;
		if (movie_time(m) > due(m, FRAMENUM(m)) + period(m) * 0.5) m->st_late++;
		m->st_last = t;
		if (++m->st_shown % 300 == 0) {
			fprintf(stderr, "xrc: Bink frame %u: 300 shown, %u skipped, %u over half a frame late; decoder wait %.1f ms (max %.1f), copy %.1f ms; longest gap %.1f ms; clock %.3f s (frame due %.3f, audio %.3f, wall %.3f)\n",
			        FRAMENUM(m), m->st_skips, m->st_late, m->st_wait * 1e3, m->st_wait_max * 1e3, m->st_copy * 1e3, m->st_gap_max * 1e3,
			        movie_time(m), due(m, FRAMENUM(m)), movie_au(m) ? host_audio_time(movie_au(m)) : -1.0, now_s() - m->t0);
			m->st_skips = m->st_late = 0;
			m->st_wait = m->st_wait_max = m->st_copy = m->st_gap_max = 0;
		}
	}
#ifdef XRC_LR
	if (m && FRAMENUM(m) % 150 == 1 && xrc_tool_env("XRC_BINK_TRACE")) { /* the caller's esi is the movie player ([100] is
		                                                              * reset at the start of its update: -1 here) */
		u32 p = c->esi;
		fprintf(stderr, "xrc: Bink trace: frame %u, player %08x: c4 %u c8 %u d0 %u d1 %u [100] %08x [c0] %08x [10c] %u tex %08x %08x %08x; FrameNum %d cur %d pending %d\n",
		        FRAMENUM(m), p, xrc_mem[p + 0xc4], xrc_mem[p + 0xc8], xrc_mem[p + 0xd0], xrc_mem[p + 0xd1], M32(p + 0x100), M32(p + 0xc0),
		        M32(p + 0x10c), M32(p + 0x118), M32(p + 0x120), M32(p + 0x128), m->fb ? (int)GF(BinkFrameBuffers, m->fb, frame_num) : -9, m->cur, m->pending);
	}
#endif
	if (m && FRAMENUM(m) < m->frames) FRAMENUM(m)++;
	RETV(1);
}
static void hle_BinkPause(XCtx *c) {
	Movie *m = mov_of(A(0));
	u32 p = A(1);
	if (m) {
		double t = now_s();
		if (p && !m->paused) {
			m->paused = 1;
			m->paused_at = t;
			if (movie_au(m) && m->a_started && !m->a_done) host_audio_pause(movie_au(m), 1);
		} else if (!p && m->paused) {
			m->paused = 0;
			/* wall clock: the frame waiting to be shown is due now (with audio: where the audio is) */
			m->t0 = t - due(m, FRAMENUM(m));
			run_clock(m);
		}
	}
	RET(p, 2);
}
/* 0: the next frame (FrameNum) is due */
static void hle_BinkWait(XCtx *c) {
	Movie *m = mov_of(A(0));
	if (!m) RET(0, 1);
	RET(m->paused || movie_time(m) < due(m, FRAMENUM(m)), 1);
}
static void hle_BinkShouldSkip(XCtx *c) {
	Movie *m = mov_of(A(0));
	if (!m || m->paused) RET(0, 1);
	if (movie_time(m) > due(m, FRAMENUM(m)) + period(m)) { m->st_skips++; RET(1, 1); }
	RET(0, 1);
}
static void hle_BinkSetSpeakerVolumes(XCtx *c) { RETV(5); }
/* Lightning Returns: BinkOpenWithOptions(name, options, flags 0x02204400: BINKIOPROCESSOR and others). The name encodes
 * the game's archive object and the movie's hash; the options carry the BINKIO open proc (+0x20) that reads it out of
 * weiss_data\movie\movie*.win32.bin. That reader, on a scratch BINKIO, returns the stream's first 0x40 bytes (its
 * ReadHeader, slot 0; then its Close, slot 5); the stream with those bytes in the archives (host_movie_convert.c's
 * index) is the movie, and its MP4 (converted in the background) plays as FFXIII's do. */
#ifdef XRC_LR
static void hle_BinkOpenWithOptions(XCtx *c) {
	u32 name = A(0), opts = A(1), flags = A(2), sp = c->esp, open = 0, got = 0;
	unsigned char hd[0x40];
	char dir[1024], mp4[1200];
	const LrMovie *ix;
	int n, i;
	if (opts && (flags & 0x02000000u)) open = M32(opts + 0x20);
	if (!open) { fprintf(stderr, "xrc: BinkOpenWithOptions(flags %08x): no BINKIO open proc: fails\n", flags); RET(0, 3); }
	c->esp = (c->esp - 0x400u) & ~15u;
	{
		u32 bio = c->esp, dst = bio + 0x200, a[4];
		memset(xrc_mem + bio, 0, 0x300);
		c->esp -= 0x40;
		a[0] = bio; a[1] = name; a[2] = flags;
		if (xrc_call_guest(c, open, 3, a)) {
			a[0] = bio; a[1] = 0; a[2] = dst; a[3] = sizeof hd;
			got = xrc_call_guest(c, M32(bio + 0), 4, a);
			memcpy(hd, xrc_mem + dst, sizeof hd);
			a[0] = bio;
			xrc_call_guest(c, M32(bio + 20), 1, a);
		}
	}
	c->esp = sp;
	if (got != sizeof hd || memcmp(hd, "KB2", 3)) {
		fprintf(stderr, "xrc: BinkOpenWithOptions(\"%.24s\"): the game's reader returned no Bink 2 header (%u bytes): fails\n", S(name), got);
		RET(0, 3);
	}
	if (!xrc_host_path(XRC_GUEST_EXE_DIR "\\weiss_data\\movie", dir, sizeof dir)) RET(0, 3);
	n = host_movie_lr_index(dir, &ix);
	for (i = 0; i < n; i++) if (!memcmp(ix[i].hd, hd, sizeof hd)) break;
	if (i == n) { fprintf(stderr, "xrc: BinkOpenWithOptions(\"%.24s\"): no stream in %s has its header: fails\n", S(name), dir); RET(0, 3); }
	host_movie_lr_mp4(i, mp4, sizeof mp4);
	fprintf(stderr, "xrc: BinkOpenWithOptions(\"%.24s\"): %s.win32.bin @%llx (%lld bytes)\n", S(name), ix[i].arc,
	        (unsigned long long)ix[i].off, ix[i].size);
	host_movie_convert_start_lr(i);
	{
		FILE *f = fopen(mp4, "rb");
		if (!f) { host_movie_convert_want(i); fprintf(stderr, "xrc: BinkOpenWithOptions: no %s yet (converting): the game skips the movie\n", mp4); RET(0, 3); }
		fclose(f);
	}
#ifdef XRC_SKIP_MOVIES
	RET(0, 3);
#endif
	{ /* the audio: the set of 4 tracks the options name (their first / 4), none without tracks */
		u32 nt = M32(opts + 0x18), tn = M32(opts + 0x1c), g;
		next_nth = nt && tn ? (int)(M32(tn) / 4) : -2;
		g = open_mp4(mp4, hd);
		next_nth = -1;
		RET(g, 3);
	}
}
#else
static void hle_BinkOpenWithOptions(XCtx *c) { fprintf(stderr, "xrc: BinkOpenWithOptions: not expected here: fails\n"); RET(0, 3); }
#endif
static void hle_BinkSetVolume(XCtx *c) { RETV(3); }
static void hle_BinkGoto(XCtx *c) { RETV(3); }
static void hle_BinkSetWillLoop(XCtx *c) { RET(0, 2); }
static void hle_BinkFreeGlobals(XCtx *c) { RETV(0); }

#define E(n, a) {"bink2w32.dll", "_" #n "@" #a, hle_##n}
const XrcImport xrc_hle_bink[] = {
	E(BinkSetMemory, 8), E(BinkSetSoundSystem, 8), E(BinkOpenXAudio2, 4), E(BinkSetSoundTrack, 8), E(BinkSetIO, 4),
	E(BinkSetIOSize, 4), E(BinkOpen, 8), E(BinkClose, 4), E(RADTimerRead, 0), E(BinkGetRealtime, 12),
	E(BinkGetFrameBuffersInfo, 8), E(BinkRegisterFrameBuffers, 8), E(BinkDoFrameAsync, 12), E(BinkDoFrameAsyncWait, 8),
	E(BinkNextFrame, 4), E(BinkPause, 8), E(BinkWait, 4), E(BinkShouldSkip, 4), E(BinkSetSpeakerVolumes, 20),
	E(BinkOpenWithOptions, 12), E(BinkSetVolume, 12), E(BinkGoto, 12), E(BinkSetWillLoop, 8), E(BinkFreeGlobals, 0),
	{"bink2w32.dll", "_BinkStartAsyncThread@8", hle_BinkAsyncThread},
	{"bink2w32.dll", "_BinkRequestStopAsyncThread@4", hle_BinkAsyncThread},
	{"bink2w32.dll", "_BinkWaitStopAsyncThread@4", hle_BinkAsyncThread},
};
const unsigned xrc_hle_bink_n = sizeof xrc_hle_bink / sizeof xrc_hle_bink[0];
