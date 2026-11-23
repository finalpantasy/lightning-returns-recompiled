/* host_movie_convert.h - the movies converted to MP4 in the background (host_movie_convert.c; Windows only) */
#ifndef HOST_MOVIE_CONVERT_H
#define HOST_MOVIE_CONVERT_H
/* once: converts the movies in movie_dir (*.wmp) that have no MP4 yet, one at a time, on one CPU at idle priority */
void host_movie_convert_start(const char *movie_dir);
/* ends a running conversion (its processes, its partial files); also at exit */
void host_movie_convert_stop(void);
#ifdef XRC_LR
/* Lightning Returns: the Bink 2 streams in movie_dir's archives (indexed once), each with its first 0x40 bytes */
typedef struct { char arc[16]; long long off, size; unsigned char hd[0x40]; } LrMovie;
int host_movie_lr_index(const char *movie_dir, const LrMovie **out);
void host_movie_lr_mp4(int i, char *out, size_t n);    /* the MP4 of stream i: <movie dir>\xrc\<archive>@<offset>.mp4 */
void host_movie_lr_source(int i, char *out, size_t n); /* its archive */
/* once: converts the streams without an MP4, stream `first` first; host_movie_convert_want: that one next */
void host_movie_convert_start_lr(int first);
void host_movie_convert_want(int i);
/* lr.exe --convert-movies <movie dir>: every stream converted (and upscaled, XRC_MOVIE_HD_HEIGHT) without the game;
 * the process's exit code: 0 */
int host_movie_convert_all_lr(const char *movie_dir);
#endif
#include "xrc_quiet.h" /* (-DXRC_NO_TOOLS: nothing printed) */
#endif
