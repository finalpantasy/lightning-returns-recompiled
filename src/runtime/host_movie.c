/* host_movie.c - MP4 decoding with FFmpeg's libraries (LGPL 2.1 or later; prebuilt static libraries and their
 * provenance in third_party/ffmpeg). Used by hle_bink.c for the converted movies: H.264 4:2:0 video (already in the
 * game's YUV: BT.601 limited range, copied unchanged) and AAC 5.1 audio. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include "host_movie.h"
#include "xrc_tool_env.h"

static void quiet(void) {
	static int done;
#ifdef XRC_NO_TOOLS
	if (!done) { av_log_set_level(AV_LOG_QUIET); done = 1; } /* (nothing printed: xrc_quiet.h) */
#else
	if (!done) { av_log_set_level(xrc_tool_env("XRC_AV_LOG") ? atoi(xrc_tool_env("XRC_AV_LOG")) : AV_LOG_ERROR); done = 1; }
#endif
}

/* a demuxer + the decoder of its best stream of type t */
typedef struct {
	AVFormatContext *fc;
	AVCodecContext *cc;
	AVPacket *pkt;
	AVFrame *frm;
	int st, eof;
} Dec;
/* the stream: the nth of type t (nth >= 0), else the best */
static int dec_open(Dec *d, const char *path, enum AVMediaType t, int threads, int nth) {
	const AVCodec *codec = 0;
	int want = -1;
	memset(d, 0, sizeof *d);
	quiet();
	if (avformat_open_input(&d->fc, path, 0, 0) < 0) return 0;
	if (avformat_find_stream_info(d->fc, 0) < 0) return 0;
	if (nth >= 0) {
		unsigned k;
		for (k = 0; k < d->fc->nb_streams; k++)
			if (d->fc->streams[k]->codecpar->codec_type == t && nth-- == 0) { want = (int)k; break; }
	}
	d->st = av_find_best_stream(d->fc, t, want, -1, &codec, 0);
	if (d->st < 0 || !codec) return 0;
	if (!(d->cc = avcodec_alloc_context3(codec))) return 0;
	if (avcodec_parameters_to_context(d->cc, d->fc->streams[d->st]->codecpar) < 0) return 0;
	d->cc->thread_count = threads;
	if (avcodec_open2(d->cc, codec, 0) < 0) return 0;
	d->pkt = av_packet_alloc();
	d->frm = av_frame_alloc();
	return d->pkt && d->frm;
}
static void dec_close(Dec *d) {
	av_frame_free(&d->frm);
	av_packet_free(&d->pkt);
	avcodec_free_context(&d->cc);
	avformat_close_input(&d->fc);
}
/* the next decoded frame in d->frm; 0 at the end */
static int dec_next(Dec *d) {
	for (;;) {
		int r = avcodec_receive_frame(d->cc, d->frm);
		if (r == 0) return 1;
		if (r != AVERROR(EAGAIN)) return 0; /* (AVERROR_EOF after the drain, or an error) */
		if (d->eof) return 0;
		r = av_read_frame(d->fc, d->pkt);
		if (r < 0) {
			d->eof = 1;
			avcodec_send_packet(d->cc, 0); /* (drain the decoder's delayed frames) */
			continue;
		}
		if (d->pkt->stream_index == d->st && avcodec_send_packet(d->cc, d->pkt) < 0) { av_packet_unref(d->pkt); return 0; }
		av_packet_unref(d->pkt);
	}
}

/* ============================================================================================ video */
struct HostVideo { Dec d; unsigned w, h; int warned; };

int host_video_size(const char *path, unsigned *w, unsigned *h) {
	AVFormatContext *fc = 0;
	int st, ok = 0;
	quiet();
	if (avformat_open_input(&fc, path, 0, 0) < 0) return 0;
	if (avformat_find_stream_info(fc, 0) >= 0 && (st = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, 0, 0)) >= 0) {
		*w = (unsigned)fc->streams[st]->codecpar->width;
		*h = (unsigned)fc->streams[st]->codecpar->height;
		ok = *w && *h;
	}
	avformat_close_input(&fc);
	return ok;
}

HostVideo *host_video_open(const char *path, unsigned w, unsigned h) {
	HostVideo *v = (HostVideo *)calloc(1, sizeof *v);
	if (!v) return 0;
	v->w = w;
	v->h = h;
	if (!dec_open(&v->d, path, AVMEDIA_TYPE_VIDEO, w * h > 1280u * 720u ? 4 : 2, -1)) { /* (an upscaled movie: more threads) */
		fprintf(stderr, "xrc: movie: cannot decode the video of %s\n", path);
		dec_close(&v->d);
		free(v);
		return 0;
	}
	return v;
}

/* one plane of sw x sh from src (pitch sp) into a packed dw x dh one: cropped, the rest filled with fill */
static void plane(unsigned char *dst, unsigned dw, unsigned dh, const unsigned char *src, int sp, unsigned sw, unsigned sh, int fill) {
	unsigned y, cw = sw < dw ? sw : dw;
	for (y = 0; y < dh; y++) {
		unsigned char *o = dst + (size_t)y * dw;
		if (y < sh) {
			memcpy(o, src + (ptrdiff_t)y * sp, cw);
			if (cw < dw) memset(o + cw, fill, dw - cw);
		} else memset(o, fill, dw);
	}
}

int host_video_next(HostVideo *v, unsigned char *dst) {
	AVFrame *f;
	unsigned w = v->w, h = v->h;
	if (!dec_next(&v->d)) return 0;
	f = v->d.frm;
	/* 4:2:0 planar, 8-bit (yuvj420p is the same layout; tools/bink2mp4 writes yuv420p) */
	if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVJ420P) {
		if (!v->warned++) fprintf(stderr, "xrc: movie: video is %s, not yuv420p: not shown\n", av_get_pix_fmt_name((enum AVPixelFormat)f->format));
		return 0;
	}
	plane(dst, w, h, f->data[0], f->linesize[0], (unsigned)f->width, (unsigned)f->height, 16);
	plane(dst + w * h, w / 2, h / 2, f->data[1], f->linesize[1], (unsigned)(f->width + 1) / 2, (unsigned)(f->height + 1) / 2, 128);
	plane(dst + w * h + (w / 2) * (h / 2), w / 2, h / 2, f->data[2], f->linesize[2], (unsigned)(f->width + 1) / 2, (unsigned)(f->height + 1) / 2, 128);
	av_frame_unref(f);
	return 1;
}

void host_video_close(HostVideo *v) {
	if (!v) return;
	dec_close(&v->d);
	free(v);
}

/* ============================================================================================ audio */
struct HostAudioSrc {
	Dec d;
	SwrContext *swr;
	unsigned ch;
	short *pend;              /* converted samples not handed out yet */
	unsigned pend_n, pend_at, pend_cap;
	int flushed;
};

HostAudioSrc *host_audio_src_open(const char *path, unsigned rate, unsigned channels) { return host_audio_src_open_nth(path, rate, channels, -1); }
HostAudioSrc *host_audio_src_open_nth(const char *path, unsigned rate, unsigned channels, int nth) {
	HostAudioSrc *a = (HostAudioSrc *)calloc(1, sizeof *a);
	AVChannelLayout out;
	if (!a) return 0;
	a->ch = channels;
	if (!dec_open(&a->d, path, AVMEDIA_TYPE_AUDIO, 1, nth)) goto fail;
	av_channel_layout_default(&out, (int)channels); /* (as ffmpeg -ac N) */
	if (swr_alloc_set_opts2(&a->swr, &out, AV_SAMPLE_FMT_S16, (int)rate, &a->d.cc->ch_layout, a->d.cc->sample_fmt, a->d.cc->sample_rate, 0, 0) < 0 ||
	    swr_init(a->swr) < 0)
		goto fail;
	av_channel_layout_uninit(&out);
	return a;
fail:
	fprintf(stderr, "xrc: movie: cannot decode the audio of %s\n", path);
	host_audio_src_close(a);
	return 0;
}

/* converts the next decoded frame (or, at the end, what the resampler still holds) into pend; 0 when nothing is left */
static int refill(HostAudioSrc *a) {
	const uint8_t **in = 0;
	int nin = 0, room, got;
	uint8_t *o;
	if (!a->flushed) {
		if (dec_next(&a->d)) { in = (const uint8_t **)a->d.frm->extended_data; nin = a->d.frm->nb_samples; }
		else a->flushed = 1; /* (the end: drain the resampler once) */
	} else return 0;
	room = swr_get_out_samples(a->swr, nin);
	if (room <= 0) room = 1024;
	if ((unsigned)room > a->pend_cap) {
		short *p = (short *)realloc(a->pend, (size_t)room * a->ch * sizeof(short));
		if (!p) return 0;
		a->pend = p;
		a->pend_cap = (unsigned)room;
	}
	o = (uint8_t *)a->pend;
	got = swr_convert(a->swr, &o, room, in, nin);
	if (in) av_frame_unref(a->d.frm);
	a->pend_n = got > 0 ? (unsigned)got : 0;
	a->pend_at = 0;
	return a->pend_n > 0 || !a->flushed;
}

unsigned host_audio_src_read(HostAudioSrc *a, short *dst, unsigned max) {
	unsigned n = 0;
	while (n < max) {
		unsigned k;
		if (a->pend_at == a->pend_n && !refill(a)) break;
		k = a->pend_n - a->pend_at;
		if (k > max - n) k = max - n;
		memcpy(dst + (size_t)n * a->ch, a->pend + (size_t)a->pend_at * a->ch, (size_t)k * a->ch * sizeof(short));
		a->pend_at += k;
		n += k;
	}
	return n;
}

void host_audio_src_close(HostAudioSrc *a) {
	if (!a) return;
	swr_free(&a->swr);
	dec_close(&a->d);
	free(a->pend);
	free(a);
}
