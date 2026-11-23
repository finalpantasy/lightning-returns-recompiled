/* host_audio.c - a PCM stream to the host's audio output (16-bit interleaved, any rate / 1, 2 or 6 channels), with
 * its own play position for A/V sync. Used by the Bink HLE (movie audio) until the platform layer mixes XAudio2.
 *   Windows: waveOut (always present; 5.1 through WAVEFORMATEXTENSIBLE, Windows downmixes to the speakers there are).
 *   elsewhere: SDL2's audio queue (libSDL2 loaded at run time; SDL converts to the device's channels and rate).
 *     XRC_NO_AUDIO=1: none (host_audio_open returns 0: callers keep time without audio).
 * host_audio_write blocks until the data is queued (a feeder thread's call); the others may be called from any
 * thread. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_audio.h"

/* every open output (host_audio_silence_all), under a spin lock (opens and closes are rare) */
#define NREG 16
static HostAudio *reg[NREG];
static volatile int reg_busy, silenced;
static void reg_lock(void) { while (__sync_lock_test_and_set(&reg_busy, 1)) {} }
static void reg_unlock(void) { __sync_lock_release(&reg_busy); }
static void reg_set(HostAudio *from, HostAudio *to) {
	int k;
	reg_lock();
	for (k = 0; k < NREG; k++) if (reg[k] == from) { reg[k] = to; break; }
	reg_unlock();
}
static void out_silence(HostAudio *a); /* (per platform: paused, aborted) */
void host_audio_silence_all(void) {
	int k;
	reg_lock();
	silenced = 1;
	for (k = 0; k < NREG; k++) if (reg[k]) out_silence(reg[k]);
	reg_unlock();
}

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>

#define NBUF 64 /* (the XAudio2 mixer writes 10 ms a pass and keeps up to 500 ms queued) */
#define BUF_MS 40
struct HostAudio {
	HWAVEOUT wo;
	HANDLE ev;
	WAVEHDR hdr[NBUF];
	char *mem[NBUF];
	unsigned bufsize, block, rate;
	volatile LONG stop;
};

HostAudio *host_audio_open(unsigned rate, unsigned channels) {
#ifdef XRC_NO_AUDIO
	(void)rate; (void)channels;
	return 0; /* built without audio (-DXRC_NO_AUDIO) */
#endif
	static const GUID pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
	WAVEFORMATEXTENSIBLE f;
	HostAudio *a;
	int k;
	memset(&f, 0, sizeof f);
	f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	f.Format.nChannels = (WORD)channels;
	f.Format.nSamplesPerSec = rate;
	f.Format.wBitsPerSample = 16;
	f.Format.nBlockAlign = (WORD)(channels * 2);
	f.Format.nAvgBytesPerSec = rate * channels * 2;
	f.Format.cbSize = 22;
	f.Samples.wValidBitsPerSample = 16;
	f.dwChannelMask = channels == 6 ? 0x3f /* FL FR FC LFE BL BR */ : channels == 2 ? 0x3 : 0x4;
	f.SubFormat = pcm;
	if (silenced || getenv("XRC_NO_AUDIO")) return 0; /* XRC_NO_AUDIO=1: muted (callers keep time without audio) */
	a = (HostAudio *)calloc(1, sizeof *a);
	a->ev = CreateEventA(0, FALSE, FALSE, 0);
	if (waveOutOpen(&a->wo, WAVE_MAPPER, &f.Format, (DWORD_PTR)a->ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
		CloseHandle(a->ev);
		free(a);
		return 0;
	}
	a->rate = rate;
	a->block = channels * 2;
	a->bufsize = rate * BUF_MS / 1000 * a->block;
	for (k = 0; k < NBUF; k++) {
		a->mem[k] = (char *)malloc(a->bufsize);
		a->hdr[k].lpData = a->mem[k];
		a->hdr[k].dwBufferLength = a->bufsize;
		waveOutPrepareHeader(a->wo, &a->hdr[k], sizeof a->hdr[k]);
		a->hdr[k].dwFlags |= WHDR_DONE; /* (free) */
	}
	reg_set(0, a);
	return a;
}

int host_audio_write(HostAudio *a, const void *pcm, unsigned bytes) {
	const char *p = (const char *)pcm;
	while (bytes && !a->stop) {
		int k, got = -1;
		for (k = 0; k < NBUF; k++) if (a->hdr[k].dwFlags & WHDR_DONE) { got = k; break; }
		if (got < 0) { WaitForSingleObject(a->ev, 100); continue; }
		{
			unsigned n = bytes < a->bufsize ? bytes : a->bufsize;
			n -= n % a->block;
			if (!n) break;
			memcpy(a->mem[got], p, n);
			a->hdr[got].dwBufferLength = n;
			a->hdr[got].dwFlags &= ~(DWORD)WHDR_DONE;
			waveOutWrite(a->wo, &a->hdr[got], sizeof a->hdr[got]);
			p += n;
			bytes -= n;
		}
	}
	return !a->stop;
}

void host_audio_pause(HostAudio *a, int pause) {
	if (pause) waveOutPause(a->wo); else waveOutRestart(a->wo);
}
/* the queued audio: every buffer not yet played back (the one playing counted whole) */
unsigned host_audio_queued_ms(HostAudio *a) {
	unsigned long long b = 0;
	int k;
	for (k = 0; k < NBUF; k++) if (!(a->hdr[k].dwFlags & WHDR_DONE)) b += a->hdr[k].dwBufferLength;
	return (unsigned)(b * 1000ull / ((unsigned long long)a->rate * a->block));
}
/* never waits (the XAudio2 mixer calls it holding the guest lock): over max_ms queued, or no free buffer, the rest
 * is dropped. (It used to be host_audio_write, and queued_ms was always 0: each 10 ms pass took the "underrun"
 * path and wrote 100-400 ms of silence, blocking with the lock held -- clicks, and 2-6 fps for every thread.) */
int host_audio_try_write(HostAudio *a, const void *pcm, unsigned bytes, unsigned max_ms) {
	const char *p = (const char *)pcm;
	if (a->stop) return 0;
	if (host_audio_queued_ms(a) > max_ms) return 1;
	while (bytes) {
		int k, got = -1;
		for (k = 0; k < NBUF; k++) if (a->hdr[k].dwFlags & WHDR_DONE) { got = k; break; }
		if (got < 0) break;
		{
			unsigned n = bytes < a->bufsize ? bytes : a->bufsize;
			n -= n % a->block;
			if (!n) break;
			memcpy(a->mem[got], p, n);
			a->hdr[got].dwBufferLength = n;
			a->hdr[got].dwFlags &= ~(DWORD)WHDR_DONE;
			waveOutWrite(a->wo, &a->hdr[got], sizeof a->hdr[got]);
			p += n;
			bytes -= n;
		}
	}
	return 1;
}

double host_audio_time(HostAudio *a) {
	MMTIME t;
	t.wType = TIME_SAMPLES;
	if (waveOutGetPosition(a->wo, &t, sizeof t) != MMSYSERR_NOERROR || t.wType != TIME_SAMPLES) return 0;
	return (double)t.u.sample / a->rate;
}

int host_audio_idle(HostAudio *a) {
	int k;
	for (k = 0; k < NBUF; k++) if (!(a->hdr[k].dwFlags & WHDR_DONE)) return 0;
	return 1;
}

void host_audio_abort(HostAudio *a) {
	InterlockedExchange(&a->stop, 1);
	SetEvent(a->ev);
}

static void out_silence(HostAudio *a) {
	waveOutPause(a->wo);
	InterlockedExchange(&a->stop, 1);
	SetEvent(a->ev);
}

void host_audio_close(HostAudio *a) {
	int k;
	if (!a) return;
	reg_set(a, 0);
	a->stop = 1;
	waveOutReset(a->wo); /* (returns every queued buffer) */
	for (k = 0; k < NBUF; k++) {
		waveOutUnprepareHeader(a->wo, &a->hdr[k], sizeof a->hdr[k]);
		free(a->mem[k]);
	}
	waveOutClose(a->wo);
	CloseHandle(a->ev);
	free(a);
}

#else /* elsewhere: SDL2's audio queue (libSDL2-2.0.so.0, loaded at run time like the window: no headers needed) */
#include <dlfcn.h>
#include <time.h>
typedef struct { int freq; unsigned short format; unsigned char channels, silence; unsigned short samples, padding; unsigned size; void (*callback)(void *, unsigned char *, int); void *userdata; } SdlSpec;
static int (*InitSubSystem_)(unsigned);
static unsigned (*OpenAudioDevice_)(const char *, int, const SdlSpec *, SdlSpec *, int);
static int (*QueueAudio_)(unsigned, const void *, unsigned);
static unsigned (*GetQueuedAudioSize_)(unsigned);
static void (*PauseAudioDevice_)(unsigned, int);
static void (*CloseAudioDevice_)(unsigned);
static const char *(*GetError_)(void);
static int sdl_audio(void) {
	static int state; /* 0 not tried, 1 ok, -1 none */
	void *h;
	if (state) return state > 0;
	state = -1;
	if (getenv("XRC_NO_AUDIO")) return 0;
	setenv("SDL_NO_SIGNAL_HANDLERS", "1", 0); /* (SIGINT / SIGTERM stay ours: SDL would make them an event) */
	if (!(h = dlopen("libSDL2-2.0.so.0", RTLD_NOW | RTLD_GLOBAL))) return 0;
#define SYM(n) if (!(*(void **)&n##_ = dlsym(h, "SDL_" #n))) return 0;
	SYM(InitSubSystem) SYM(OpenAudioDevice) SYM(QueueAudio) SYM(GetQueuedAudioSize) SYM(PauseAudioDevice) SYM(CloseAudioDevice) SYM(GetError)
#undef SYM
	if (InitSubSystem_(0x10) != 0) { fprintf(stderr, "xrc: audio: no SDL audio (%s)\n", GetError_()); return 0; } /* SDL_INIT_AUDIO */
	state = 1;
	return 1;
}
struct HostAudio {
	unsigned dev, rate, block;
	unsigned long long queued; /* bytes queued in all */
	volatile int stop;
};
#define MAX_QUEUED_MS 200
HostAudio *host_audio_open(unsigned rate, unsigned channels) {
#ifdef XRC_NO_AUDIO
	(void)rate; (void)channels;
	return 0; /* built without audio (-DXRC_NO_AUDIO) */
#endif
	SdlSpec want, have;
	HostAudio *a;
	if (silenced || !sdl_audio()) return 0;
	memset(&want, 0, sizeof want);
	want.freq = (int)rate;
	want.format = 0x8010; /* AUDIO_S16LSB */
	want.channels = (unsigned char)channels;
	want.samples = 1024;
	a = (HostAudio *)calloc(1, sizeof *a);
	a->dev = OpenAudioDevice_(0, 0, &want, &have, 0); /* (0: SDL converts to the device's own format and channels) */
	if (!a->dev) { fprintf(stderr, "xrc: audio: no output (%s)\n", GetError_()); free(a); return 0; }
	a->rate = rate;
	a->block = channels * 2;
	PauseAudioDevice_(a->dev, 0);
	reg_set(0, a);
	return a;
}
static unsigned queued_ms(HostAudio *a) { return (unsigned)((unsigned long long)GetQueuedAudioSize_(a->dev) * 1000ull / ((unsigned long long)a->rate * a->block)); }
int host_audio_write(HostAudio *a, const void *pcm, unsigned bytes) {
	while (!a->stop && queued_ms(a) > MAX_QUEUED_MS) { struct timespec ts = {0, 5000000}; nanosleep(&ts, 0); }
	if (a->stop) return 0;
	bytes -= bytes % a->block;
	if (bytes && QueueAudio_(a->dev, pcm, bytes) == 0) a->queued += bytes;
	return 1;
}
int host_audio_try_write(HostAudio *a, const void *pcm, unsigned bytes, unsigned max_ms) {
	if (a->stop || queued_ms(a) > max_ms) return 0;
	bytes -= bytes % a->block;
	if (bytes && QueueAudio_(a->dev, pcm, bytes) == 0) a->queued += bytes;
	return 1;
}
unsigned host_audio_queued_ms(HostAudio *a) { return queued_ms(a); }
void host_audio_pause(HostAudio *a, int pause) { PauseAudioDevice_(a->dev, pause); }
double host_audio_time(HostAudio *a) {
	unsigned long long q = GetQueuedAudioSize_(a->dev);
	return q > a->queued ? 0 : (double)(a->queued - q) / ((double)a->rate * a->block);
}
int host_audio_idle(HostAudio *a) { return GetQueuedAudioSize_(a->dev) == 0; }
void host_audio_abort(HostAudio *a) { a->stop = 1; }
static void out_silence(HostAudio *a) { PauseAudioDevice_(a->dev, 1); a->stop = 1; }
void host_audio_close(HostAudio *a) {
	if (!a) return;
	reg_set(a, 0);
	a->stop = 1;
	CloseAudioDevice_(a->dev);
	free(a);
}
#endif
