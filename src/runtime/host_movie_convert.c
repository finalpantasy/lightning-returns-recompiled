/* host_movie_convert.c - converting the movies in the background while the game runs (Windows only: the converter,
 * tools/bink2mp4, is a 32-bit Windows program around the game's own bink2w32.dll, with an ffmpeg program encoding).
 *
 * hle_bink.c starts it when the game first opens a movie. One host thread converts the movies that have no MP4 yet,
 * one at a time, by name: its process (and the ffmpeg processes it runs) in a job object that holds them to
 * XRC_MOVIE_THREADS CPUs (a compile define, default 1: the launcher's "Cutscene Convert Threads") at idle priority,
 * with ffmpeg's threads at as many (--threads N). A movie the game opens
 * meanwhile without its MP4 is skipped (hle_bink.c); the converter writes only temporary files and renames the MP4 into
 * place when it is complete, so the game never sees a partial one.
 *
 * When the game ends (exit: atexit; Ctrl-C / console close: host_movie_convert_stop from xrc_rt.c) the job is
 * terminated -- the converter and its ffmpeg children with it -- and the movie in progress's temporary files deleted.
 * If the game is killed outright, the converter sees it (--watch: it waits on the game's process) and does the same
 * itself: kills its ffmpeg processes, deletes its temporary files, ends. Anything still left (the converter killed
 * too) is deleted when the conversion starts next time.
 *
 * Finding the tools: XRC_MOVIE_CONVERTER (bink2mp4.exe; default: beside the game's exe, then ..\tools\ -- the
 * launcher's layout), XRC_MOVIE_FFMPEG (an ffmpeg with libx264; default: ffmpeg on PATH). Without either nothing is
 * started. XRC_MOVIE_NO_CONVERT=1: never. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host_movie_convert.h"
#ifndef XRC_MOVIE_THREADS
#define XRC_MOVIE_THREADS 1 /* the CPUs (the last ones) and ffmpeg threads the conversion has: the launcher's option */
#endif

#ifdef XRC_LR
/* ============================================================================================ Lightning Returns
 * LR's movies are Bink 2 streams ("KB2j") packed back to back, each at a 0x800 boundary, in
 * weiss_data\movie\movie{,2,3}{v,a}.win32.bin (the lists naming them, sys\movielist*, are mostly encrypted: not
 * needed). The index here walks the streams' headers; the game's movie is the stream whose first 0x40 bytes its
 * reader returns. Each converts to <movie dir>\xrc\<archive>@<offset>.mp4. */
#ifdef _WIN32
#define LR_SEP "\\"
#else
#define LR_SEP "/"
#endif
#define LR_MAX 256
static const char *const lr_arcs[] = {"moviev", "moviea", "movie2v", "movie2a", "movie3v", "movie3a"};
static LrMovie lrm[LR_MAX];
static int nlrm = -1;
static char lr_dir[1024];
static unsigned lr_u32(const unsigned char *p) { return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24; }
static int lr_seek(FILE *f, long long off) {
#ifdef _WIN32
	return _fseeki64(f, off, SEEK_SET);
#else
	return fseeko(f, (off_t)off, SEEK_SET);
#endif
}
int host_movie_lr_index(const char *movie_dir, const LrMovie **out) {
	size_t a;
	if (nlrm >= 0) { *out = lrm; return nlrm; }
	nlrm = 0;
	snprintf(lr_dir, sizeof lr_dir, "%s", movie_dir);
	for (a = 0; a < sizeof lr_arcs / sizeof lr_arcs[0]; a++) {
		char p[1100];
		long long off = 0;
		FILE *f;
		snprintf(p, sizeof p, "%s" LR_SEP "%s.win32.bin", lr_dir, lr_arcs[a]);
		f = fopen(p, "rb");
		if (!f) continue;
		while (nlrm < LR_MAX && lr_seek(f, off) == 0) {
			LrMovie *m = &lrm[nlrm];
			if (fread(m->hd, 1, sizeof m->hd, f) != sizeof m->hd || memcmp(m->hd, "KB2", 3)) break;
			snprintf(m->arc, sizeof m->arc, "%s", lr_arcs[a]);
			m->off = off;
			m->size = (long long)lr_u32(m->hd + 4) + 8;
			nlrm++;
			off = (off + m->size + 0x7ff) & ~0x7ffLL;
		}
		fclose(f);
	}
	*out = lrm;
	return nlrm;
}
void host_movie_lr_mp4(int i, char *out, size_t n) {
	snprintf(out, n, "%s" LR_SEP "xrc" LR_SEP "%s@%010llx.mp4", lr_dir, lrm[i].arc, (unsigned long long)lrm[i].off);
}
void host_movie_lr_source(int i, char *out, size_t n) { snprintf(out, n, "%s" LR_SEP "%s.win32.bin", lr_dir, lrm[i].arc); }
#endif

#ifdef _WIN32
#include <windows.h>
#ifdef XRC_LR
#include <libavformat/avformat.h> /* (lr_mix: the converter's audio streams counted) */
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation" /* (paths: a truncated one just does not exist) */
#endif

#define MAXMOV 256
#define PATHN 1024 /* (paths: longer than MAX_PATH is fine with the ANSI calls on a long-path system) */
static char dir[PATHN], tool[PATHN], ffmpeg[PATHN], dll[PATHN];
static char todo[MAXMOV][PATHN]; /* the .wmp names (in dir) to convert */
static int ntodo;
static HANDLE job, stop_ev, worker, log_h = INVALID_HANDLE_VALUE; /* (log_h: <dir>\convert.log, the tools' output) */
static long last_code; /* (the failed job's exit code, for its line) */
static volatile LONG cur = -1; /* the movie being converted (an index into todo), -1: none */
static volatile LONG started;
static volatile LONG want = -1; /* (LR: the stream the game asked for, converted next) */
static char done_flag[MAXMOV];
#ifdef XRC_LR
static int lr_mode, lr_todo[MAXMOV]; /* (LR: todo[i] is a name, lr_todo[i] the stream) */
#endif

static int file_exists(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

/* the MP4 of todo[i] (<name>.mp4 for <name>.wmp, as the converter's "all" mode names them) */
static void mp4_of(int i, char *out) {
	size_t n;
	snprintf(out, PATHN, "%s\\%s", dir, todo[i]);
	n = strlen(out);
	if (n > 4) memcpy(out + n - 4, ".mp4", 4);
}

/* the converter's temporary files for out (OUT.video.tmp.mp4, OUT.trackN.tmp.pcm, OUT.tmp.mp4) */
static void delete_temps(const char *out) {
	char p[PATHN + 32];
	int k;
	snprintf(p, sizeof p, "%s.video.tmp.mp4", out); DeleteFileA(p);
	snprintf(p, sizeof p, "%s.tmp.mp4", out); DeleteFileA(p);
	for (k = 0; k < 32; k++) { snprintf(p, sizeof p, "%s.track%d.tmp.pcm", out, k); DeleteFileA(p); }
	snprintf(p, sizeof p, "%s.tmp.wmp", out); DeleteFileA(p); /* (LR: the stream copied out, */
	snprintf(p, sizeof p, "%s.mix.tmp.mp4", out); DeleteFileA(p); /* its audio being mixed, */
	snprintf(p, sizeof p, "%s.raw.mp4", out); /* its unmixed MP4 and that one's temporary files) */
	if (strlen(out) + 8 < sizeof p - 32 && !strstr(out, ".raw.mp4")) { DeleteFileA(p); delete_temps(p); }
}
/* job i's input and MP4 */
static void job_paths(int i, char *in, char *out) {
#ifdef XRC_LR
	if (lr_mode) { host_movie_lr_mp4(lr_todo[i], out, PATHN); snprintf(in, PATHN, "%s.tmp.wmp", out); return; }
#endif
	snprintf(in, PATHN, "%s\\%s", dir, todo[i]);
	mp4_of(i, out);
}
#ifdef XRC_LR
/* the stream copied out of its archive as a .wmp (FFXIII's 16-byte header, then the stream): 1 done, 0 failed,
 * 2 stopped */
static int lr_extract(int s, const char *in) {
	static const unsigned char wmp[16] = {'W', 'M', 'P', 0, 'V', 'e', 'r', ':', '0', '.', '0', '1', 0, 0, 0, 0};
	char src[PATHN];
	const LrMovie *ix;
	long long left;
	FILE *f, *o;
	char *buf;
	int r = 1;
	host_movie_lr_index("", &ix);
	host_movie_lr_source(s, src, sizeof src);
	f = fopen(src, "rb");
	if (!f) return 0;
	o = fopen(in, "wb");
	buf = (char *)malloc(1 << 22);
	if (!o || !buf || _fseeki64(f, ix[s].off, SEEK_SET) || fwrite(wmp, 1, 16, o) != 16) r = 0;
	for (left = ix[s].size; r == 1 && left > 0;) {
		size_t n = left > (1 << 22) ? (1 << 22) : (size_t)left;
		if (WaitForSingleObject(stop_ev, 0) == WAIT_OBJECT_0) r = 2;
		else if (fread(buf, 1, n, f) != n || fwrite(buf, 1, n, o) != n) r = 0;
		else left -= (long long)n;
	}
	free(buf);
	fclose(f);
	if (o && fclose(o)) r = r == 1 ? 0 : r;
	if (r != 1) DeleteFileA(in);
	return r;
}
static long run_in_job(char *cmd, int i);
/* LR's movies carry their audio as sets of 4 Bink tracks (stereo front, centre, LFE, stereo surround: 5.1 split up),
 * one set per language. The launcher's bink2mp4 mixes each set into one 5.1 stream itself; an older one writes each
 * track as its own stream, and then each set is mixed here into one 5.1 stream (the player plays the set the game
 * asks for, by its index), the video copied: raw -> out. 0 done, -2 stopped, else failed. A movie whose tracks are
 * not sets of 4, or whose MP4 already has a stream per set, is kept as it is. */
static int audio_streams(const char *path) {
	AVFormatContext *fc = 0;
	int k, a = 0;
	if (avformat_open_input(&fc, path, 0, 0) < 0) return -1;
	for (k = 0; k < (int)fc->nb_streams; k++) a += fc->streams[k]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
	avformat_close_input(&fc);
	return a;
}
static long lr_mix(int s, const char *raw, const char *out, char *cmd, size_t n, int i) {
	const LrMovie *ix;
	char tmp[PATHN + 16], *p;
	unsigned tracks, k;
	long code;
	host_movie_lr_index("", &ix);
	tracks = ix[s].hd[0x28] | (unsigned)ix[s].hd[0x29] << 8;
	if (!tracks || tracks % 4 || tracks > 32 || audio_streams(raw) != (int)tracks) return MoveFileA(raw, out) ? 0 : 1;
	snprintf(tmp, sizeof tmp, "%s.mix.tmp.mp4", out);
	p = cmd + snprintf(cmd, n, "\"%s\" -hide_banner -loglevel error -y -threads %d -i \"%s\" -filter_complex \"", ffmpeg, XRC_MOVIE_THREADS, raw);
	for (k = 0; k < tracks / 4; k++)
		p += snprintf(p, n - (size_t)(p - cmd), "%s[0:a:%u][0:a:%u][0:a:%u][0:a:%u]amerge=inputs=4,pan=5.1(side)|FL=c0|FR=c1|FC=c2|LFE=c3|SL=c4|SR=c5[s%u]",
		              k ? ";" : "", 4 * k, 4 * k + 1, 4 * k + 2, 4 * k + 3, k);
	p += snprintf(p, n - (size_t)(p - cmd), "\" -map 0:v -c:v copy");
	for (k = 0; k < tracks / 4; k++) p += snprintf(p, n - (size_t)(p - cmd), " -map [s%u]", k);
	snprintf(p, n - (size_t)(p - cmd), " -c:a aac -b:a 384k -movflags +faststart \"%s\"", tmp);
	code = run_in_job(cmd, i);
	if (code == -2) return -2;
	DeleteFileA(raw);
	if (code != 0 || !file_exists(tmp) || !MoveFileA(tmp, out)) { DeleteFileA(tmp); return code ? code : 1; }
	return 0;
}
#endif

/* the movie has its stream (a WMP header and a Bink 2 one: the 16-byte stubs have none) */
static int is_movie(const char *path) {
	unsigned char hd[0x30];
	FILE *f = fopen(path, "rb");
	size_t n = f ? fread(hd, 1, sizeof hd, f) : 0;
	if (f) fclose(f);
	return n == sizeof hd && !memcmp(hd, "WMP", 3) && !memcmp(hd + 16, "KB2", 3);
}

static int cmp_name(const void *a, const void *b) { return _stricmp((const char *)a, (const char *)b); }

/* runs cmd in the job (idle priority, no window) and waits: its exit code, or -2 when the game is ending (the job is
 * then ended by host_movie_convert_stop) */
static long run_in_job(char *cmd, int i) {
	STARTUPINFOEXA si;
	PROCESS_INFORMATION pi;
	HANDLE hs[2];
	DWORD code = 1, w, flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | IDLE_PRIORITY_CLASS;
	char attr[256];
	SIZE_T an = sizeof attr;
	BOOL inherit = FALSE, ok;
	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si.StartupInfo;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES; /* (output: the log, else none) */
	if (log_h != INVALID_HANDLE_VALUE) {
		char line[64];
		DWORD wr;
		snprintf(line, sizeof line, "\r\n== job %d ==\r\n", i);
		WriteFile(log_h, line, (DWORD)strlen(line), &wr, 0);
		WriteFile(log_h, cmd, (DWORD)strlen(cmd), &wr, 0);
		WriteFile(log_h, "\r\n", 2, &wr, 0);
		si.StartupInfo.hStdOutput = si.StartupInfo.hStdError = log_h;
		/* (the log the only handle it inherits) */
		if (InitializeProcThreadAttributeList((LPPROC_THREAD_ATTRIBUTE_LIST)attr, 1, 0, &an) &&
		    UpdateProcThreadAttribute((LPPROC_THREAD_ATTRIBUTE_LIST)attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log_h, sizeof log_h, 0, 0)) {
			si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attr;
			flags |= EXTENDED_STARTUPINFO_PRESENT;
			inherit = TRUE;
		}
	}
	/* suspended until it is in the job, so no process of it starts outside */
	ok = CreateProcessA(0, cmd, 0, 0, inherit, flags, 0, 0, &si.StartupInfo, &pi);
	if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
	if (!ok) return 1;
	if (!AssignProcessToJobObject(job, pi.hProcess)) { TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return 1; }
	InterlockedExchange(&cur, i);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	hs[0] = stop_ev;
	hs[1] = pi.hProcess;
	w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
	if (w == WAIT_OBJECT_0) { CloseHandle(pi.hProcess); return -2; }
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	return (long)code;
}

/* one movie: 0 converted, 1 failed, 2 stopped */
static int convert_one(int i) {
	char in[PATHN], out[PATHN], conv[PATHN + 16], cmd[6 * PATHN + 512];
	long code;
	job_paths(i, in, out);
	if (file_exists(out)) return 0;
	delete_temps(out);
	snprintf(conv, sizeof conv, "%s", out);
#ifdef XRC_LR
	if (lr_mode) {
		int e = lr_extract(lr_todo[i], in);
		if (e != 1) return e == 2 ? 2 : 1;
		snprintf(conv, sizeof conv, "%s.raw.mp4", out); /* (its audio tracks mixed after: lr_mix) */
	}
#endif
	snprintf(cmd, sizeof cmd, "\"%s\" --dll \"%s\" --ffmpeg \"%s\" --threads %d --watch %lu convert \"%s\" \"%s\"", tool, dll, ffmpeg, XRC_MOVIE_THREADS,
	         (unsigned long)GetCurrentProcessId(), in, conv);
	code = run_in_job(cmd, i);
	if (code == -2) return 2;
#ifdef XRC_LR
	if (lr_mode) {
		DeleteFileA(in);
		if (code == 0 && file_exists(conv)) code = lr_mix(lr_todo[i], conv, out, cmd, sizeof cmd, i);
		if (code == -2) return 2;
	}
#endif
	InterlockedExchange(&cur, -1);
	if (code != 0 || !file_exists(out)) { last_code = code; delete_temps(out); return 1; }
	return 0;
}

/* XRC_MOVIE_HD_HEIGHT=1440|2160 (the launcher's Cinematics option): each MP4 also gets <name>.hd.mp4, its picture
 * scaled to that height (Lanczos, then contrast-adaptive sharpening; NVENC H.264, libx264 if NVENC fails), which
 * hle_bink.c plays instead at that size. Made after each conversion, then for every MP4 that has none */
static int hd_h = -1;
static char hd_tmp[PATHN]; /* (the one being made: deleted if stopped) */
static int hd_height(void) {
	if (hd_h < 0) {
		const char *e = getenv("XRC_MOVIE_HD_HEIGHT");
		hd_h = e ? atoi(e) : 0;
		if (hd_h && (hd_h <= 720 || hd_h > 2160)) hd_h = 0;
	}
	return hd_h;
}
/* mp4's upscaled copy: 0 made (or there already), 1 failed, 2 stopped */
static int upscale_one(const char *mp4) {
	char hd[PATHN], cmd[3 * PATHN + 640];
	size_t n = strlen(mp4);
	long code = 1;
	int enc;
	if (n < 5 || n + 8 >= PATHN) return 1;
	memcpy(hd, mp4, n - 4); strcpy(hd + n - 4, ".hd.mp4");
	if (file_exists(hd)) return 0;
	memcpy(hd_tmp, mp4, n - 4); strcpy(hd_tmp + n - 4, ".hd.tmp.mp4");
	for (enc = 0; enc < 2; enc++) {
		snprintf(cmd, sizeof cmd,
		         "\"%s\" -nostdin -v error -y -i \"%s\" -map 0:v:0 -map 0:a? -vf \"scale=-2:%d:flags=lanczos:out_range=tv,cas=0.4\" %s -pix_fmt yuv420p "
		         "-colorspace smpte170m -color_range tv -c:a copy -metadata comment=xrc-hd:lanczos -movflags +faststart \"%s\"",
		         ffmpeg, mp4, hd_h,
		         enc ? "-c:v libx264 -preset medium -crf 20" : "-c:v h264_nvenc -preset p6 -rc vbr -cq 23 -b:v 0 -maxrate 30M -bufsize 60M -profile:v high", hd_tmp);
		code = run_in_job(cmd, -1);
		if (code == -2) { DeleteFileA(hd_tmp); hd_tmp[0] = 0; return 2; }
		if (code == 0 && file_exists(hd_tmp)) break;
		DeleteFileA(hd_tmp);
	}
	if (enc == 2 || !MoveFileA(hd_tmp, hd)) { DeleteFileA(hd_tmp); hd_tmp[0] = 0; last_code = code; return 1; }
	hd_tmp[0] = 0;
	return 0;
}
/* every MP4 without its upscaled copy: 2 stopped */
static int upscale_all(void) {
	char pat[PATHN], p[PATHN];
	WIN32_FIND_DATAA fd;
	HANDLE h;
	int made = 0, failed = 0, r;
#ifdef XRC_LR
	int sub = lr_mode;
#else
	int sub = 0;
#endif
	snprintf(pat, sizeof pat, sub ?"%s\\xrc\\*.mp4" : "%s\\*.mp4", dir);
	if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
	do {
		if (strstr(fd.cFileName, ".hd.") || strstr(fd.cFileName, ".raw.") || strstr(fd.cFileName, ".tmp.")) continue;
		snprintf(p, sizeof p, sub ?"%s\\xrc\\%s" : "%s\\%s", dir, fd.cFileName);
		if (strlen(p) > 4) {
			char hd[PATHN];
			size_t n = strlen(p);
			memcpy(hd, p, n - 4); strcpy(hd + n - 4, ".hd.mp4");
			if (file_exists(hd)) continue;
		}
		r = upscale_one(p);
		if (r == 2) { FindClose(h); return 2; }
		if (r == 0) { made++; fprintf(stderr, "xrc: movies: upscaled %s to %dp\n", fd.cFileName, hd_h); }
		else { failed++; fprintf(stderr, "xrc: movies: %s was not upscaled (exit %ld; see %s\\convert.log)\n", fd.cFileName, last_code, dir); if (failed >= 2) break; }
	} while (FindNextFileA(h, &fd));
	FindClose(h);
	if (made || failed) fprintf(stderr, "xrc: movies: upscaling over: %d upscaled, %d failed\n", made, failed);
	return 0;
}

/* the next job: the one wanted, else the first not done; -1: none */
static int next_job(void) {
	LONG w = InterlockedExchange(&want, -1);
	int i;
#ifdef XRC_LR
	if (lr_mode && w >= 0) {
		for (i = 0; i < ntodo; i++) if (lr_todo[i] == w && !done_flag[i]) return i;
		w = -1;
	}
#endif
	if (w >= 0 && w < ntodo && !done_flag[w]) return (int)w;
	for (i = 0; i < ntodo; i++) if (!done_flag[i]) return i;
	return -1;
}
void host_movie_convert_want(int i) { InterlockedExchange(&want, i); }

static DWORD WINAPI work(LPVOID arg) {
	int i, done = 0, failed = 0, fails_in_row = 0;
	(void)arg;
	while ((i = next_job()) >= 0) {
		int r;
		done_flag[i] = 1;
		r = convert_one(i);
		if (r == 2) return 0;
		if (r == 0) {
			done++; fails_in_row = 0; fprintf(stderr, "xrc: movies: converted %s (%d of %d)\n", todo[i], done, ntodo);
			if (hd_height()) {
				char in[PATHN], out[PATHN];
				job_paths(i, in, out);
				r = upscale_one(out);
				if (r == 2) return 0;
				if (r == 0) fprintf(stderr, "xrc: movies: upscaled %s to %dp\n", todo[i], hd_h);
			}
		}
		else {
			failed++;
			fprintf(stderr, "xrc: movies: %s did not convert (exit %ld; its output: %s\\convert.log)\n", todo[i], last_code, dir);
			if (++fails_in_row >= 2) { fprintf(stderr, "xrc: movies: two failed in a row: the background conversion stops\n"); break; }
		}
	}
	if (ntodo) fprintf(stderr, "xrc: movies: background conversion over: %d converted, %d failed\n", done, failed);
	if (hd_height()) upscale_all();
	return 0;
}

void host_movie_convert_stop(void) {
	LONG i;
	int k;
	if (!InterlockedExchange(&started, 0)) return;
	SetEvent(stop_ev);
	TerminateJobObject(job, 1); /* (the converter and its ffmpeg processes) */
	if (worker) { WaitForSingleObject(worker, 5000); CloseHandle(worker); worker = 0; }
	/* the processes gone (they hold the files open until then) */
	for (k = 0; k < 100; k++) {
		JOBOBJECT_BASIC_ACCOUNTING_INFORMATION ai;
		if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &ai, sizeof ai, 0) || !ai.ActiveProcesses) break;
		Sleep(20);
	}
	if (hd_tmp[0]) { DeleteFileA(hd_tmp); hd_tmp[0] = 0; }
	i = InterlockedExchange(&cur, -1);
	if (i >= 0) {
		char in[PATHN], out[PATHN];
		job_paths(i, in, out);
		delete_temps(out);
		fprintf(stderr, "xrc: movies: the conversion of %s was stopped (its partial files deleted)\n", todo[i]);
	}
}
static void at_exit(void) { host_movie_convert_stop(); }

/* the tool: path given, or beside the exe, or ..\tools\ */
static int find_tool(void) {
	char exe[PATHN], *sl;
	const char *e = getenv("XRC_MOVIE_CONVERTER");
	if (e && *e) { snprintf(tool, sizeof tool, "%s", e); return file_exists(tool); }
	if (!GetModuleFileNameA(0, exe, (DWORD)sizeof exe)) return 0;
	sl = strrchr(exe, '\\');
	if (!sl) return 0;
	*sl = 0;
	snprintf(tool, sizeof tool, "%s\\bink2mp4.exe", exe);
	if (file_exists(tool)) return 1;
	snprintf(tool, sizeof tool, "%s\\..\\tools\\bink2mp4.exe", exe);
	return file_exists(tool);
}
static int find_ffmpeg(void) {
	const char *e = getenv("XRC_MOVIE_FFMPEG");
	char *part;
	if (e && *e) { snprintf(ffmpeg, sizeof ffmpeg, "%s", e); return file_exists(ffmpeg); }
	return SearchPathA(0, "ffmpeg.exe", 0, sizeof ffmpeg, ffmpeg, &part) > 0;
}

static void begin(void);
void host_movie_convert_start(const char *movie_dir) {
	static int tried;
	WIN32_FIND_DATAA fd;
	HANDLE fh;
	char pat[PATHN + 8], p[PATHN];
	int k;
	if (tried) return;
	tried = 1;
	if (getenv("XRC_MOVIE_NO_CONVERT")) return;
	snprintf(dir, sizeof dir, "%s", movie_dir);
	for (k = 0; dir[k]; k++) if (dir[k] == '/') dir[k] = '\\';
	/* the movies without an MP4 (and stale temporary files of a conversion that was killed) */
	snprintf(pat, sizeof pat, "%s\\*.wmp", dir);
	fh = FindFirstFileA(pat, &fd);
	if (fh == INVALID_HANDLE_VALUE) return;
	do {
		size_t n = strlen(fd.cFileName);
		char out[PATHN];
		if (n < 5 || n >= PATHN - 8) continue;
		snprintf(p, sizeof p, "%s\\%s", dir, fd.cFileName);
		snprintf(out, sizeof out, "%.*s.mp4", (int)(strlen(p) - 4), p);
		delete_temps(out);
		if (file_exists(out) || !is_movie(p) || ntodo == MAXMOV) continue;
		snprintf(todo[ntodo++], PATHN, "%s", fd.cFileName);
	} while (FindNextFileA(fh, &fd));
	FindClose(fh);
	if (!ntodo) return; /* (all converted) */
	qsort(todo, (size_t)ntodo, sizeof todo[0], cmp_name);
	snprintf(dll, sizeof dll, "%s\\..\\prog\\win\\bin\\bink2w32.dll", dir);
	begin();
}

#ifdef XRC_LR
void host_movie_convert_start_lr(int first) {
	static int tried;
	const LrMovie *ix;
	int n, i, j;
	char out[PATHN];
	if (tried) { if (first >= 0) host_movie_convert_want(first); return; }
	tried = 1;
	if (getenv("XRC_MOVIE_NO_CONVERT")) return;
	n = host_movie_lr_index("", &ix);
	if (n <= 0) return;
	lr_mode = 1;
	snprintf(dir, sizeof dir, "%s", lr_dir);
	for (i = 0; dir[i]; i++) if (dir[i] == '/') dir[i] = '\\';
	snprintf(out, sizeof out, "%s\\xrc", dir);
	CreateDirectoryA(out, 0);
	/* the streams without an MP4 (one of each: a stream with the same header and size as an earlier one is that one;
	 * the game's lookup finds the first), stale temporary files deleted */
	for (i = 0; i < n && ntodo < MAXMOV; i++) {
		for (j = 0; j < i; j++) if (ix[j].size == ix[i].size && !memcmp(ix[j].hd, ix[i].hd, sizeof ix[i].hd)) break;
		if (j < i) continue;
		host_movie_lr_mp4(i, out, sizeof out);
		delete_temps(out);
		if (file_exists(out)) continue;
		lr_todo[ntodo] = i;
		snprintf(todo[ntodo++], PATHN, "%s@%010llx", ix[i].arc, (unsigned long long)ix[i].off);
	}
	if (!ntodo && !hd_height()) return;
	snprintf(dll, sizeof dll, "%s\\..\\..\\bink2w32.dll", dir); /* (LR's, beside its exe) */
	if (first >= 0) want = first;
	begin();
}
int host_movie_convert_all_lr(const char *movie_dir) {
	const LrMovie *ix;
	if (host_movie_lr_index(movie_dir, &ix) <= 0) { fprintf(stderr, "xrc: movies: no movie archives in %s\n", movie_dir); return 1; }
	host_movie_convert_start_lr(-1);
	if (!started) { fprintf(stderr, "xrc: movies: nothing to do\n"); return 0; }
	WaitForSingleObject(worker, INFINITE);
	return 0;
}
#endif

/* the converter's job and its thread (todo, dir and dll set) */
static void begin(void) {
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
	SYSTEM_INFO si;
	if (!find_tool()) { fprintf(stderr, "xrc: movies: %d have no MP4, and there is no converter (XRC_MOVIE_CONVERTER): they are skipped\n", ntodo); return; }
	if (!find_ffmpeg()) { fprintf(stderr, "xrc: movies: %d have no MP4, and there is no ffmpeg to convert them (XRC_MOVIE_FFMPEG): they are skipped\n", ntodo); return; }
	if (!file_exists(dll)) { fprintf(stderr, "xrc: movies: no %s: cannot convert\n", dll); return; }
	/* the job: killed with the game, XRC_MOVIE_THREADS CPUs (the last ones), idle priority */
	job = CreateJobObjectA(0, 0);
	if (!job) return;
	memset(&li, 0, sizeof li);
	GetSystemInfo(&si);
	/* (not KILL_ON_JOB_CLOSE: if the game is killed outright, the converter -- --watch -- ends its processes and deletes
	 * its files itself; killed with the game it could not) */
	li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PRIORITY_CLASS | JOB_OBJECT_LIMIT_AFFINITY;
	li.BasicLimitInformation.PriorityClass = IDLE_PRIORITY_CLASS;
	{
		DWORD_PTR proc_mask = 0, sys_mask = 0, bit, mask = 0;
		int left = XRC_MOVIE_THREADS;
		GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask);
		for (bit = (DWORD_PTR)1 << (sizeof(DWORD_PTR) * 8 - 1); bit && left > 0; bit >>= 1)
			if (proc_mask & bit) { mask |= bit; left--; }
		li.BasicLimitInformation.Affinity = mask ? mask : 1;
	}
	if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li)) {
		li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PRIORITY_CLASS;
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
	}
	{
		SECURITY_ATTRIBUTES sa = {sizeof sa, 0, TRUE};
		char lp[PATHN + 16];
		snprintf(lp, sizeof lp, "%s\\convert.log", dir);
		log_h = CreateFileA(lp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
	}
	stop_ev = CreateEventA(0, TRUE, FALSE, 0);
	worker = CreateThread(0, 0, work, 0, 0, 0);
	if (!worker) return;
	SetThreadPriority(worker, THREAD_PRIORITY_IDLE);
	started = 1;
	atexit(at_exit);
	fprintf(stderr, "xrc: movies: converting %d movies in the background (one at a time, %d CPU(s), idle priority) with %s\n", ntodo, XRC_MOVIE_THREADS, tool);
}

#else
void host_movie_convert_start(const char *movie_dir) { (void)movie_dir; } /* (the converter is a Windows program) */
void host_movie_convert_stop(void) {}
#ifdef XRC_LR
void host_movie_convert_start_lr(int first) { (void)first; }
void host_movie_convert_want(int i) { (void)i; }
#endif
#endif
