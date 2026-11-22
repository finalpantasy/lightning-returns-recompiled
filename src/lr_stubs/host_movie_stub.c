/* host_movie_stub.c - host_movie.h without FFmpeg: every movie fails to open (the game's no-movie path).
 * For builds before third_party/ffmpeg is set up; replaces src/runtime/host_movie.c. */
#include <stddef.h>
#include "host_movie.h"
HostVideo *host_video_open(const char *path, unsigned w, unsigned h) { (void)path; (void)w; (void)h; return NULL; }
int host_video_next(HostVideo *v, unsigned char *dst) { (void)v; (void)dst; return 0; }
void host_video_close(HostVideo *v) { (void)v; }
HostAudioSrc *host_audio_src_open(const char *path, unsigned rate, unsigned channels) { (void)path; (void)rate; (void)channels; return NULL; }
unsigned host_audio_src_read(HostAudioSrc *a, short *dst, unsigned max) { (void)a; (void)dst; (void)max; return 0; }
void host_audio_src_close(HostAudioSrc *a) { (void)a; }
