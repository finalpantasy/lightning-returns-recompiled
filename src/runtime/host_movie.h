/* host_movie.h - an MP4's video and audio decoded in-process (FFmpeg's libavformat / libavcodec / libswresample,
 * statically linked from third_party/ffmpeg). Two independent sources, one per host thread (each has its own demuxer:
 * neither is shared between threads). */
#ifndef HOST_MOVIE_H
#define HOST_MOVIE_H

typedef struct HostVideo HostVideo;
typedef struct HostAudioSrc HostAudioSrc;

/* the first video stream of path, as yuv420p frames of w x h (the movie's own size: larger frames are cropped,
 * smaller ones padded black); 0 if it cannot be opened or decoded */
HostVideo *host_video_open(const char *path, unsigned w, unsigned h);
/* the size of path's video stream; 0 if it has none */
int host_video_size(const char *path, unsigned *w, unsigned *h);
/* the next frame into dst (w * h Y, then (w / 2) * (h / 2) U, then V: packed); 0 at the end or on an error */
int host_video_next(HostVideo *v, unsigned char *dst);
void host_video_close(HostVideo *v);

/* the first audio stream of path, as interleaved signed 16-bit PCM at rate Hz with channels channels (FFmpeg's default
 * layout for the count: 6 = 5.1, 2 = stereo, a downmix of 5.1); 0 if it has none or it cannot be decoded */
HostAudioSrc *host_audio_src_open(const char *path, unsigned rate, unsigned channels);
/* the same, the nth audio stream (nth < 0: the best) */
HostAudioSrc *host_audio_src_open_nth(const char *path, unsigned rate, unsigned channels, int nth);
/* up to max sample frames into dst; the number written, 0 at the end */
unsigned host_audio_src_read(HostAudioSrc *a, short *dst, unsigned max);
void host_audio_src_close(HostAudioSrc *a);

#include "xrc_quiet.h" /* (-DXRC_NO_TOOLS: nothing printed) */
#endif
