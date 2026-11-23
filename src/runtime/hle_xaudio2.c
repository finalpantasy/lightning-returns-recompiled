/* hle_xaudio2.c -- XAudio2 2.7 (the DirectX June 2010 runtime the game creates with CoCreateInstance), silent.
 *
 * The engine keeps time without making sound: a started source voice consumes its queued buffers at its sample rate
 * (times its frequency ratio), and the game's callbacks run as XAudio2 runs them -- from the engine's own thread, a
 * guest thread whose start routine is the HLE loop below, once per 10 ms pass:
 *   engine OnProcessingPassStart; per started source voice OnVoiceProcessingPassStart(bytes required),
 *   OnBufferStart / OnLoopEnd / OnBufferEnd / OnStreamEnd as its buffers play, OnVoiceProcessingPassEnd;
 *   engine OnProcessingPassEnd.
 * Streaming code that refills from OnBufferEnd or waits for BuffersQueued to drop therefore moves on. Operation sets
 * are applied at once (CommitChanges has nothing left to do). Effects (CLSID_AudioReverb objects in effect chains)
 * are accepted and do nothing.
 * The mixer (every target but the reference one, whose real XAudio2 plays): each pass, the samples a source voice
 * consumes are decoded (PCM 8 / 16 bit, float, MS-ADPCM), resampled (linear) to its destinations' rate and mixed into
 * them with its volume, channel volumes and output matrix (XAudio2's default matrices when not set); submix voices then
 * mix into theirs in processing-stage order, and the mastering voice goes to the host output (host_audio.c). Not
 * rendered: effects (reverb), filters. XRC_NO_AUDIO=1: silent (the engine still keeps time); XRC_XA2_DUMP=FILE: the
 * mix also as raw 16-bit PCM; XRC_XA2_LOG=1: the source formats and output underruns.
 *
 * Objects are {vtable, index} in guest memory; vtable slots are imports "IXAudio2::Method", "IXAudio2Voice::Method"
 * (the methods all voices share) and "IXAudio2SourceVoice::Method" (xrc_import_slot). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "xrc_rt.h"
#include "host_audio.h"
#include "guest_types.h"

#define A(i) xrc_arg(c, (i))
#define RET(v, n) do { c->eax = (u32)(v); xrc_stdcall_ret(c, (n)); return; } while (0)
#define DLL "XAudio2_7.dll"

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_INVALIDARG 0x80070057u
#define XAUDIO2_E_INVALID_CALL 0x88960001u

static unsigned long long now_us(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000ull + (unsigned long long)ts.tv_nsec / 1000ull;
}

/* ============================================================================================ vtables */
#define I_XA2(X) X(QueryInterface, 3) X(AddRef, 1) X(Release, 1) X(GetDeviceCount, 2) X(GetDeviceDetails, 3) \
	X(Initialize, 3) X(RegisterForCallbacks, 2) X(UnregisterForCallbacks, 2) X(CreateSourceVoice, 8) \
	X(CreateSubmixVoice, 8) X(CreateMasteringVoice, 7) X(StartEngine, 1) X(StopEngine, 1) X(CommitChanges, 2) \
	X(GetPerformanceData, 2) X(SetDebugConfiguration, 3)
#define I_VOICE(X) X(GetVoiceDetails, 2) X(SetOutputVoices, 2) X(SetEffectChain, 2) X(EnableEffect, 3) \
	X(DisableEffect, 3) X(GetEffectState, 3) X(SetEffectParameters, 5) X(GetEffectParameters, 4) \
	X(SetFilterParameters, 3) X(GetFilterParameters, 2) X(SetOutputFilterParameters, 4) \
	X(GetOutputFilterParameters, 3) X(SetVolume, 3) X(GetVolume, 2) X(SetChannelVolumes, 4) \
	X(GetChannelVolumes, 3) X(SetOutputMatrix, 6) X(GetOutputMatrix, 5) X(DestroyVoice, 1)
#define I_SRC(X) X(Start, 3) X(Stop, 3) X(SubmitSourceBuffer, 3) X(FlushSourceBuffers, 1) X(Discontinuity, 1) \
	X(ExitLoop, 2) X(GetState, 2) X(SetFrequencyRatio, 3) X(GetFrequencyRatio, 2) X(SetSourceSampleRate, 2)
#define I_UNK(X) X(QueryInterface, 3) X(AddRef, 1) X(Release, 1)
#define NAME(m, a) #m,
static const char *const xa2_m[] = {I_XA2(NAME)};
static const char *const voice_m[] = {I_VOICE(NAME)};
static const char *const src_m[] = {I_SRC(NAME)};
static const char *const unk_m[] = {I_UNK(NAME)};
#define N_XA2 (int)(sizeof xa2_m / sizeof xa2_m[0])
#define N_VOICE (int)(sizeof voice_m / sizeof voice_m[0])
#define N_SRC (int)(sizeof src_m / sizeof src_m[0])
#define N_UNK (int)(sizeof unk_m / sizeof unk_m[0])

static void fill(u32 vt, int at, const char *iface, const char *const *m, int n) {
	char nm[96];
	int k;
	for (k = 0; k < n; k++) {
		snprintf(nm, sizeof nm, "%s::%s", iface, m[k]);
		M32(vt + 4u * (u32)(at + k)) = xrc_import_slot(DLL, nm);
	}
}
static u32 vt_xa2, vt_src, vt_voice, vt_reverb;
static void make_vtables(void) {
	if (vt_xa2) return;
	vt_xa2 = xrc_heap_alloc(4u * N_XA2, 1);
	fill(vt_xa2, 0, "IXAudio2", xa2_m, N_XA2);
	vt_voice = xrc_heap_alloc(4u * N_VOICE, 1); /* submix and mastering voices */
	fill(vt_voice, 0, "IXAudio2Voice", voice_m, N_VOICE);
	vt_src = xrc_heap_alloc(4u * (N_VOICE + N_SRC), 1);
	fill(vt_src, 0, "IXAudio2Voice", voice_m, N_VOICE);
	fill(vt_src, N_VOICE, "IXAudio2SourceVoice", src_m, N_SRC);
	vt_reverb = xrc_heap_alloc(4u * N_UNK, 1);
	fill(vt_reverb, 0, "XAPOReverb", unk_m, N_UNK);
}

/* ============================================================================================ state */
enum { V_SOURCE = 1, V_SUBMIX, V_MASTER };
typedef struct { u32 flags, bytes, data, play_begin, play_len, loop_begin, loop_len, loop_count, ctx; } Buf; /* XAUDIO2_BUFFER */
#define MAXQ 64 /* XAUDIO2_MAX_QUEUED_BUFFERS */
typedef struct {
	int kind, alive, started;
	u32 obj, cb, flags, channels, rate, block, tag, spb; /* spb: samples per block (ADPCM), else 1 */
	u32 avg_bytes;
	float volume, ratio, max_ratio;
	Buf q[MAXQ];
	int qhead, qn;
	/* the buffer at the queue head: its play position (samples from its start), whether OnBufferStart ran, loops
	 * left (XAUDIO2_LOOP_INFINITE = 255) */
	double pos;
	int head_started, loops_left;
	unsigned long long played; /* SamplesPlayed */
	u32 flushed[MAXQ]; /* contexts of flushed buffers, ended (OnBufferEnd) on the next pass */
	int nflushed;
	unsigned long long removed; /* buffers taken off the queue (ended or flushed) */
	/* routing: sends (nsend -1: the default, the mastering voice), output matrices per send (has_mat), channel volumes,
	 * processing stage (submix) */
	int nsend;
	u32 send[8];
	float mat[8][64];
	u8 has_mat[8];
	float chvol[8];
	u32 stage, bits;
	/* the mixer: a source's resampling position (in its head buffer's samples) and output frame this pass; a submix /
	 * mastering voice's input for this pass (in_n frames of channels floats) */
	double acc;
	int oi, mix_cnt;
	unsigned st_passes, st_starved, st_submits; unsigned long long st_bytes, st_qn, st_played; unsigned st_flush, st_stop, st_start, st_ends, st_getstate; /* (XRC_XA2_LOG) */
	float *in;
	int in_n, in_cap;
	double in_carry;
	u32 cache_key[2];          /* ADPCM: the decoded blocks (data address + block index + 1) */
	short *cache[2];
	int cache_next;
} Voice;
#define MAXVOICES 4096
static Voice voices[MAXVOICES];
static int nvoices;
static u32 engine_obj, engine_refs = 1, engine_cbs[16];
static int nengine_cbs, engine_running, engine_thread;

static Voice *voice_of(u32 obj) {
	u32 i = GF(XaObject, obj, id);
	if (i >= (u32)nvoices || voices[i].obj != obj || !voices[i].alive) xrc_fatal(0, "xaudio2: %08x is not a live voice", obj);
	return &voices[i];
}
/* a destroyed voice's slot is reused (the game makes and destroys voices all the time: ~13 a second in the field, so
 * 4096 slots ran out after ~5 min); a stale handle to it no longer matches its obj (voice_of stops on it) */
static u32 new_voice(int kind, u32 vt) {
	u32 o = xrc_heap_alloc(16, 1);
	Voice *v;
	int i;
	for (i = 0; i < nvoices && voices[i].alive; i++) {}
	if (i == MAXVOICES) xrc_fatal(0, "xaudio2: too many voices (%d live)", MAXVOICES);
	v = &voices[i];
	free(v->in); free(v->cache[0]); free(v->cache[1]);
	memset(v, 0, sizeof *v);
	v->kind = kind;
	v->alive = 1;
	v->obj = o;
	v->volume = 1.0f;
	v->ratio = 1.0f;
	v->nsend = -1;
	{ int k; for (k = 0; k < 8; k++) v->chvol[k] = 1.0f; }
	GF(XaObject, o, vtable) = vt;
	GF(XaObject, o, id) = (u32)i;
	if (i == nvoices) nvoices++;
	return o;
}

/* the samples a buffer plays once (PlayLength, else all its data) */
static u32 buf_samples(const Voice *v, const Buf *b) {
	if (b->play_len) return b->play_len;
	if (v->tag == 0x161 || v->tag == 0x166) /* xWMA: by the average byte rate */
		return v->avg_bytes ? (u32)((unsigned long long)b->bytes * v->rate / v->avg_bytes) : 0;
	return v->block ? b->bytes / v->block * v->spb : 0;
}

/* ============================================================================================ the engine thread */
static void cb_call(XCtx *c, u32 obj, int slot, int nargs, u32 a1, u32 a2) {
	u32 args[3];
	if (!obj) return;
	args[0] = obj; args[1] = a1; args[2] = a2;
	xrc_call_guest(c, M32(M32(obj) + 4u * (u32)slot), nargs, args);
}
#ifdef XRC_REFERENCE
/* ref_xaudio2.c: a voice the real engine plays -- the samples it played since the last pass (*n) and how many more
 * buffers it has finished than this voice has removed (*ends); 0 if it has no real voice */
int ref_xa2_pass(u32 obj, unsigned long long removed, double *n, long long *ends);
#endif
/* ============================================================================================ the mixer */
#ifndef XRC_REFERENCE
static HostAudio *mix_out;
#ifdef XRC_NO_AUDIO
static int mix_off = 1;        /* built without audio: no mixing, no output */
#else
static int mix_off = -1;       /* XRC_NO_AUDIO, or no output */
#endif
static short *mix_pcm;
static unsigned mix_target;
static int mix_pcm_cap;
/* the mastering voice (the default destination) */
static Voice *master_voice(void) {
	int k;
	for (k = 0; k < nvoices; k++) if (voices[k].alive && voices[k].kind == V_MASTER) return &voices[k];
	return 0;
}
static Voice *voice_at(u32 obj) {
	u32 i;
	if (!obj) return 0;
	i = M32(obj + 4);
	return i < (u32)nvoices && voices[i].obj == obj && voices[i].alive && voices[i].kind != V_SOURCE ? &voices[i] : 0;
}
/* send k's destination (0: none / gone) */
static Voice *dest_of(Voice *v, int k) { return v->nsend < 0 ? (k ? 0 : master_voice()) : k < v->nsend ? voice_at(v->send[k]) : 0; }
static int nsends(const Voice *v) { return v->nsend < 0 ? 1 : v->nsend; }
/* XAudio2's default level from source channel s to destination channel d (sc -> dc channels) */
static float default_level(u32 sc, u32 dc, u32 s, u32 d) {
	if (sc == 1) return dc == 1 || d < 2 ? 1.0f : 0.0f;          /* mono: front left and right */
	if (dc == 1) return 1.0f / (float)sc;                       /* to mono: the average */
	if (sc == 2 && dc >= 2) return s == d ? 1.0f : 0.0f;          /* stereo: left / right */
	if (sc == 6 && dc == 2) {                                   /* 5.1 to stereo: FL FR FC LFE BL BR */
		static const float m[2][6] = {{1, 0, 0.7071f, 0, 0.7071f, 0}, {0, 1, 0.7071f, 0, 0, 0.7071f}};
		return m[d][s];
	}
	return s == d ? 1.0f : 0.0f;
}
/* the gains from v's channels to send k's destination's channels: g[d * sc + s] */
static void gains(const Voice *v, int k, const Voice *dst, float *g) {
	u32 sc = v->channels ? v->channels : 1, dc = dst->channels ? dst->channels : 1, s, d;
	for (d = 0; d < dc && d < 8; d++)
		for (s = 0; s < sc && s < 8; s++) {
			float l = k < 8 && v->has_mat[k] ? v->mat[k][d * sc + s] : default_level(sc, dc, s, d);
			g[d * sc + s] = l * v->volume * v->chvol[s];
		}
}
/* a submix / mastering voice's input for a pass of dt seconds (zeroed) */
static void mix_begin(Voice *v, double dt) {
	double f = dt * v->rate + v->in_carry;
	int n = (int)f;
	u32 ch = v->channels ? v->channels : 1;
	v->in_carry = f - n;
	if (n * (int)ch > v->in_cap) { v->in_cap = n * (int)ch * 2; v->in = (float *)realloc(v->in, (size_t)v->in_cap * sizeof(float)); }
	v->in_n = n;
	memset(v->in, 0, (size_t)n * ch * sizeof(float));
}
/* MS-ADPCM: block `blk` of v's data at `data` decoded (spb samples of every channel, interleaved) */
static const short *adpcm_block(Voice *v, u32 data, u32 blk) {
	static const int adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230};
	static const int coef[7][2] = {{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}};
	u32 key = data + blk * v->block + 1, ch = v->channels ? v->channels : 1, k, i;
	const u8 *p = xrc_mem + data + blk * v->block;
	int s1[2], s2[2], delta[2], c1[2], c2[2];
	short *out;
	int slot;
	for (slot = 0; slot < 2; slot++) if (v->cache[slot] && v->cache_key[slot] == key) return v->cache[slot];
	if (ch > 2) return 0;
	slot = v->cache_next; v->cache_next ^= 1;
	if (!v->cache[slot]) v->cache[slot] = (short *)malloc((size_t)v->spb * ch * sizeof(short));
	out = v->cache[slot];
	v->cache_key[slot] = key;
	for (k = 0; k < ch; k++) { int pi = p[k] < 7 ? p[k] : 0; c1[k] = coef[pi][0]; c2[k] = coef[pi][1]; }
	for (k = 0; k < ch; k++) {
		delta[k] = (short)(p[ch + 2 * k] | p[ch + 2 * k + 1] << 8);
		s1[k] = (short)(p[3 * ch + 2 * k] | p[3 * ch + 2 * k + 1] << 8);
		s2[k] = (short)(p[5 * ch + 2 * k] | p[5 * ch + 2 * k + 1] << 8);
		out[k] = (short)s2[k];          /* (sample 0 is the older one) */
		if (v->spb > 1) out[ch + k] = (short)s1[k];
	}
	p += 7 * ch;
	for (i = 2 * ch; i < v->spb * ch; i++) {
		u32 n = i - 2 * ch, k2 = i % ch;
		int nib = (n & 1) ? p[n >> 1] & 15 : p[n >> 1] >> 4, sn = nib & 8 ? nib - 16 : nib;
		int pred = (s1[k2] * c1[k2] + s2[k2] * c2[k2]) >> 8, x = pred + sn * delta[k2];
		if (x > 32767) x = 32767; else if (x < -32768) x = -32768;
		s2[k2] = s1[k2]; s1[k2] = x;
		out[i] = (short)x;
		delta[k2] = adapt[nib] * delta[k2] >> 8;
		if (delta[k2] < 16) delta[k2] = 16;
	}
	return out;
}
/* sample frame i (from the buffer's PlayBegin) of v's buffer b, as floats in f[channels] */
static void frame_at(Voice *v, const Buf *b, long long i, float *f) {
	u32 ch = v->channels ? v->channels : 1, k;
	unsigned long long total = v->block ? (unsigned long long)b->bytes / v->block * v->spb : 0;
	long long x = (long long)b->play_begin + i;
	if (!total) { for (k = 0; k < ch; k++) f[k] = 0; return; }
	if (x < 0) x = 0;
	if ((unsigned long long)x >= total) x = (long long)total - 1;
	if (v->tag == 2) {
		const short *d = adpcm_block(v, b->data, (u32)(x / v->spb));
		for (k = 0; k < ch; k++) f[k] = d ? d[(x % v->spb) * ch + k] * (1.0f / 32768) : 0;
	} else if (v->tag == 3 || (v->tag == 0xfffe && v->bits == 32)) {
		const float *d = (const float *)(xrc_mem + b->data) + x * ch;
		for (k = 0; k < ch; k++) f[k] = d[k];
	} else if (v->bits == 8) {
		const u8 *d = xrc_mem + b->data + x * ch;
		for (k = 0; k < ch; k++) f[k] = (d[k] - 128) * (1.0f / 128);
	} else {
		const short *d = (const short *)(xrc_mem + b->data) + x * ch;
		for (k = 0; k < ch; k++) f[k] = d[k] * (1.0f / 32768);
	}
}
/* the source's samples [p0, p1) of buffer b (in its own position units) mixed into its destinations: an output frame at
 * every step (the source's rate x ratio / the destination's rate) from v->acc on */
static void mix_seg(Voice *v, const Buf *b, double p0, double p1, int last) {
	float g[8][64], f0[8], f1[8];
	Voice *dst[8];
	int nd = 0, k;
	u32 sc = v->channels ? v->channels : 1, s, d;
	double step;
	if (mix_off == 1) return;
	for (k = 0; k < nsends(v) && k < 8; k++) if ((dst[nd] = dest_of(v, k)) != 0 && dst[nd]->in) { gains(v, k, dst[nd], g[nd]); nd++; }
	if (!nd || sc > 8) return;
	step = (double)v->rate * v->ratio / (dst[0]->rate ? dst[0]->rate : 48000);
	if (step <= 0) return;
	if (v->acc < p0) v->acc = p0;
	{ /* the frames whose positions fall in [p0, p1) -- the pass's last segment: all its frames left */
		int left = dst[0]->in_n - v->oi, cnt = last ? left : v->acc >= p1 ? 0 : (int)ceil((p1 - v->acc) / step - 1e-9);
		if (cnt > left) cnt = left;
		v->mix_cnt = cnt;
	}
	while (v->mix_cnt-- > 0) {
		long long i = (long long)v->acc;
		float t = (float)(v->acc - (double)i);
		frame_at(v, b, i, f0);
		if (t > 0) frame_at(v, b, i + 1, f1); else memcpy(f1, f0, sizeof f0);
		for (s = 0; s < sc; s++) f0[s] += (f1[s] - f0[s]) * t;
		for (k = 0; k < nd; k++) {
			u32 dc = dst[k]->channels ? dst[k]->channels : 1;
			float *o;
			if (v->oi >= dst[k]->in_n) continue;
			o = dst[k]->in + (size_t)v->oi * dc;
			for (d = 0; d < dc && d < 8; d++) {
				float a = 0;
				for (s = 0; s < sc; s++) a += g[k][d * sc + s] * f0[s];
				o[d] += a;
			}
		}
		v->oi++;
		v->acc += step;
	}
}
/* the submix voices (by processing stage), then the mastering voice to the host output */
static void mix_end(void) {
	Voice *m = master_voice();
	int k, st, n;
	u32 stages_done = 0;
	for (st = 0; st < 64 && stages_done < 1u << 31; st++) {
		int any = 0;
		for (k = 0; k < nvoices; k++) {
			Voice *v = &voices[k];
			int j;
			if (!v->alive || v->kind != V_SUBMIX || (int)v->stage != st || !v->in) continue;
			any = 1;
			for (j = 0; j < nsends(v) && j < 8; j++) {
				Voice *dst = dest_of(v, j);
				float g[64];
				u32 sc = v->channels ? v->channels : 1, dc, s, d;
				int i;
				if (!dst || !dst->in || dst == v || sc > 8) continue;
				dc = dst->channels ? dst->channels : 1;
				gains(v, j, dst, g);
				for (i = 0; i < dst->in_n; i++) { /* (the rates: frame i of the destination from frame i * n / dn) */
					int si = dst->in_n == v->in_n ? i : (int)((long long)i * v->in_n / (dst->in_n ? dst->in_n : 1));
					const float *f;
					if (si >= v->in_n) break;
					f = v->in + (size_t)si * sc;
					for (d = 0; d < dc && d < 8; d++) {
						float a = 0;
						for (s = 0; s < sc; s++) a += g[d * sc + s] * f[s];
						dst->in[(size_t)i * dc + d] += a;
					}
				}
			}
		}
		if (!any && st > 32) break;
	}
	if (!m || !m->in || mix_off == 1) return;
	if (mix_off < 0) {
		mix_out = host_audio_open(m->rate, m->channels ? m->channels : 2);
		mix_off = mix_out ? 0 : 1;
		if (mix_out) fprintf(stderr, "xrc: xaudio2: mixing to the host output (%u Hz, %u channels)\n", m->rate, m->channels);
		if (mix_off) return;
	}
	{ /* the latency: 100 ms queued ahead (the engine thread's passes wait for the guest lock); an underrun adds silence
	   * back up to it and raises it by 40 ms, to 400 */
		static unsigned target = 100, underruns;
		static short *sil;
		u32 ch = m->channels ? m->channels : 2;
		unsigned q = host_audio_queued_ms(mix_out);
		if (q < 10) {
			unsigned ms;
			if (underruns++ && target < 400) target += 40;
			if (underruns > 1 && xrc_tool_env("XRC_XA2_LOG")) fprintf(stderr, "xrc: xaudio2: an underrun (%u): %u ms ahead now\n", underruns - 1, target);
			ms = target - q;
			if (!sil) sil = (short *)calloc((size_t)m->rate / 1000 * 400 * ch, 2);
			host_audio_try_write(mix_out, sil, m->rate / 1000 * ms * ch * 2u, 1000);
		}
		mix_target = target;
	}
	n = m->in_n * (int)(m->channels ? m->channels : 2);
	if (n > mix_pcm_cap) { mix_pcm_cap = n * 2; mix_pcm = (short *)realloc(mix_pcm, (size_t)mix_pcm_cap * sizeof(short)); }
	for (k = 0; k < n; k++) {
		float x = m->in[k] * m->volume * m->chvol[k % (int)(m->channels ? m->channels : 2) & 7] * 32767.0f;
		mix_pcm[k] = (short)(x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : (int)x);
	}
	host_audio_try_write(mix_out, mix_pcm, (u32)n * 2u, mix_target + 100); /* (more queued: dropped, catching up) */
	{ /* XRC_XA2_DUMP=FILE: the mix as raw 16-bit PCM (the master's rate and channels) */
		static FILE *dump;
		static int tried;
		if (!tried) { tried = 1; if (xrc_tool_env("XRC_XA2_DUMP")) dump = fopen(xrc_tool_env("XRC_XA2_DUMP"), "wb"); }
		if (dump) fwrite(mix_pcm, 2, (size_t)n, dump);
	}
}
#endif

/* one pass of a source voice: advances it by n samples, running its callbacks; at most `ends` buffers end (the
 * reference target's real voice decides when: a buffer it still plays waits at its end) */
static void voice_pass(XCtx *c, int vi, double n, long long ends) {
	Voice *v = &voices[vi];
	u32 need, queued = 0, vobj = v->obj; /* (a callback can destroy the voice and a new one take its slot) */
	int k;
	for (k = 0; k < v->qn; k++) queued += v->q[(v->qhead + k) % MAXQ].bytes;
	if (v->qn && v->head_started) { /* (less what the head buffer has played) */
		u32 done = (u32)(v->pos / v->spb) * v->block;
		queued = done < queued ? queued - done : 0;
	}
	need = (u32)(n / v->spb + 1) * v->block;
	cb_call(c, v->cb, 0, 2, queued >= need ? 0 : need - queued, 0); /* OnVoiceProcessingPassStart */
	v = &voices[vi];
	if (v->started && v->st_submits) { v->st_passes++; if (!v->qn) v->st_starved++; }
	while (n > 0 && v->alive && v->obj == vobj && v->started && v->qn) {
		Buf *b = &v->q[v->qhead];
		u32 len = buf_samples(v, b), end = len;
		if (!v->head_started) {
			v->head_started = 1;
			v->pos = 0;
			v->loops_left = b->loop_count;
			cb_call(c, v->cb, 3, 2, b->ctx, 0); /* OnBufferStart */
			if (!v->alive || v->obj != vobj || !v->qn) break;
			b = &v->q[v->qhead];
		}
		if (v->loops_left && b->loop_len) end = b->loop_begin + b->loop_len - b->play_begin;
		else if (v->loops_left && b->loop_count) end = len; /* whole-buffer loop */
#ifndef XRC_REFERENCE
		mix_seg(v, b, v->pos, v->pos + n < end ? v->pos + n : end, v->pos + n < end);
#endif
		if (v->pos + n < end) { v->pos += n; v->played += (unsigned long long)n; break; }
		n -= end - v->pos;
		v->played += (unsigned long long)(end - v->pos);
		if (!v->loops_left && ends <= 0) { v->played += (unsigned long long)(end - v->pos); v->pos = end; break; }
		if (v->loops_left) {
			if (v->loops_left != 255) v->loops_left--;
			v->pos = b->loop_len ? b->loop_begin - b->play_begin : 0;
			v->acc += v->pos - end; /* (the resampling position, in the loop's start's terms) */
			cb_call(c, v->cb, 5, 2, b->ctx, 0); /* OnLoopEnd */
			if (b->loop_len == 0 && len == 0) break;
			continue;
		}
		{
			u32 ctx = b->ctx, eos = b->flags & 0x40;
			v->qhead = (v->qhead + 1) % MAXQ;
			v->qn--;
			v->removed++;
			ends--;
			v->st_ends++;
			v->head_started = 0;
			v->acc -= end; /* (in the next buffer's terms) */
			v->pos = 0;
			cb_call(c, v->cb, 4, 2, ctx, 0); /* OnBufferEnd */
			if (eos) cb_call(c, v->cb, 2, 1, 0, 0); /* OnStreamEnd */
		}
	}
	if (vi < nvoices && voices[vi].alive && voices[vi].obj == vobj && !voices[vi].qn) voices[vi].acc = 0; /* (starved: the next buffer from its start) */
#ifdef XRC_REFERENCE
	/* buffers the real voice finished before this one's count got there (elsewhere ends is unbounded: none) */
	while (ends > 0 && vi < nvoices && voices[vi].alive && voices[vi].obj == vobj && voices[vi].qn) {
		Buf *b;
		u32 ctx, eos;
		v = &voices[vi];
		b = &v->q[v->qhead];
		if (!v->head_started) {
			v->head_started = 1;
			cb_call(c, v->cb, 3, 2, b->ctx, 0); /* OnBufferStart */
			v = &voices[vi];
			if (!v->alive || !v->qn) break;
			b = &v->q[v->qhead];
		}
		ctx = b->ctx;
		eos = b->flags & 0x40;
		v->qhead = (v->qhead + 1) % MAXQ;
		v->qn--;
		v->removed++;
		ends--;
		v->head_started = 0;
		v->pos = 0;
		cb_call(c, v->cb, 4, 2, ctx, 0); /* OnBufferEnd */
		if (eos) cb_call(c, v->cb, 2, 1, 0, 0); /* OnStreamEnd */
	}
#endif
	if (vi < nvoices && voices[vi].alive && voices[vi].obj == vobj) cb_call(c, voices[vi].cb, 1, 1, 0, 0); /* OnVoiceProcessingPassEnd */
}
static void hle_xa2_engine_thread(XCtx *c) {
	unsigned long long last = now_us();
	for (;;) {
		unsigned long long t;
		double dt;
		int k;
		xrc_sleep_c(10, 4);
		t = now_us();
		dt = (double)(t - last) / 1e6;
		last = t;
		if (dt > 0.1) dt = 0.1; /* a stall does not run seconds of audio at once */
		if (!engine_running) continue;
		if (xrc_tool_env("XRC_XA2_LOG")) { /* each second: the voices that starved (passes with nothing queued) */
			static unsigned long long next;
			static unsigned passes, slow;
			passes++; if (dt > 0.015) slow++;
			if (!next) next = t + 1000000;
			if (t >= next) {
				next = t + 1000000;
				fprintf(stderr, "xrc: xaudio2: %u passes, %u late\n", passes, slow);
				passes = slow = 0;
				for (k = 0; k < nvoices; k++) {
					Voice *v = &voices[k];
					if (v->alive && v->st_starved)
						fprintf(stderr, "xrc: xaudio2:   voice %d (%04x %u ch %u Hz block %u ratio %.3f -> %d): starved %u of %u passes; %u submits of %llu bytes avg (play %u+%u), %.1f queued before; %llu samples played; %u ended, %u flushes, %u stops, %u starts, %u GetState\n",
						        k, v->tag, v->channels, v->rate, v->block, v->ratio, v->nsend >= 0 && v->nsend ? (int)M32(v->send[0] + 4) : -1,
						        v->st_starved, v->st_passes, v->st_submits, v->st_submits ? v->st_bytes / v->st_submits : 0,
						        v->q[v->qhead].play_begin, v->q[v->qhead].play_len,
						        v->st_submits ? (double)v->st_qn / v->st_submits : 0, v->played - v->st_played,
						        v->st_ends, v->st_flush, v->st_stop, v->st_start, v->st_getstate);
					v->st_ends = v->st_flush = v->st_stop = v->st_start = v->st_getstate = 0;
					v->st_played = v->played;
					v->st_starved = v->st_passes = 0; v->st_submits = v->st_submits ? 1 : 0; v->st_bytes = v->st_qn = 0;
				}
			}
		}
		for (k = 0; k < nengine_cbs; k++) cb_call(c, engine_cbs[k], 0, 1, 0, 0); /* OnProcessingPassStart */
#ifndef XRC_REFERENCE
		if (mix_off != 1) {
			for (k = 0; k < nvoices; k++) {
				if (voices[k].alive && voices[k].kind != V_SOURCE) mix_begin(&voices[k], dt);
				voices[k].oi = 0;
			}
		}
#endif
		for (k = 0; k < nvoices; k++)
			while (voices[k].alive && voices[k].nflushed) { /* (a callback may flush more) */
				u32 ctx = voices[k].flushed[0];
				memmove(voices[k].flushed, voices[k].flushed + 1, 4u * (u32)--voices[k].nflushed);
				cb_call(c, voices[k].cb, 4, 2, ctx, 0); /* OnBufferEnd */
			}
		for (k = 0; k < nvoices; k++)
			if (voices[k].alive && voices[k].kind == V_SOURCE && voices[k].started) {
				double n = dt * voices[k].rate * voices[k].ratio;
				long long ends = 1ll << 40;
#ifndef XRC_REFERENCE
				if (mix_off != 1) { /* the samples its output frames this pass take (no gap or skip between passes) */
					Voice *d0 = 0;
					int j;
					for (j = 0; j < nsends(&voices[k]) && j < 8 && !d0; j++) d0 = dest_of(&voices[k], j);
					if (d0 && d0->in && d0->rate) n = (double)d0->in_n * voices[k].rate * voices[k].ratio / d0->rate;
					voices[k].acc = voices[k].pos;
				}
#endif
#ifdef XRC_REFERENCE
				ref_xa2_pass(voices[k].obj, voices[k].removed, &n, &ends);
#endif
				voice_pass(c, k, n, ends);
			}
#ifndef XRC_REFERENCE
		if (mix_off != 1) mix_end();
#endif
		for (k = 0; k < nengine_cbs; k++) cb_call(c, engine_cbs[k], 1, 1, 0, 0); /* OnProcessingPassEnd */
	}
}

/* ============================================================================================ IXAudio2 */
static void xa_QueryInterface(XCtx *c) { engine_refs++; M32(A(2)) = A(0); RET(S_OK, 3); }
static void xa_AddRef(XCtx *c) { RET(++engine_refs, 1); }
static void xa_Release(XCtx *c) {
	if (engine_refs > 1) engine_refs--;
	else engine_running = 0; /* the last reference: the engine stops (the object stays for a later create) */
	RET(engine_refs - (engine_running ? 0 : 1), 1);
}
static void xa_GetDeviceCount(XCtx *c) { M32(A(1)) = 1; RET(S_OK, 2); }
static void put_w(u32 p, const char *s) { u32 k; for (k = 0; s[k]; k++) M16(p + 2 * k) = (u8)s[k]; M16(p + 2 * k) = 0; }
/* XAUDIO2_DEVICE_DETAILS: DeviceID[256] DisplayName[256] (WCHAR) Role WAVEFORMATEXTENSIBLE */
static void put_mix_format(u32 f) {
	GF(WaveFormatExtensible, f, format_tag) = 0xfffe; GF(WaveFormatExtensible, f, channels) = 2; GF(WaveFormatExtensible, f, samples_per_sec) = 48000; GF(WaveFormatExtensible, f, avg_bytes_per_sec) = 48000 * 8; GF(WaveFormatExtensible, f, block_align) = 8; GF(WaveFormatExtensible, f, bits_per_sample) = 32;
	GF(WaveFormatExtensible, f, cb_size) = 22; GF(WaveFormatExtensible, f, valid_bits_per_sample) = 32; GF(WaveFormatExtensible, f, channel_mask) = 3; /* SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT */
	GF(WaveFormatExtensible, f, sub_format_data1) = 3; GF(WaveFormatExtensible, f, sub_format_data2) = 0; GF(WaveFormatExtensible, f, sub_format_data3) = 0x10; /* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT */
	GFI(WaveFormatExtensible, f, sub_format_data4, 0) = 0x80; GFI(WaveFormatExtensible, f, sub_format_data4, 1) = 0; GFI(WaveFormatExtensible, f, sub_format_data4, 2) = 0; GFI(WaveFormatExtensible, f, sub_format_data4, 3) = 0xaa; GFI(WaveFormatExtensible, f, sub_format_data4, 4) = 0; GFI(WaveFormatExtensible, f, sub_format_data4, 5) = 0x38; GFI(WaveFormatExtensible, f, sub_format_data4, 6) = 0x9b; GFI(WaveFormatExtensible, f, sub_format_data4, 7) = 0x71;
}
static void xa_GetDeviceDetails(XCtx *c) {
	u32 p = A(2);
	if (A(1) != 0) RET(E_INVALIDARG, 3);
	memset(xrc_mem + p, 0, 1068);
	put_w(p, "{0.0.0.00000000}.{xrc-speakers}");
	put_w(p + 512, "Speakers");
	GF(XaDeviceDetails, p, role) = 0xf; /* GlobalDefaultDevice */
	put_mix_format(GADDR(XaDeviceDetails, p, output_format));
	RET(S_OK, 3);
}
static void xa_Initialize(XCtx *c) {
	if (!engine_thread) {
		u32 start = xrc_import_addr(DLL, "xrc_xa2_engine_thread");
		if (!xrc_thread_create(start, 0, 0x40000, 0)) xrc_fatal(c, "xaudio2: no engine thread");
		engine_thread = 1;
	}
	engine_running = 1;
	RET(S_OK, 3);
}
static void xa_RegisterForCallbacks(XCtx *c) {
	int k;
	for (k = 0; k < nengine_cbs; k++) if (engine_cbs[k] == A(1)) RET(S_OK, 2);
	if (nengine_cbs < 16) engine_cbs[nengine_cbs++] = A(1);
	RET(S_OK, 2);
}
static void xa_UnregisterForCallbacks(XCtx *c) {
	int k;
	for (k = 0; k < nengine_cbs; k++)
		if (engine_cbs[k] == A(1)) { engine_cbs[k] = engine_cbs[--nengine_cbs]; break; }
	RET(0, 2);
}
/* XAUDIO2_VOICE_SENDS {count, {flags, voice} *} (0: the default, the mastering voice) */
static void set_sends(Voice *v, u32 p) {
	u32 n, k;
	v->nsend = -1;
	memset(v->has_mat, 0, sizeof v->has_mat);
	if (!p) return;
	n = GF(XaVoiceSends, p, send_count);
	v->nsend = n > 8 ? 8 : (int)n;
	for (k = 0; k < (u32)v->nsend; k++) v->send[k] = GF(XaSendDescriptor, GF(XaVoiceSends, p, sends) + 8 * k, output_voice);
}
/* CreateSourceVoice(this, **voice, WAVEFORMATEX *, flags, max ratio, callback, sends, effects) */
static void xa_CreateSourceVoice(XCtx *c) {
	u32 f = A(2), o;
	Voice *v;
	if (!A(1) || !f) RET(E_INVALIDARG, 8);
	o = new_voice(V_SOURCE, vt_src);
	v = voice_of(o);
	v->tag = GF(WaveFormatExtensible, f, format_tag); v->channels = GF(WaveFormatExtensible, f, channels); v->rate = GF(WaveFormatExtensible, f, samples_per_sec); v->avg_bytes = GF(WaveFormatExtensible, f, avg_bytes_per_sec); v->block = GF(WaveFormatExtensible, f, block_align);
	v->spb = 1;
	if (v->tag == 2 && GF(WaveFormatExtensible, f, cb_size) >= 2) v->spb = GF(WaveFormatExtensible, f, valid_bits_per_sample); /* ADPCMWAVEFORMAT.wSamplesPerBlock */
	if (!v->block) v->block = 1;
	v->bits = GF(WaveFormatExtensible, f, bits_per_sample);
	if (v->tag == 0xfffe && GF(WaveFormatExtensible, f, cb_size) >= 22 && GF(WaveFormatExtensible, f, sub_format_data1) == 1) v->tag = 1; /* (extensible PCM) */
	else if (v->tag == 0xfffe && GF(WaveFormatExtensible, f, cb_size) >= 22 && GF(WaveFormatExtensible, f, sub_format_data1) == 3) v->tag = 3; /* (extensible float) */
	set_sends(v, A(6));
	v->flags = A(3);
	v->max_ratio = f32_of(A(4));
	v->cb = A(5);
	if (xrc_tool_env("XRC_XA2_LOG")) { /* each format once */
		static u32 seen[64][3];
		static int nseen;
		int k;
		for (k = 0; k < nseen && !(seen[k][0] == v->tag && seen[k][1] == v->channels && seen[k][2] == v->rate); k++) {}
		if (k == nseen && nseen < 64) {
			seen[nseen][0] = v->tag; seen[nseen][1] = v->channels; seen[nseen][2] = v->rate; nseen++;
			fprintf(stderr, "xrc: xaudio2: a source voice: format %04x, %u channels, %u Hz, block %u, %u samples/block, %u bits\n",
			        v->tag, v->channels, v->rate, v->block, v->spb, GF(WaveFormatExtensible, f, bits_per_sample));
		}
	}
	M32(A(1)) = o;
	RET(S_OK, 8);
}
/* CreateSubmixVoice(this, **voice, channels, rate, flags, stage, sends, effects) */
static void xa_CreateSubmixVoice(XCtx *c) {
	u32 o = new_voice(V_SUBMIX, vt_voice);
	Voice *v = voice_of(o);
	v->channels = A(2); v->rate = A(3); v->flags = A(4); v->stage = A(5);
	set_sends(v, A(6));
	M32(A(1)) = o;
	RET(S_OK, 8);
}
/* CreateMasteringVoice(this, **voice, channels, rate, flags, device, effects): XAUDIO2_DEFAULT_* (0) -> the device's */
static void xa_CreateMasteringVoice(XCtx *c) {
	u32 o = new_voice(V_MASTER, vt_voice);
	Voice *v = voice_of(o);
	v->channels = A(2) ? A(2) : 2; v->rate = A(3) ? A(3) : 48000; v->flags = A(4);
	M32(A(1)) = o;
	RET(S_OK, 7);
}
static void xa_StartEngine(XCtx *c) { engine_running = 1; RET(S_OK, 1); }
static void xa_StopEngine(XCtx *c) { engine_running = 0; RET(0, 1); }
static void xa_CommitChanges(XCtx *c) { RET(S_OK, 2); }
static void xa_GetPerformanceData(XCtx *c) {
	u32 p = A(1), act = 0, tot = 0, sub = 0;
	int k;
	for (k = 0; k < nvoices; k++)
		if (voices[k].alive) {
			if (voices[k].kind == V_SOURCE) { tot++; act += voices[k].started != 0; }
			else if (voices[k].kind == V_SUBMIX) sub++;
		}
	memset(xrc_mem + p, 0, 64);
	GF(XaPerformanceData, p, current_latency_in_samples) = 480; /* CurrentLatencyInSamples */
	GF(XaPerformanceData, p, active_source_voice_count) = act; GF(XaPerformanceData, p, total_source_voice_count) = tot; GF(XaPerformanceData, p, active_submix_voice_count) = sub;
	RET(0, 2);
}
static void xa_SetDebugConfiguration(XCtx *c) { RET(0, 3); }

/* ============================================================================================ voices */
/* XAUDIO2_VOICE_DETAILS: CreationFlags InputChannels InputSampleRate */
static void vo_GetVoiceDetails(XCtx *c) {
	Voice *v = voice_of(A(0));
	GF(XaVoiceDetails, A(1), creation_flags) = v->flags; GF(XaVoiceDetails, A(1), input_channels) = v->channels; GF(XaVoiceDetails, A(1), input_sample_rate) = v->rate;
	RET(0, 2);
}
static void vo_GetEffectState(XCtx *c) { M32(A(2)) = 1; RET(0, 3); }
/* GetEffectParameters(this, effect, params, size): a volume meter's XAUDIO2FX_VOLUMEMETER_LEVELS {peak *, rms *,
 * channels} (12 bytes) gets silence in the caller's arrays; anything else zeros */
static void vo_GetEffectParameters(XCtx *c) {
	u32 p = A(2), n = A(3);
	if (n == 12) {
		u32 ch = GF(XaVolumeMeterLevels, p, channel_count) > 64 ? 64 : GF(XaVolumeMeterLevels, p, channel_count);
		if (GF(XaVolumeMeterLevels, p, peak_levels)) memset(xrc_mem + GF(XaVolumeMeterLevels, p, peak_levels), 0, 4u * ch);
		if (GF(XaVolumeMeterLevels, p, rms_levels)) memset(xrc_mem + GF(XaVolumeMeterLevels, p, rms_levels), 0, 4u * ch);
	} else memset(xrc_mem + p, 0, n);
	RET(S_OK, 4);
}
/* XAUDIO2_FILTER_PARAMETERS: Type (LowPassFilter) Frequency OneOverQ -- the "no filter" default */
static void vo_GetFilterParameters(XCtx *c) {
	u32 p = A(1);
	GF(XaFilterParameters, p, type) = 0; GF(XaFilterParameters, p, frequency) = u32_of(1.0f); GF(XaFilterParameters, p, one_over_q) = u32_of(1.0f);
	RET(0, 2);
}
static void vo_GetOutputFilterParameters(XCtx *c) {
	u32 p = A(2);
	GF(XaFilterParameters, p, type) = 0; GF(XaFilterParameters, p, frequency) = u32_of(1.0f); GF(XaFilterParameters, p, one_over_q) = u32_of(1.0f);
	RET(0, 3);
}
static void vo_SetVolume(XCtx *c) { voice_of(A(0))->volume = f32_of(A(1)); RET(S_OK, 3); }
static void vo_GetVolume(XCtx *c) { M32(A(1)) = u32_of(voice_of(A(0))->volume); RET(0, 2); }
static void vo_GetChannelVolumes(XCtx *c) {
	Voice *v = voice_of(A(0));
	u32 k;
	for (k = 0; k < A(1); k++) M32(A(2) + 4 * k) = u32_of(k < 8 ? v->chvol[k] : 1.0f);
	RET(0, 3);
}
/* SetChannelVolumes(this, channels, volumes, operation set) */
static void vo_SetChannelVolumes(XCtx *c) {
	Voice *v = voice_of(A(0));
	u32 k;
	for (k = 0; k < A(1) && k < 8; k++) v->chvol[k] = f32_of(M32(A(2) + 4 * k));
	RET(S_OK, 4);
}
/* SetOutputVoices(this, sends) */
static void vo_SetOutputVoices(XCtx *c) { set_sends(voice_of(A(0)), A(1)); RET(S_OK, 2); }
/* the send that goes to dest (0: the only one) */
static int send_index(Voice *v, u32 dest) {
	int k;
	if (!dest) return 0;
	if (v->nsend < 0) return 0;
	for (k = 0; k < v->nsend; k++) if (v->send[k] == dest) return k;
	return -1;
}
/* SetOutputMatrix(this, dest, source channels, dest channels, levels[dest][source], operation set) */
static void vo_SetOutputMatrix(XCtx *c) {
	Voice *v = voice_of(A(0));
	u32 sc = A(2), dc = A(3), k;
	int i = send_index(v, A(1));
	if (i < 0 || i >= 8 || sc * dc > 64) RET(S_OK, 6);
	for (k = 0; k < sc * dc; k++) v->mat[i][k] = f32_of(M32(A(4) + 4 * k));
	v->has_mat[i] = 1;
	RET(S_OK, 6);
}
/* GetOutputMatrix(this, dest, src channels, dest channels, levels[dest][src]) */
static void vo_GetOutputMatrix(XCtx *c) {
	Voice *v = voice_of(A(0));
	u32 s = A(2), d = A(3), i, j;
	int k = send_index(v, A(1));
	for (i = 0; i < d; i++)
		for (j = 0; j < s; j++)
			M32(A(4) + 4 * (i * s + j)) = u32_of(k >= 0 && k < 8 && v->has_mat[k] && s * d <= 64 ? v->mat[k][i * s + j] : i % s == j ? 1.0f : 0.0f);
	RET(0, 5);
}
static void vo_DestroyVoice(XCtx *c) {
	Voice *v = voice_of(A(0));
	v->alive = 0;
	v->started = 0;
	RET(0, 1);
}
static void vo_set_generic(XCtx *c) {
	/* the setters all voices accept: arguments from the method list */
	static const char *const n[] = {"SetOutputVoices", "SetEffectChain", "EnableEffect", "DisableEffect", "SetEffectParameters",
	                                "SetFilterParameters", "SetOutputFilterParameters", "SetChannelVolumes", "SetOutputMatrix"};
	static const int a[] = {2, 2, 3, 3, 5, 3, 4, 4, 6};
	const char *m = strstr(xrc_cur_import, "::");
	unsigned k;
	for (k = 0; m && k < sizeof a / sizeof a[0]; k++)
		if (!strcmp(m + 2, n[k])) RET(S_OK, a[k]);
	xrc_fatal(c, "xaudio2: %s is not implemented yet", xrc_cur_import);
}

static void sv_Start(XCtx *c) { voice_of(A(0))->started = 1; voice_of(A(0))->st_start++; RET(S_OK, 3); }
static void sv_Stop(XCtx *c) {
	Voice *v = voice_of(A(0));
	v->started = 0;
	v->st_stop++;
	RET(S_OK, 3);
}
static void sv_SubmitSourceBuffer(XCtx *c) {
	Voice *v = voice_of(A(0));
	u32 p = A(1);
	Buf *b;
	if (v->qn == MAXQ) RET(XAUDIO2_E_INVALID_CALL, 3);
	b = &v->q[(v->qhead + v->qn) % MAXQ];
	b->flags = GF(XaBuffer, p, flags); b->bytes = GF(XaBuffer, p, audio_bytes); b->data = GF(XaBuffer, p, audio_data); b->play_begin = GF(XaBuffer, p, play_begin); b->play_len = GF(XaBuffer, p, play_length);
	b->loop_begin = GF(XaBuffer, p, loop_begin); b->loop_len = GF(XaBuffer, p, loop_length); b->loop_count = GF(XaBuffer, p, loop_count); b->ctx = GF(XaBuffer, p, context);
	v->st_submits++; v->st_bytes += b->bytes; v->st_qn += (unsigned)v->qn;
	v->qn++;
	RET(S_OK, 3);
}
/* FlushSourceBuffers: the queued buffers but the one playing (of a started voice) are removed; as in XAudio2 their
 * OnBufferEnd runs from the engine thread, on its next pass */
static void sv_FlushSourceBuffers(XCtx *c) {
	Voice *v = voice_of(A(0));
	int keep = v->head_started && v->started ? 1 : 0, k;
	v->st_flush++;
	for (k = keep; k < v->qn && v->nflushed < MAXQ; k++) v->flushed[v->nflushed++] = v->q[(v->qhead + k) % MAXQ].ctx;
	v->removed += (unsigned long long)(v->qn - keep);
	v->qn = keep;
	if (!keep) v->head_started = 0;
	RET(S_OK, 1);
}
static void sv_Discontinuity(XCtx *c) {
	Voice *v = voice_of(A(0));
	if (v->qn) v->q[(v->qhead + v->qn - 1) % MAXQ].flags |= 0x40;
	RET(S_OK, 1);
}
static void sv_ExitLoop(XCtx *c) { voice_of(A(0))->loops_left = 0; RET(S_OK, 2); }
/* XAUDIO2_VOICE_STATE: pCurrentBufferContext BuffersQueued SamplesPlayed (u64) */
static void sv_GetState(XCtx *c) {
	Voice *v = voice_of(A(0));
	u32 p = A(1);
	v->st_getstate++;
	GF(XaVoiceState, p, current_buffer_context) = v->qn ? v->q[v->qhead].ctx : 0;
	GF(XaVoiceState, p, buffers_queued) = (u32)v->qn;
	GF(XaVoiceState, p, samples_played_lo) = (u32)v->played;
	GF(XaVoiceState, p, samples_played_hi) = (u32)(v->played >> 32);
	RET(0, 2);
}
static void sv_SetFrequencyRatio(XCtx *c) {
	Voice *v = voice_of(A(0));
	float r = f32_of(A(1));
	if (r < 1.0f / 1024) r = 1.0f / 1024;
	if (v->max_ratio > 0 && r > v->max_ratio) r = v->max_ratio;
	v->ratio = r;
	RET(S_OK, 3);
}
static void sv_GetFrequencyRatio(XCtx *c) { M32(A(1)) = u32_of(voice_of(A(0))->ratio); RET(0, 2); }
static void sv_SetSourceSampleRate(XCtx *c) { voice_of(A(0))->rate = A(1); RET(S_OK, 2); }

/* ============================================================================================ effects */
static void rv_QueryInterface(XCtx *c) { GF(XaObject, A(0), refs)++; M32(A(2)) = A(0); RET(S_OK, 3); }
static void rv_AddRef(XCtx *c) { RET(++GF(XaObject, A(0), refs), 1); }
static void rv_Release(XCtx *c) { u32 r = GF(XaObject, A(0), refs); if (r) GF(XaObject, A(0), refs) = --r; RET(r, 1); }

/* ============================================================================================ creation */
/* CoCreateInstance for the XAudio2 2.7 classes: 0 and *out set, or REGDB_E_CLASSNOTREG (out 0: only whether the
 * class is one of them) */
u32 xrc_xaudio2_create(const char *cls, u32 out) {
	if (!strcmp(cls, "{5a508685-a254-4fba-9b82-9a24b00306af}") || !strcmp(cls, "{db05ea35-0329-4d4b-a53a-6dead03d3852}")) {
		if (!out) return S_OK;
		make_vtables(); /* CLSID_XAudio2 / CLSID_XAudio2_Debug: one engine per process */
		if (!engine_obj) {
			engine_obj = xrc_heap_alloc(16, 1);
			M32(engine_obj) = vt_xa2;
		} else engine_refs++;
		M32(out) = engine_obj;
		return S_OK;
	}
	if (!strcmp(cls, "{6a93130e-1d53-41d1-a9cf-e758800bb179}") || !strcmp(cls, "{c4f82dd4-cb4e-4ce1-8bdb-ee32d4198269}") ||
	    !strcmp(cls, "{cac1105f-619b-4d04-831a-44e1cbf12d57}") || !strcmp(cls, "{2d9a0f9c-e67b-4b24-ab44-92b3e770c020}")) {
		u32 o; /* CLSID_AudioReverb, CLSID_AudioVolumeMeter (/ _Debug): an effect object, not applied */
		if (!out) return S_OK;
		o = xrc_heap_alloc(16, 1);
		make_vtables();
		GF(XaObject, o, vtable) = vt_reverb;
		GF(XaObject, o, refs) = 1;
		M32(out) = o;
		return S_OK;
	}
	return 0x80040154u;
}

#define M(i, n, f) {DLL, i "::" #n, f}
#define GEN_VOICE(m, a) {DLL, "IXAudio2Voice::" #m, vo_set_generic},
const XrcImport xrc_hle_xaudio2[] = {
	{DLL, "xrc_xa2_engine_thread", hle_xa2_engine_thread},
	M("IXAudio2", QueryInterface, xa_QueryInterface), M("IXAudio2", AddRef, xa_AddRef), M("IXAudio2", Release, xa_Release),
	M("IXAudio2", GetDeviceCount, xa_GetDeviceCount), M("IXAudio2", GetDeviceDetails, xa_GetDeviceDetails),
	M("IXAudio2", Initialize, xa_Initialize), M("IXAudio2", RegisterForCallbacks, xa_RegisterForCallbacks),
	M("IXAudio2", UnregisterForCallbacks, xa_UnregisterForCallbacks), M("IXAudio2", CreateSourceVoice, xa_CreateSourceVoice),
	M("IXAudio2", CreateSubmixVoice, xa_CreateSubmixVoice), M("IXAudio2", CreateMasteringVoice, xa_CreateMasteringVoice),
	M("IXAudio2", StartEngine, xa_StartEngine), M("IXAudio2", StopEngine, xa_StopEngine),
	M("IXAudio2", CommitChanges, xa_CommitChanges), M("IXAudio2", GetPerformanceData, xa_GetPerformanceData),
	M("IXAudio2", SetDebugConfiguration, xa_SetDebugConfiguration),
	M("IXAudio2Voice", GetVoiceDetails, vo_GetVoiceDetails), M("IXAudio2Voice", GetEffectState, vo_GetEffectState),
	M("IXAudio2Voice", GetEffectParameters, vo_GetEffectParameters),
	M("IXAudio2Voice", GetFilterParameters, vo_GetFilterParameters),
	M("IXAudio2Voice", GetOutputFilterParameters, vo_GetOutputFilterParameters),
	M("IXAudio2Voice", SetVolume, vo_SetVolume), M("IXAudio2Voice", GetVolume, vo_GetVolume),
	M("IXAudio2Voice", GetChannelVolumes, vo_GetChannelVolumes), M("IXAudio2Voice", GetOutputMatrix, vo_GetOutputMatrix),
	M("IXAudio2Voice", SetChannelVolumes, vo_SetChannelVolumes), M("IXAudio2Voice", SetOutputVoices, vo_SetOutputVoices),
	M("IXAudio2Voice", SetOutputMatrix, vo_SetOutputMatrix),
	M("IXAudio2Voice", DestroyVoice, vo_DestroyVoice),
	M("IXAudio2SourceVoice", Start, sv_Start), M("IXAudio2SourceVoice", Stop, sv_Stop),
	M("IXAudio2SourceVoice", SubmitSourceBuffer, sv_SubmitSourceBuffer),
	M("IXAudio2SourceVoice", FlushSourceBuffers, sv_FlushSourceBuffers),
	M("IXAudio2SourceVoice", Discontinuity, sv_Discontinuity), M("IXAudio2SourceVoice", ExitLoop, sv_ExitLoop),
	M("IXAudio2SourceVoice", GetState, sv_GetState), M("IXAudio2SourceVoice", SetFrequencyRatio, sv_SetFrequencyRatio),
	M("IXAudio2SourceVoice", GetFrequencyRatio, sv_GetFrequencyRatio),
	M("IXAudio2SourceVoice", SetSourceSampleRate, sv_SetSourceSampleRate),
	M("XAPOReverb", QueryInterface, rv_QueryInterface), M("XAPOReverb", AddRef, rv_AddRef), M("XAPOReverb", Release, rv_Release),
	I_VOICE(GEN_VOICE)
};
const unsigned xrc_hle_xaudio2_n = sizeof xrc_hle_xaudio2 / sizeof xrc_hle_xaudio2[0];
