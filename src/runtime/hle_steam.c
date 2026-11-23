/* hle_steam.c - LR's steam_api.dll, forwarded to Valve's 64-bit steam_api64.dll (next to the runtime's exe).
 *
 * The game is 32-bit and the runtime 64-bit, so the game's own steam_api.dll cannot load here. The real Steam
 * client still does the work: SteamAPI_InitFlat (Steam must be running and the account must own the game), the
 * interfaces at the exact versions the game was built against (SteamClient()->GetISteam*(..., "...007")), and
 * callbacks through manual dispatch, delivered to the game's own callback objects.
 *
 * Interfaces: the game gets a guest object per interface whose vtable entries are synthetic imports
 * ("ISteamApps007::m4"); each runs the same slot of the host object of the same version (thiscall here: ecx =
 * this, the callee pops). Slots LR calls (from its Steam wrapper at 0x00a03700):
 *   ISteamRemoteStorage013  0 FileWrite, 1 FileRead, 6 FileDelete, 15 GetFileSize, 16 GetFileTimestamp,
 *                           18 GetFileCount, 19 GetFileNameAndSize, 20 GetQuota, 21/22 IsCloudEnabledFor{Account,App}
 *   ISteamUserStats011      0 RequestCurrentStats, 6 GetAchievement, 7 SetAchievement, 10 StoreStats
 *   ISteamApps007           4 GetCurrentGameLanguage
 *   ISteamUtils007          9 GetAppID
 *   ISteamClient017         21 SetWarningMessageHook (not forwarded: the hook is guest code)
 * Any other slot stops the run with its name.
 * Callbacks: the game registers CCallback objects for UserStatsReceived_t (1101) and UserStatsStored_t (1102).
 * Guest layout: vtable [0] Run(void *param, bool io_failure, SteamAPICall_t) (MSVC puts the overload declared second
 * first), [1] Run(void *) (_purecall here), [2] GetCallbackSizeBytes; +4 flags (u8), +8 callback id. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xrc_rt.h"
#ifdef _WIN32
#include <windows.h>
#endif

#define A(i) xrc_arg(c, (i))
#define S(a) ((char *)(xrc_mem + (u32)(a)))
#define DLL "steam_api.dll"

typedef unsigned char sbool;
typedef void *Obj;
/* host interface objects (old versions) and their vtables */
static Obj h_client, h_apps, h_stats, h_rs, h_utils;
static int h_pipe, h_user, steam_ok;
static u32 app_id = 345350; /* LIGHTNING RETURNS: FINAL FANTASY XIII on Steam */

#define VFN(obj, slot) ((*(void ***)(obj))[slot])

/* ------------------------------------------------------------------------------------------------ the DLL */
#ifdef _WIN32
static HMODULE dll;
static void *sym(const char *n) { return dll ? (void *)GetProcAddress(dll, n) : 0; }
static int load_dll(void) {
	char p[MAX_PATH], *s;
	if (dll) return 1;
	GetModuleFileNameA(0, p, sizeof p);
	s = strrchr(p, '\\');
	if (s) strcpy(s + 1, "steam_api64.dll"); else strcpy(p, "steam_api64.dll");
	dll = LoadLibraryA(p);
	if (!dll) fprintf(stderr, "xrc: steam: %s not found (copy Valve's steam_api64.dll next to the exe)\n", p);
	return dll != 0;
}
#else
static void *sym(const char *n) { (void)n; return 0; }
static int load_dll(void) { fprintf(stderr, "xrc: steam: no Steam on this platform\n"); return 0; }
#endif

typedef struct { int user, id; unsigned char *param; int size; } CallbackMsg; /* CallbackMsg_t (x64) */
static void (*md_run_frame)(int);
static sbool (*md_next)(int, CallbackMsg *);
static void (*md_free)(int);

static int steam_init(void) {
	char err[1024] = "";
	int r = -1;
	char env[32];
	if (!load_dll()) return 0;
	snprintf(env, sizeof env, "SteamAppId=%u", app_id); _putenv(env);
	snprintf(env, sizeof env, "SteamGameId=%u", app_id); _putenv(env);
	{
		int (*init_flat)(char *) = (int (*)(char *))sym("SteamAPI_InitFlat");
		sbool (*init_safe)(void) = (sbool (*)(void))sym("SteamAPI_InitSafe");
		sbool (*init_old)(void) = (sbool (*)(void))sym("SteamAPI_Init");
		if (init_flat) r = init_flat(err);
		else if (init_safe) r = init_safe() ? 0 : 1;
		else if (init_old) r = init_old() ? 0 : 1;
	}
	if (r != 0) {
		fprintf(stderr, "xrc: steam: SteamAPI init failed (%d)%s%s -- is Steam running and signed in to an account that owns the game?\n",
		        r, err[0] ? ": " : "", err);
		return 0;
	}
	{
		void (*md_init)(void) = (void (*)(void))sym("SteamAPI_ManualDispatch_Init");
		int (*get_pipe)(void) = (int (*)(void))sym("SteamAPI_GetHSteamPipe");
		int (*get_user)(void) = (int (*)(void))sym("SteamAPI_GetHSteamUser");
		Obj (*client)(void) = (Obj (*)(void))sym("SteamClient");
		Obj (*g_apps)(Obj, int, int, const char *) = (Obj (*)(Obj, int, int, const char *))sym("SteamAPI_ISteamClient_GetISteamApps");
		Obj (*g_stats)(Obj, int, int, const char *) = (Obj (*)(Obj, int, int, const char *))sym("SteamAPI_ISteamClient_GetISteamUserStats");
		Obj (*g_rs)(Obj, int, int, const char *) = (Obj (*)(Obj, int, int, const char *))sym("SteamAPI_ISteamClient_GetISteamRemoteStorage");
		Obj (*g_utils)(Obj, int, const char *) = (Obj (*)(Obj, int, const char *))sym("SteamAPI_ISteamClient_GetISteamUtils");
		md_run_frame = (void (*)(int))sym("SteamAPI_ManualDispatch_RunFrame");
		md_next = (sbool (*)(int, CallbackMsg *))sym("SteamAPI_ManualDispatch_GetNextCallback");
		md_free = (void (*)(int))sym("SteamAPI_ManualDispatch_FreeLastCallback");
		if (!md_init || !get_pipe || !get_user || !client || !g_apps || !g_stats || !g_rs || !g_utils || !md_run_frame || !md_next || !md_free) {
			fprintf(stderr, "xrc: steam: steam_api64.dll lacks the flat API (too old)\n");
			return 0;
		}
		md_init();
		h_pipe = get_pipe();
		h_user = get_user();
		h_client = client();
		h_apps = g_apps(h_client, h_user, h_pipe, "STEAMAPPS_INTERFACE_VERSION007");
		h_stats = g_stats(h_client, h_user, h_pipe, "STEAMUSERSTATS_INTERFACE_VERSION011");
		h_rs = g_rs(h_client, h_user, h_pipe, "STEAMREMOTESTORAGE_INTERFACE_VERSION013");
		h_utils = g_utils(h_client, h_pipe, "SteamUtils007");
	}
	fprintf(stderr, "xrc: steam: initialized for app %u (apps %s, user stats %s, remote storage %s, utils %s)\n", app_id,
	        h_apps ? "ok" : "MISSING", h_stats ? "ok" : "MISSING", h_rs ? "ok" : "MISSING", h_utils ? "ok" : "MISSING");
	return 1;
}

/* ------------------------------------------------------------------------------------------------ guest interfaces */
static void m_unimpl(XCtx *c) { xrc_fatal(c, "unimplemented Steam method %s", xrc_cur_import); }
static void need(XCtx *c, Obj o) { if (!o) xrc_fatal(c, "%s called without Steam (SteamAPI_Init failed?)", xrc_cur_import); }
#define THIS_RET(v, n) do { c->eax = (u32)(v); xrc_stdcall_ret(c, (n)); return; } while (0)

/* a guest string copy of a host one (a ring of buffers: GetFileNameAndSize, GetCurrentGameLanguage) */
static u32 gstr(const char *s) {
	static u32 ring[8];
	static unsigned k;
	u32 n = (u32)strlen(s ? s : "") + 1, *b = &ring[k++ & 7];
	if (*b) xrc_heap_free(*b);
	*b = xrc_heap_alloc(n, 0);
	memcpy(S(*b), s ? s : "", n);
	return *b;
}

/* Save profiles: with XRC_SAVE_DIR set (the launcher's profile folder), the remote-storage calls are served from
 * files in that folder instead of Steam Cloud, which is not touched. Unset: Steam Cloud, as the game shipped. */
#define LS_QUOTA (100u << 20)
#define LS_MAXF 256
static char ls_dir[MAX_PATH];
static int ls_on = -1;
static char ls_names[LS_MAXF][MAX_PATH];
static int ls_sizes[LS_MAXF], ls_n;
static int lsave(void) {
	if (ls_on < 0) {
		const char *d = getenv("XRC_SAVE_DIR");
		ls_on = d && *d && strlen(d) < sizeof ls_dir - 64;
		if (ls_on) {
			snprintf(ls_dir, sizeof ls_dir, "%s", d);
			CreateDirectoryA(ls_dir, NULL);
			fprintf(stderr, "xrc: saves go to the profile folder %s (not Steam Cloud)\n", ls_dir);
		}
	}
	return ls_on;
}
/* name -> path in the folder; 0 for a name that would leave it */
static int ls_path(const char *name, char *out) {
	const char *p;
	if (!name || !*name || strlen(name) > 200 || strstr(name, "..") || strchr(name, ':') || name[0] == '/' || name[0] == '\\') return 0;
	for (p = name; *p; p++) if ((unsigned char)*p < 32) return 0;
	snprintf(out, MAX_PATH, "%s\\%s", ls_dir, name);
	return 1;
}
static void ls_mkdirs(char *path) { /* the folders of a name with '/' in it */
	char *p;
	for (p = path + strlen(ls_dir) + 1; *p; p++)
		if (*p == '/' || *p == '\\') { char k = *p; *p = 0; CreateDirectoryA(path, NULL); *p = k; }
}
static long long ls_size(const char *path) {
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
	return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}
static void ls_scan(const char *sub) { /* every file under the folder, names relative with '/' */
	char pat[MAX_PATH], rel[MAX_PATH];
	WIN32_FIND_DATAA f;
	HANDLE h;
	snprintf(pat, sizeof pat, "%s%s%s\\*", ls_dir, *sub ? "\\" : "", sub);
	if ((h = FindFirstFileA(pat, &f)) == INVALID_HANDLE_VALUE) return;
	do {
		if (!strcmp(f.cFileName, ".") || !strcmp(f.cFileName, "..")) continue;
		snprintf(rel, sizeof rel, "%s%s%s", sub, *sub ? "/" : "", f.cFileName);
		if (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (strlen(rel) < 200) ls_scan(rel); }
		else if (ls_n < LS_MAXF && !strstr(f.cFileName, ".xrctmp")) {
			snprintf(ls_names[ls_n], MAX_PATH, "%s", rel);
			ls_sizes[ls_n++] = (int)f.nFileSizeLow;
		}
	} while (FindNextFileA(h, &f));
	FindClose(h);
}
static int ls_used(void) {
	int i;
	unsigned long long u = 0;
	ls_n = 0;
	ls_scan("");
	for (i = 0; i < ls_n; i++) u += (unsigned)ls_sizes[i];
	return u > LS_QUOTA ? (int)LS_QUOTA : (int)u;
}
/* written whole to a temporary file and then moved over the old one: a crash mid-write keeps the old save */
static sbool ls_write(const char *name, const void *data, int n) {
	char path[MAX_PATH], tmp[MAX_PATH + 16];
	FILE *f;
	size_t ok;
	if (n < 0 || !ls_path(name, path)) return 0;
	ls_mkdirs(path);
	snprintf(tmp, sizeof tmp, "%s.xrctmp", path);
	/* (the disk part -- an antivirus scan, the flush, a write-through move: tens of ms -- runs without the GIL, on a host
	 * copy of the data and host path strings made first: no guest memory is touched while it is let go) */
	{
		void *cp = 0;
		int rel = xrc_gil_held();
		if (rel && n > 0) { if (!(cp = malloc((size_t)n))) rel = 0; else memcpy(cp, data, (size_t)n); }
		if (rel) xrc_gil_release();
		f = fopen(tmp, "wb");
		if (f) {
			ok = n ? fwrite(cp ? cp : data, 1, (size_t)n, f) == (size_t)n : 1;
			if (fflush(f) || fclose(f)) ok = 0;
			if (ok) { /* (longrun: an antivirus / cloud-sync / indexer scan of the old save can hold it for a moment -- access denied or a sharing violation -- so the move is retried for about 3 s before giving up) */
				int tries;
				DWORD err = 0;
				for (tries = 0; tries < 10; tries++) {
					if (MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) break;
					err = GetLastError();
					Sleep(25u << (tries < 7 ? tries : 7));
				}
				if (tries == 10) { fprintf(stderr, "xrc: save: write of %s FAILED (move error %lu after %d tries)\n", name, (unsigned long)err, tries); ok = 0; }
				else if (tries) fprintf(stderr, "xrc: save: %s needed %d move retries (error %lu) before it was written\n", name, tries, (unsigned long)err);
			}
			if (!ok) DeleteFileA(tmp);
		} else ok = 0;
		if (rel) xrc_gil_acquire();
		free(cp);
		if (!ok) return 0;
	}
	fprintf(stderr, "xrc: save: wrote %s (%d bytes) to the profile folder\n", name, n);
	return 1;
}
static int ls_read(const char *name, void *data, int n) {
	char path[MAX_PATH];
	FILE *f;
	size_t got;
	if (n <= 0 || !ls_path(name, path)) return 0;
	if (xrc_gil_held()) { /* (read into a host buffer without the GIL, then copied into the guest's) */
		void *tmp = malloc((size_t)n);
		if (tmp) {
			xrc_gil_release();
			f = fopen(path, "rb");
			got = f ? fread(tmp, 1, (size_t)n, f) : 0;
			if (f) fclose(f);
			xrc_gil_acquire();
			if (!f) { free(tmp); return 0; }
			memcpy(data, tmp, got);
			free(tmp);
			fprintf(stderr, "xrc: save: read %s (%d bytes) from the profile folder\n", name, (int)got);
			return (int)got;
		}
	}
	if (!(f = fopen(path, "rb"))) return 0;
	got = fread(data, 1, (size_t)n, f);
	fclose(f);
	fprintf(stderr, "xrc: save: read %s (%d bytes) from the profile folder\n", name, (int)got);
	return (int)got;
}
static long long ls_time(const char *name) { /* unix seconds of the last write */
	char path[MAX_PATH];
	WIN32_FILE_ATTRIBUTE_DATA a;
	unsigned long long t;
	if (!ls_path(name, path) || !GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
	t = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
	return (long long)(t / 10000000ull - 11644473600ull);
}

static int raw_size(XCtx *c, const char *name) {
	if (lsave()) { char path[MAX_PATH]; long long n = ls_path(name, path) ? ls_size(path) : -1; return n < 0 ? 0 : (int)n; }
	need(c, h_rs);
	return ((int (*)(Obj, const char *))VFN(h_rs, 15))(h_rs, name);
}
static int raw_read(XCtx *c, const char *name, void *buf, int n) {
	if (lsave()) return ls_read(name, buf, n);
	need(c, h_rs);
	return ((int (*)(Obj, const char *, void *, int))VFN(h_rs, 1))(h_rs, name, buf, n);
}

/* The game's own settings file (SquareEnix/.../Configuration.ini: Graphics_Resolution = 3840x2160, ...): a line
 * "Key = value" is read as "Key = $XRC_CFG_Key" when that is set (lr.cfg, from the launcher), for Steam Cloud and
 * profiles alike; nothing is written. When the game saves its settings (its own options menu), what it writes is
 * what it is running with. 0: not that file, or nothing overridden. */
static int is_cfg(const char *name) { size_t n = strlen(name); return n >= 17 && !_stricmp(name + n - 17, "Configuration.ini"); }
static char *cfg_patched(XCtx *c, const char *name, int *len) {
	static int told;
	int n = raw_size(c, name), i, o = 0, changed = 0;
	char *in, *out;
	if (n <= 0 || n > (1 << 20) || !(in = malloc((size_t)n + 1))) return 0;
	n = raw_read(c, name, in, n);
	in[n < 0 ? 0 : n] = 0;
	if (!(out = malloc((size_t)n * 2 + 4096))) { free(in); return 0; }
	for (i = 0; i < n;) {
		int e = i, eq = -1, k0, k1, v0, v1;
		char key[96], env[112];
		const char *v;
		while (e < n && in[e] != '\n') { if (in[e] == '=' && eq < 0) eq = e; e++; }
		v1 = e; /* the value ends before \r\n */
		if (v1 > i && in[v1 - 1] == '\r') v1--;
		if (eq > i) {
			for (k0 = i; k0 < eq && (in[k0] == ' ' || in[k0] == '\t'); k0++) ;
			for (k1 = eq; k1 > k0 && (in[k1 - 1] == ' ' || in[k1 - 1] == '\t'); k1--) ;
			for (v0 = eq + 1; v0 < v1 && (in[v0] == ' ' || in[v0] == '\t'); v0++) ;
			if (k1 > k0 && k1 - k0 < (int)sizeof key - 1) {
				memcpy(key, in + k0, (size_t)(k1 - k0));
				key[k1 - k0] = 0;
				snprintf(env, sizeof env, "XRC_CFG_%s", key);
#if defined(XRC_LR) && defined(_WIN32)
				{ void xrc_menu_cfg_seen(const char *key, const char *v, int n); xrc_menu_cfg_seen(key, in + v0, v1 - v0); } /* (the F10 menu shows the file's own value) */
#endif
				if ((v = getenv(env)) && *v && strlen(v) < 64 && !strpbrk(v, "\r\n")) {
					size_t vl = strlen(v);
					if (told++ < 32) fprintf(stderr, "xrc: settings: %s = %s (was %.*s)\n", key, v, v1 - v0, in + v0);
					memcpy(out + o, in + i, (size_t)(v0 - i)); o += v0 - i;
					memcpy(out + o, v, vl); o += (int)vl;
					i = v1; /* the line ending is copied below */
					changed = 1;
				}
			}
		}
		if (e < n) e++;
		memcpy(out + o, in + i, (size_t)(e - i)); o += e - i;
		i = e;
	}
	free(in);
	if (!changed) { free(out); return 0; }
	*len = o;
	return out;
}

/* ISteamRemoteStorage013 */
static void rs_FileWrite(XCtx *c) {
	if (lsave()) THIS_RET(ls_write(S(A(0)), xrc_mem + A(1), (int)A(2)), 3);
	need(c, h_rs); THIS_RET(((sbool (*)(Obj, const char *, const void *, int))VFN(h_rs, 0))(h_rs, S(A(0)), xrc_mem + A(1), (int)A(2)), 3);
}
static void rs_FileRead(XCtx *c) {
	u32 name = A(0), buf = A(1);
	int n = (int)A(2), len;
	char *p;
	if (is_cfg(S(name)) && (p = cfg_patched(c, S(name), &len))) {
		if (n > len) n = len;
		if (n > 0) memcpy(xrc_mem + buf, p, (size_t)n);
		free(p);
		THIS_RET(n < 0 ? 0 : n, 3);
	}
	THIS_RET(raw_read(c, S(name), xrc_mem + buf, n), 3);
}
static void rs_FileDelete(XCtx *c) {
	if (lsave()) { char path[MAX_PATH]; THIS_RET(ls_path(S(A(0)), path) && DeleteFileA(path), 1); }
	need(c, h_rs); THIS_RET(((sbool (*)(Obj, const char *))VFN(h_rs, 6))(h_rs, S(A(0))), 1);
}
static void rs_GetFileSize(XCtx *c) {
	int len;
	char *p;
	if (is_cfg(S(A(0))) && (p = cfg_patched(c, S(A(0)), &len))) { free(p); THIS_RET(len, 1); }
	THIS_RET(raw_size(c, S(A(0))), 1);
}
static void rs_GetFileTimestamp(XCtx *c) {
	long long t;
	if (lsave()) t = ls_time(S(A(0)));
	else { need(c, h_rs); t = ((long long (*)(Obj, const char *))VFN(h_rs, 16))(h_rs, S(A(0))); }
	c->edx = (u32)((unsigned long long)t >> 32);
	THIS_RET((u32)t, 1);
}
static void rs_GetFileCount(XCtx *c) {
	if (lsave()) { ls_used(); THIS_RET(ls_n, 0); }
	need(c, h_rs); THIS_RET(((int (*)(Obj))VFN(h_rs, 18))(h_rs), 0);
}
static void rs_GetFileNameAndSize(XCtx *c) {
	int size = 0, i = (int)A(0);
	const char *n;
	if (lsave()) {
		if (i < 0 || i >= ls_n) ls_used(); /* (GetFileCount normally comes first and fills the list) */
		n = i >= 0 && i < ls_n ? ls_names[i] : "";
		size = i >= 0 && i < ls_n ? ls_sizes[i] : 0;
	} else {
		need(c, h_rs);
		n = ((const char *(*)(Obj, int, int *))VFN(h_rs, 19))(h_rs, i, &size);
	}
	if (A(1)) M32(A(1)) = (u32)size;
	THIS_RET(gstr(n), 2);
}
/* GetQuota: int32 * in version 013 (uint64 * later); 8-byte buffers take either, the low half is the value */
static void rs_GetQuota(XCtx *c) {
	unsigned long long total = 0, avail = 0;
	sbool r;
	if (lsave()) { total = LS_QUOTA; avail = LS_QUOTA - (unsigned)ls_used(); r = 1; }
	else { need(c, h_rs); r = ((sbool (*)(Obj, void *, void *))VFN(h_rs, 20))(h_rs, &total, &avail); }
	if (A(0)) M32(A(0)) = (u32)total;
	if (A(1)) M32(A(1)) = (u32)avail;
	THIS_RET(r, 2);
}
/* (a profile folder answers "enabled", so the game saves the same way it does to the cloud) */
static void rs_IsCloudEnabledForAccount(XCtx *c) { if (lsave()) THIS_RET(1, 0); need(c, h_rs); THIS_RET(((sbool (*)(Obj))VFN(h_rs, 21))(h_rs), 0); }
static void rs_IsCloudEnabledForApp(XCtx *c) { if (lsave()) THIS_RET(1, 0); need(c, h_rs); THIS_RET(((sbool (*)(Obj))VFN(h_rs, 22))(h_rs), 0); }

/* ISteamUserStats011 */
static void us_RequestCurrentStats(XCtx *c) { need(c, h_stats); THIS_RET(((sbool (*)(Obj))VFN(h_stats, 0))(h_stats), 0); }
static void us_GetAchievement(XCtx *c) {
	sbool got = 0, r;
	need(c, h_stats);
	r = ((sbool (*)(Obj, const char *, sbool *))VFN(h_stats, 6))(h_stats, S(A(0)), &got);
	if (A(1)) M8(A(1)) = got;
	THIS_RET(r, 2);
}
static void us_SetAchievement(XCtx *c) {
	need(c, h_stats);
	fprintf(stderr, "xrc: steam: achievement %s\n", S(A(0)));
	THIS_RET(((sbool (*)(Obj, const char *))VFN(h_stats, 7))(h_stats, S(A(0))), 1);
}
static void us_StoreStats(XCtx *c) { need(c, h_stats); THIS_RET(((sbool (*)(Obj))VFN(h_stats, 10))(h_stats), 0); }

/* ISteamApps007, ISteamUtils007, ISteamClient017 */
static void ap_GetCurrentGameLanguage(XCtx *c) {
	static u32 g;
	need(c, h_apps);
	if (!g) g = gstr(((const char *(*)(Obj))VFN(h_apps, 4))(h_apps));
	THIS_RET(g, 0);
}
static void ut_GetAppID(XCtx *c) { need(c, h_utils); THIS_RET(((u32 (*)(Obj))VFN(h_utils, 9))(h_utils), 0); }
static void cl_SetWarningMessageHook(XCtx *c) { THIS_RET(0, 1); }

typedef struct { int slot; XrcHle fn; } Slot;
#define NSLOTS 48
static u32 make_iface(const char *iname, const Slot *impl, int nimpl) {
	u32 vt = xrc_heap_alloc(4 * NSLOTS, 1), obj = xrc_heap_alloc(16, 1);
	char nm[64];
	int k, j;
	for (k = 0; k < NSLOTS; k++) {
		XrcHle fn = m_unimpl;
		for (j = 0; j < nimpl; j++) if (impl[j].slot == k) fn = impl[j].fn;
		snprintf(nm, sizeof nm, "%s::m%d", iname, k);
		M32(vt + 4 * k) = xrc_import_with(DLL, nm, fn);
	}
	M32(obj) = vt;
	return obj;
}
static const Slot rs_slots[] = {{0, rs_FileWrite}, {1, rs_FileRead}, {6, rs_FileDelete}, {15, rs_GetFileSize},
	{16, rs_GetFileTimestamp}, {18, rs_GetFileCount}, {19, rs_GetFileNameAndSize}, {20, rs_GetQuota},
	{21, rs_IsCloudEnabledForAccount}, {22, rs_IsCloudEnabledForApp}};
static const Slot us_slots[] = {{0, us_RequestCurrentStats}, {6, us_GetAchievement}, {7, us_SetAchievement}, {10, us_StoreStats}};
static const Slot ap_slots[] = {{4, ap_GetCurrentGameLanguage}};
static const Slot ut_slots[] = {{9, ut_GetAppID}};
static const Slot cl_slots[] = {{21, cl_SetWarningMessageHook}};
#define IFACE(fn, iname, slots) \
	static void fn(XCtx *c) { \
		static u32 o; \
		if (!o) o = make_iface(iname, slots, (int)(sizeof slots / sizeof slots[0])); \
		c->eax = o; \
		xrc_cdecl_ret(c); \
	}
IFACE(hle_SteamRemoteStorage, "ISteamRemoteStorage013", rs_slots)
IFACE(hle_SteamUserStats, "ISteamUserStats011", us_slots)
IFACE(hle_SteamApps, "ISteamApps007", ap_slots)
IFACE(hle_SteamUtils, "ISteamUtils007", ut_slots)
IFACE(hle_SteamClient, "ISteamClient017", cl_slots)

/* ------------------------------------------------------------------------------------------------ API + callbacks */
static u32 cbs[64];
static int ncbs;

static void hle_SteamAPI_RestartAppIfNecessary(XCtx *c) {
	/* Not forwarded: the real one relaunches the original exe through Steam unless Steam started this process.
	 * SteamAPI_Init below still asks Steam whether this account owns the game. */
	if (A(0)) app_id = A(0);
	c->eax = 0;
	xrc_cdecl_ret(c);
}
static void hle_SteamAPI_Init(XCtx *c) {
	if (!steam_ok) steam_ok = steam_init();
	c->eax = (u32)steam_ok;
	xrc_cdecl_ret(c);
}
static void hle_SteamAPI_Shutdown(XCtx *c) {
	void (*f)(void) = (void (*)(void))sym("SteamAPI_Shutdown");
	if (steam_ok && f) f();
	steam_ok = 0;
	xrc_cdecl_ret(c);
}
static void hle_SteamAPI_RegisterCallback(XCtx *c) {
	u32 obj = A(0);
	M8(obj + 4) |= 1; /* k_ECallbackFlagsRegistered */
	M32(obj + 8) = A(1);
	if (ncbs < 64) cbs[ncbs++] = obj;
	xrc_cdecl_ret(c);
}
static void hle_SteamAPI_UnregisterCallback(XCtx *c) {
	u32 obj = A(0);
	int k;
	for (k = 0; k < ncbs; k++) if (cbs[k] == obj) { cbs[k] = cbs[--ncbs]; break; }
	M8(obj + 4) &= (u8)~1u;
	xrc_cdecl_ret(c);
}
static void hle_SteamAPI_RunCallbacks(XCtx *c) {
	CallbackMsg m;
	if (steam_ok) {
		md_run_frame(h_pipe);
		while (md_next(h_pipe, &m)) {
			int k;
			if (m.id != 703) /* SteamAPICallCompleted_t: call results, which LR does not use */
				for (k = 0; k < ncbs; k++) {
					u32 obj = cbs[k], vt = M32(obj), size, buf, a[4];
					if ((int)M32(obj + 8) != m.id) continue;
					c->ecx = obj;
					size = xrc_call_guest(c, M32(vt + 8), 0, 0); /* GetCallbackSizeBytes */
					buf = xrc_heap_alloc(size > (u32)m.size ? size : (u32)m.size, 1);
					memcpy(xrc_mem + buf, m.param, (size_t)m.size);
					a[0] = buf; a[1] = 0; a[2] = 0; a[3] = 0;
					c->ecx = obj;
					xrc_call_guest(c, M32(vt), 4, a); /* Run(param, false, 0) */
					xrc_heap_free(buf);
				}
			md_free(h_pipe);
		}
	}
	xrc_cdecl_ret(c);
}

#define E(n) {DLL, #n, hle_##n}
const XrcImport xrc_hle_steam[] = {
	E(SteamAPI_RestartAppIfNecessary), E(SteamAPI_Init), E(SteamAPI_Shutdown), E(SteamAPI_RunCallbacks),
	E(SteamAPI_RegisterCallback), E(SteamAPI_UnregisterCallback),
	E(SteamClient), E(SteamUtils), E(SteamApps), E(SteamRemoteStorage), E(SteamUserStats),
};
const unsigned xrc_hle_steam_n = sizeof xrc_hle_steam / sizeof xrc_hle_steam[0];
