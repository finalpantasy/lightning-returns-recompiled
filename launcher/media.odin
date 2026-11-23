// Media tab: the HD cinematics job (tools\upscale\prepare_movies.ps1: converts the movies to MP4, then upscales them with
// Real-ESRGAN, for about a day). A job may already run that this launcher did not start: it is found by its status file
// (written within the last 10 minutes) or by its processes, shown, and a second one is never started. Stop only ends a job
// this launcher started, by its process id (the process tree).
package launcher

import "core:os"
import "core:path/filepath"
import "core:strings"
import "core:sync"
import "core:thread"
import "core:time"
import win "core:sys/windows"

Media_State :: struct {
	status:      string, // the status file's line
	status_age:  f64, // seconds since it was written (-1: no file)
	base, hd:    int, // movies with an MP4, and with an .hd.mp4
	procs:       int, // job processes found
	external:    bool, // a job runs that this launcher did not start
	started:     bool, // this launcher started a job that still runs
	started_pid: u32,
	script_ok:   bool,
	script:      string,
	xrc_dir:     string,
	// HD textures: collected originals (the runtime's XRC_TEX_DUMP) and the upscaled ones (hd_textures)
	tex_collected, tex_hd: int,
	tex_ok:      bool, // the upscaler and its Python were found
}

// HD textures, beside lr.exe: what the game collects while playing, and what it uses
tex_collect_dir :: proc() -> string {return pjoin({exe_dir(), "tex_collected"})}
tex_hd_dir :: proc() -> string {return pjoin({exe_dir(), "hd_textures"})}
tex_script :: proc() -> string {s, _ := filepath.clean(pjoin({exe_dir(), "..", "..", "tools", "upscale", "upscale_textures.py"}), context.temp_allocator); return s}
tex_python :: proc() -> string {s, _ := filepath.clean(pjoin({exe_dir(), "..", "..", "tools", "upscale", ".venv", "Scripts", "python.exe"}), context.temp_allocator); return s}

count_dds :: proc(dir: string, with_skip := false) -> int { // (with_skip: the upscaler's .skip markers count too: data textures kept as they are)
	list, err := os.read_all_directory_by_path(dir, context.temp_allocator)
	if err != nil {return 0}
	n := 0
	for e in list {if e.type != .Directory && (strings.has_suffix(strings.to_lower(e.name, context.temp_allocator), ".dds") || (with_skip && strings.has_suffix(strings.to_lower(e.name, context.temp_allocator), ".skip"))) {n += 1}}
	return n
}

media_mutex: sync.Mutex
media: Media_State
media_game: string // the game folder the poller watches
media_stop: bool
media_th: ^thread.Thread
job_pid: u32 // the job this launcher started
job_handle: win.HANDLE

STATUS_FRESH_SECONDS :: 600.0

xrc_movie_dir :: proc(game: string) -> string {return pjoin({game, "weiss_data", "movie", "xrc"})}

// where prepare_movies.ps1 is, from lr.exe's folder: ..\..\tools\upscale\
script_path :: proc() -> string {
	s, _ := filepath.clean(pjoin({exe_dir(), "..", "..", "tools", "upscale", "prepare_movies.ps1"}), context.temp_allocator)
	return s
}

// the counts for the xrc folder: base *.mp4 and *.hd.mp4 (names with .raw. or .tmp. are in-progress files)
count_movies :: proc(dir: string) -> (base, hd: int) {
	list, err := os.read_all_directory_by_path(dir, context.temp_allocator)
	if err != nil {return}
	for e in list {
		if e.type == .Directory {continue}
		n := strings.to_lower(e.name, context.temp_allocator)
		if !strings.has_suffix(n, ".mp4") || strings.contains(n, ".raw.") || strings.contains(n, ".tmp.") {continue}
		if strings.contains(n, ".hd.") {
			if strings.has_suffix(n, ".hd.mp4") {hd += 1}
		} else {
			base += 1
		}
	}
	return
}

read_status :: proc(dir: string) -> (line: string, age: f64) {
	p := pjoin({dir, "upscale_status.txt"})
	ft := file_time(p)
	if ft == 0 {return "", -1}
	now: win.FILETIME
	win.GetSystemTimeAsFileTime(&now)
	age = f64(u64(now.dwHighDateTime) << 32 | u64(now.dwLowDateTime)) / 1e7 - f64(ft) / 1e7
	d, err := os.read_entire_file(p, context.temp_allocator)
	if err != nil {return "", age}
	first := string(d)
	if i := strings.index_any(first, "\r\n"); i >= 0 {first = first[:i]}
	return strings.clone(strings.trim_space(first)), age
}

media_poll_once :: proc(game: string, scan_procs: bool) {
	dir := xrc_movie_dir(game)
	st, age := read_status(dir)
	base, hd := count_movies(dir)
	sync.mutex_lock(&media_mutex)
	delete(media.status)
	media.status = st
	media.status_age = age
	media.base, media.hd = base, hd
	media.xrc_dir = strings.clone(dir)
	media.script = strings.clone(script_path())
	media.script_ok = file_exists(media.script)
	media.tex_collected = count_dds(tex_collect_dir())
	media.tex_hd = count_dds(tex_hd_dir(), true)
	media.tex_ok = file_exists(tex_script()) && file_exists(tex_python())
	prev_procs := media.procs
	sync.mutex_unlock(&media_mutex)

	procs := prev_procs
	if scan_procs {procs, _ = find_job_processes()}
	own := job_handle != nil && win.WaitForSingleObject(job_handle, 0) == win.WAIT_TIMEOUT
	if !own && job_handle != nil {
		win.CloseHandle(job_handle)
		job_handle = nil
		job_pid = 0
	}
	fresh := age >= 0 && age < STATUS_FRESH_SECONDS
	sync.mutex_lock(&media_mutex)
	media.procs = procs
	media.started = own
	media.started_pid = job_pid
	media.external = !own && (fresh || procs > 0)
	sync.mutex_unlock(&media_mutex)
}

get_media :: proc() -> Media_State {
	sync.mutex_lock(&media_mutex)
	defer sync.mutex_unlock(&media_mutex)
	return media
}

// every 2 s: the status line and the counts; every 6 s the process list
media_start :: proc(game: string) {
	sync.atomic_store(&media_stop, false)
	media_game = strings.clone(game)
	media_th = thread.create(proc(th: ^thread.Thread) {
		tick := 0
		for !sync.atomic_load(&media_stop) {
			media_poll_once(media_game, tick % 3 == 0)
			tick += 1
			for _ in 0 ..< 20 {
				if sync.atomic_load(&media_stop) {break}
				time.sleep(100 * time.Millisecond)
			}
		}
	})
	thread.start(media_th)
}

media_shutdown :: proc() {
	sync.atomic_store(&media_stop, true)
	if media_th != nil {
		thread.join(media_th)
		thread.destroy(media_th)
		media_th = nil
	}
}

// Starts the job (hidden, BelowNormal) unless one runs. Returns "" or why not.
prepare_start :: proc(game, lr_exe: string) -> string {
	m := get_media()
	if m.external || m.started || m.procs > 0 {return "A job is already running."}
	if !is_game_dir(game) {return "Choose the game folder first."}
	if !m.script_ok {return "prepare_movies.ps1 was not found beside lr.exe (..\\..\\tools\\upscale)."}
	if !file_exists(lr_exe) {return "lr.exe was not found."}
	ps := pjoin({os.get_env("SystemRoot", context.temp_allocator), "System32", "WindowsPowerShell", "v1.0", "powershell.exe"})
	_ = os.make_directory_all(data_dir())
	h, pid, ok := spawn({ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", m.script, "-Game", game}, "", true, win.BELOW_NORMAL_PRIORITY_CLASS, pjoin({data_dir(), "prepare_movies.log"}))
	if !ok {return "Could not start PowerShell."}
	job_pid = pid
	job_handle = h
	// (a poll shortly after shows it)
	sync.mutex_lock(&media_mutex)
	media.started = true
	media.started_pid = pid
	sync.mutex_unlock(&media_mutex)
	return ""
}

// Starts the texture upscaler (hidden, BelowNormal) on the collected textures unless a job runs. Returns "" or why not.
tex_start :: proc() -> string {
	m := get_media()
	if m.external || m.started || m.procs > 0 {return "A job is already running: one at a time."}
	if !m.tex_ok {return "upscale_textures.py or its Python was not found (..\\..\\tools\\upscale beside lr.exe)."}
	if m.tex_collected == 0 {return "No textures collected yet: turn on Collect textures (Graphics tab) and play."}
	_ = os.make_directory_all(data_dir())
	h, pid, ok := spawn({tex_python(), tex_script(), "--dump", tex_collect_dir(), "--out", tex_hd_dir()}, "", true, win.BELOW_NORMAL_PRIORITY_CLASS, pjoin({data_dir(), "upscale_textures.log"}))
	if !ok {return "Could not start the upscaler."}
	job_pid = pid
	job_handle = h
	sync.mutex_lock(&media_mutex)
	media.started = true
	media.started_pid = pid
	sync.mutex_unlock(&media_mutex)
	return ""
}

// ends the job this launcher started (never one it did not start)
prepare_stop :: proc() {
	if job_pid == 0 || job_handle == nil || win.WaitForSingleObject(job_handle, 0) != win.WAIT_TIMEOUT {return}
	kill_tree(job_pid)
	if job_handle != nil {win.CloseHandle(job_handle); job_handle = nil}
	job_pid = 0
}
