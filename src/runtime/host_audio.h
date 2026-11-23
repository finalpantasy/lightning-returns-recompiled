/* host_audio.h - a PCM stream to the host's audio output (host_audio.c) */
#ifndef HOST_AUDIO_H
#define HOST_AUDIO_H
typedef struct HostAudio HostAudio;
HostAudio *host_audio_open(unsigned rate, unsigned channels); /* 16-bit interleaved; 0: no output (or this format) */
int host_audio_write(HostAudio *a, const void *pcm, unsigned bytes); /* queues, blocking while full; 0: aborted */
void host_audio_pause(HostAudio *a, int pause);
double host_audio_time(HostAudio *a); /* seconds played so far (stops while paused) */
int host_audio_idle(HostAudio *a);    /* everything queued has played */
void host_audio_abort(HostAudio *a);  /* a blocked / later host_audio_write returns 0 */
int host_audio_try_write(HostAudio *a, const void *pcm, unsigned bytes, unsigned max_ms); /* never blocks: 0 (dropped) when more than max_ms is queued */
unsigned host_audio_queued_ms(HostAudio *a);
void host_audio_close(HostAudio *a);
/* the game is ending (its window closed, ExitProcess): every open output paused and aborted at once, and no new one
 * opens -- nothing more is heard (what was queued included) */
void host_audio_silence_all(void);
#include "xrc_quiet.h" /* (-DXRC_NO_TOOLS: nothing printed) */
#endif
