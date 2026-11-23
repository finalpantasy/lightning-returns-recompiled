/* vblank.c - the display's refresh, for the frame limiter (ovr_game.c, XRC_FRAME_LIMIT builds).
 *
 * The limiter and the display each kept their own 60 Hz clock. When the limiter ran a hair faster than the refresh,
 * the swap chain's queue filled (FIFO) and every frame reached the screen up to two refreshes late: input lag at a
 * steady 60 fps, in and out as the two clocks drifted. Here a thread waits for each vertical blank of the monitor the
 * game's window is on (DXGI: IDXGIOutput::WaitForVBlank, windowed and fullscreen alike) and counts them; the limiter
 * releases each frame at a refresh, so there is one clock and the queue cannot creep up.
 *
 * Windows only (Linux drivers mostly have VK_KHR_present_wait: vk_d3d9.c). XRC_VSYNC_LOCK=0: off (the limiter's own
 * clock, as before). */
#include <stdio.h>
#include <stdlib.h>

int xrc_vblank_start(void);
unsigned long long xrc_vblank_count(double *hz);

#ifdef _WIN32
#define COBJMACROS
#include <windows.h>
#include <dxgi.h>
#include "xrc_rt.h"

static volatile LONG64 vb_n;      /* vertical blanks counted */
static volatile LONG64 vb_hz_x1k; /* the refresh rate measured, in mHz (0: not yet) */
static IDXGIOutput *output;
static HMONITOR out_mon;

/* the DXGI output of the monitor the game's window is on (else the first one) */
static IDXGIOutput *find_output(HMONITOR want, HMONITOR *got) {
	static const GUID iid_factory1 = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};
	static HRESULT(WINAPI * create)(REFIID, void **);
	IDXGIFactory1 *f = 0;
	IDXGIAdapter1 *a;
	IDXGIOutput *o, *first = 0, *pick = 0;
	HMONITOR first_mon = 0;
	UINT i, j;
	if (!create) {
		HMODULE m = LoadLibraryA("dxgi.dll");
		if (m) *(FARPROC *)&create = GetProcAddress(m, "CreateDXGIFactory1");
		if (!create) return 0;
	}
	if (FAILED(create(&iid_factory1, (void **)&f))) return 0;
	for (i = 0; !pick && IDXGIFactory1_EnumAdapters1(f, i, &a) == S_OK; i++) {
		for (j = 0; !pick && IDXGIAdapter1_EnumOutputs(a, j, &o) == S_OK; j++) {
			DXGI_OUTPUT_DESC d;
			if (IDXGIOutput_GetDesc(o, &d) == S_OK && d.AttachedToDesktop) {
				if (d.Monitor == want) { pick = o; *got = d.Monitor; continue; }
				if (!first) { first = o; first_mon = d.Monitor; continue; }
			}
			IDXGIOutput_Release(o);
		}
		IDXGIAdapter1_Release(a);
	}
	IDXGIFactory1_Release(f);
	if (pick) { if (first) IDXGIOutput_Release(first); return pick; }
	*got = first_mon;
	return first;
}

static DWORD WINAPI vblank_thread(void *arg) {
	LARGE_INTEGER fq, t, t0;
	long long n0 = 0;
	DWORD last_check = 0;
	int fails = 0;
	(void)arg;
	QueryPerformanceFrequency(&fq);
	QueryPerformanceCounter(&t0);
	for (;;) {
		DWORD now = GetTickCount();
		if (!output || now - last_check > 1000) { /* (the window may move to another monitor) */
			HMONITOR m = MonitorFromWindow((HWND)xrc_view_window, MONITOR_DEFAULTTOPRIMARY);
			last_check = now;
			if (!output || m != out_mon) {
				HMONITOR got = 0;
				IDXGIOutput *o = find_output(m, &got);
				if (o) {
					if (output) IDXGIOutput_Release(output);
					output = o; out_mon = got;
					InterlockedExchange64(&vb_hz_x1k, 0); /* (measured again) */
					n0 = vb_n; QueryPerformanceCounter(&t0);
				}
			}
			if (!output) { Sleep(100); continue; }
		}
		if (IDXGIOutput_WaitForVBlank(output) != S_OK) {
			/* (no vertical blank to wait for -- the monitor off, a remote session): not locked */
			if (++fails > 3) InterlockedExchange64(&vb_hz_x1k, 0);
			Sleep(16);
			continue;
		}
		fails = 0;
		QueryPerformanceCounter(&t);
		/* (WaitForVBlank returns at once when called within the blanking interval: the same vertical blank again.
		 * Counted twice, the rate came out 60.2-60.5 Hz on a 60 Hz display -- the limiter a little faster than it, the
		 * queue filling again. Under 2.5 ms after the last one, shorter than a refresh even at 240 Hz: the same one) */
		{
			static LONGLONG last;
			if (last && (t.QuadPart - last) * 400 < fq.QuadPart) continue;
			last = t.QuadPart;
		}
		InterlockedIncrement64(&vb_n);
		/* the rate: over each 2 s or so (a vertical blank missed now and then counts it a little low: kept steady by
		 * averaging, and what it is for -- a multiple of the limit or not -- has a margin) */
		if (t.QuadPart - t0.QuadPart >= 2 * fq.QuadPart) {
			double hz = (double)(vb_n - n0) * (double)fq.QuadPart / (double)(t.QuadPart - t0.QuadPart);
			InterlockedExchange64(&vb_hz_x1k, (LONG64)(hz * 1000.0));
			n0 = vb_n; t0 = t;
		}
	}
	return 0;
}

int xrc_vblank_start(void) {
	static int started = -1;
	if (started < 0) {
		const char *e = getenv("XRC_VSYNC_LOCK");
		started = 0;
		if (!(e && !atoi(e)) && CreateThread(0, 0, vblank_thread, 0, 0, 0)) started = 1;
	}
	return started;
}
/* the vertical blanks so far, and the refresh rate (0: not known: the limiter keeps its own clock) */
unsigned long long xrc_vblank_count(double *hz) {
	*hz = (double)InterlockedCompareExchange64(&vb_hz_x1k, 0, 0) / 1000.0;
	return (unsigned long long)InterlockedCompareExchange64(&vb_n, 0, 0);
}
#else
int xrc_vblank_start(void) { return 0; }
unsigned long long xrc_vblank_count(double *hz) { *hz = 0; return 0; }
#endif
