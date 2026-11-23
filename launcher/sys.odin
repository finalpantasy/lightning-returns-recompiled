// Windows plumbing: where the launcher lives, Steam and the game's folder, starting and finding programs.
package launcher

import "core:os"
import "core:path/filepath"
import "core:strings"
import "core:sync"
import "core:thread"
import win "core:sys/windows"

foreign import kernel32 "system:Kernel32.lib"
foreign import user32 "system:User32.lib"
foreign import ntdll "system:ntdll.lib"

@(default_calling_convention = "system")
foreign kernel32 {
	FileTimeToLocalFileTime :: proc(ft: ^win.FILETIME, out: ^win.FILETIME) -> win.BOOL ---
	OpenProcess :: proc(access: win.DWORD, inherit: win.BOOL, pid: win.DWORD) -> win.HANDLE ---
	SetPriorityClass :: proc(h: win.HANDLE, cls: win.DWORD) -> win.BOOL ---
	GetProcessId :: proc(h: win.HANDLE) -> win.DWORD ---
}
@(default_calling_convention = "system")
foreign user32 {
	PrintWindow :: proc(hwnd: win.HWND, hdc: win.HDC, flags: win.UINT) -> win.BOOL ---
	GetDpiForSystem :: proc() -> win.UINT ---
}
@(default_calling_convention = "system")
foreign ntdll {
	NtQueryInformationProcess :: proc(h: win.HANDLE, class: i32, info: rawptr, len: u32, ret: ^u32) -> i32 ---
}

// ---------------------------------------------------------------------------------------------- the launcher's own place
// the folder of LRLauncher.exe (lr.exe, and lr.cfg, are beside it)
exe_dir :: proc() -> string {
	buf: [1024]u16
	n := win.GetModuleFileNameW(nil, &buf[0], len(buf))
	if n == 0 {return "."}
	s, _ := win.utf16_to_utf8(buf[:n], context.allocator)
	d, _ := filepath.split(s)
	return strings.trim_right(d, "\\/")
}

data_dir :: proc() -> string {return pjoin({os.get_env("LOCALAPPDATA", context.temp_allocator), "lr-recomp"})}

pjoin :: proc(parts: []string, allocator := context.temp_allocator) -> string {
	s, _ := filepath.join(parts, allocator)
	return s
}
file_exists :: proc(p: string) -> bool {return os.exists(p) && !os.is_dir(p)}
dir_exists :: proc(p: string) -> bool {return os.is_dir(p)}

// Steam's folder, from the registry (HKCU\Software\Valve\Steam SteamPath)
registry_steam_path :: proc() -> string {
	buf: [1024]u16
	size := win.DWORD(size_of(buf))
	if win.RegGetValueW(win.HKEY_CURRENT_USER, win.L(`Software\Valve\Steam`), win.L("SteamPath"), win.RRF_RT_REG_SZ, nil, &buf, &size) != 0 {return ""}
	s, _ := win.wstring_to_utf8(win.wstring(&buf[0]), -1, context.temp_allocator)
	r, _ := strings.replace_all(s, "/", "\\", context.temp_allocator)
	return r
}

// the primary monitor's current mode
monitor_size :: proc() -> (w, h: int) {
	dm: win.DEVMODEW
	dm.dmSize = size_of(win.DEVMODEW)
	if win.EnumDisplaySettingsW(nil, win.ENUM_CURRENT_SETTINGS, &dm) {return int(dm.dmPelsWidth), int(dm.dmPelsHeight)}
	return 1920, 1080
}

// ---------------------------------------------------------------------------------------------- the game
GAME_EXE :: "LRFF13.exe"
GAME_FOLDER :: "LIGHTNING RETURNS FINAL FANTASY XIII"

is_game_dir :: proc(d: string) -> bool {return d != "" && file_exists(pjoin({d, GAME_EXE}))}

// Steam libraries from libraryfolders.vdf ("path" "D:\SteamLibrary")
steam_libraries :: proc(steam: string) -> [dynamic]string {
	out := make([dynamic]string, context.temp_allocator)
	append(&out, steam)
	d, err := os.read_entire_file(pjoin({steam, "steamapps", "libraryfolders.vdf"}), context.temp_allocator)
	if err != nil {return out}
	for line in strings.split_lines(string(d), context.temp_allocator) {
		t := strings.trim_space(line)
		if !strings.has_prefix(t, `"path"`) {continue}
		rest := strings.trim_space(t[len(`"path"`):])
		rest = strings.trim(rest, `"`)
		rest, _ = strings.replace_all(rest, `\\`, `\`, context.temp_allocator)
		if rest != "" {append(&out, rest)}
	}
	return out
}

// the game's folder: the libraries of the Steam in the registry and of the usual places
find_game :: proc() -> string {
	roots := make([dynamic]string, context.temp_allocator)
	if r := registry_steam_path(); r != "" {append(&roots, r)}
	append(&roots, `C:\Program Files (x86)\Steam`, `C:\Steam`)
	for r in roots {
		for lib in steam_libraries(r) {
			g := pjoin({lib, "steamapps", "common", GAME_FOLDER})
			if is_game_dir(g) {
				out := strings.clone(g)
				if len(out) > 1 && out[1] == ':' && out[0] >= 'a' && out[0] <= 'z' {out = strings.concatenate({strings.to_upper(out[:1], context.temp_allocator), out[1:]})}
				return out
			}
		}
	}
	return ""
}

// ---------------------------------------------------------------------------------------------- starting programs
// The command line of cmd, quoted the way CommandLineToArgvW / the C runtime read it.
win_cmdline :: proc(cmd: []string, allocator := context.temp_allocator) -> string {
	b := strings.builder_make(allocator)
	for a, i in cmd {
		if i > 0 {strings.write_byte(&b, ' ')}
		if len(a) > 0 && !strings.contains_any(a, " \t\"") {
			strings.write_string(&b, a)
			continue
		}
		strings.write_byte(&b, '"')
		slashes := 0
		for c in transmute([]u8)a {
			switch c {
			case '\\':
				slashes += 1
			case '"':
				for _ in 0 ..< slashes * 2 + 1 {strings.write_byte(&b, '\\')}
				slashes = 0
				strings.write_byte(&b, '"')
			case:
				for _ in 0 ..< slashes {strings.write_byte(&b, '\\')}
				slashes = 0
				strings.write_byte(&b, c)
			}
		}
		for _ in 0 ..< slashes * 2 {strings.write_byte(&b, '\\')}
		strings.write_byte(&b, '"')
	}
	return strings.to_string(b)
}

// Starts a program. hidden: no window (CREATE_NO_WINDOW), its output into log_path; else a normal window.
// priority: extra creation flags (BELOW_NORMAL_PRIORITY_CLASS). Returns the process handle and id.
spawn :: proc(cmd: []string, dir: string, hidden: bool, priority: win.DWORD = 0, log_path := "") -> (h: win.HANDLE, pid: u32, ok: bool) {
	si := win.STARTUPINFOW{cb = size_of(win.STARTUPINFOW)}
	sa := win.SECURITY_ATTRIBUTES{nLength = size_of(win.SECURITY_ATTRIBUTES), bInheritHandle = true}
	nul, logf: win.HANDLE = win.INVALID_HANDLE_VALUE, win.INVALID_HANDLE_VALUE
	flags := priority
	if hidden {
		flags |= win.CREATE_NO_WINDOW
		nul = win.CreateFileW(win.L("NUL"), win.GENERIC_READ, win.FILE_SHARE_READ | win.FILE_SHARE_WRITE, &sa, win.OPEN_EXISTING, 0, nil)
		out := nul
		if log_path != "" {
			logf = win.CreateFileW(win.utf8_to_wstring(log_path, context.temp_allocator), win.GENERIC_WRITE, win.FILE_SHARE_READ, &sa, win.CREATE_ALWAYS, win.FILE_ATTRIBUTE_NORMAL, nil)
			if logf != win.INVALID_HANDLE_VALUE {out = logf}
		}
		si.dwFlags = win.STARTF_USESTDHANDLES
		si.hStdInput, si.hStdOutput, si.hStdError = nul, out, out
	}
	pi: win.PROCESS_INFORMATION
	line := win.utf8_to_wstring(win_cmdline(cmd), context.temp_allocator)
	wdir := win.utf8_to_wstring(dir, context.temp_allocator) if dir != "" else nil
	made := win.CreateProcessW(nil, line, nil, nil, win.BOOL(hidden), flags, nil, wdir, &si, &pi)
	if nul != win.INVALID_HANDLE_VALUE {win.CloseHandle(nul)}
	if logf != win.INVALID_HANDLE_VALUE {win.CloseHandle(logf)}
	if !made {return}
	win.CloseHandle(pi.hThread)
	return pi.hProcess, u32(pi.dwProcessId), true
}

// Runs a command and waits (hidden); the exit code, -1 if it could not start.
run_hidden :: proc(cmd: []string) -> int {
	h, _, ok := spawn(cmd, "", true)
	if !ok {return -1}
	win.WaitForSingleObject(h, 20000)
	code: win.DWORD
	win.GetExitCodeProcess(h, &code)
	win.CloseHandle(h)
	return int(code)
}

// the process with this id is still running
pid_alive :: proc(pid: u32) -> bool {
	if pid == 0 {return false}
	h := OpenProcess(0x1000, false, win.DWORD(pid)) // PROCESS_QUERY_LIMITED_INFORMATION
	if h == nil {return false}
	defer win.CloseHandle(h)
	code: win.DWORD
	if !win.GetExitCodeProcess(h, &code) {return false}
	return code == 259 // STILL_ACTIVE
}

// a process and the processes it started, ended by taskkill (by id only: never by name)
kill_tree :: proc(pid: u32) {
	if pid == 0 {return}
	run_hidden({pjoin({os.get_env("SystemRoot", context.temp_allocator), "System32", "taskkill.exe"}), "/PID", itoa(int(pid)), "/T", "/F"})
}

itoa :: proc(n: int) -> string {
	b: [24]u8
	i := len(b)
	v := n
	neg := v < 0
	if neg {v = -v}
	for {
		i -= 1
		b[i] = u8('0' + v % 10)
		v /= 10
		if v == 0 {break}
	}
	if neg {i -= 1; b[i] = '-'}
	return strings.clone(string(b[i:]), context.temp_allocator)
}

// ---------------------------------------------------------------------------------------------- the game running
game_running: bool // lr.exe started by this launcher is running
game_pid: u32

// lr.exe "<game dir>\LRFF13.exe", in the game's folder, in a normal window
start_game :: proc(lr_exe, game_dir: string) -> bool {
	h, pid, ok := spawn({lr_exe, pjoin({game_dir, GAME_EXE})}, game_dir, false)
	if !ok {return false}
	game_pid = pid
	sync.atomic_store(&game_running, true)
	thread.create_and_start_with_poly_data(h, proc(h: win.HANDLE) {
		win.WaitForSingleObject(h, win.INFINITE)
		win.CloseHandle(h)
		sync.atomic_store(&game_running, false)
	}, self_cleanup = true)
	return true
}

// ---------------------------------------------------------------------------------------------- finding other programs
// The command line of a process (read from its PEB); "" if it cannot be read.
process_cmdline :: proc(pid: u32) -> string {
	h := OpenProcess(win.PROCESS_QUERY_INFORMATION | win.PROCESS_VM_READ, false, win.DWORD(pid))
	if h == nil {return ""}
	defer win.CloseHandle(h)
	Pbi :: struct {
		exit, peb, affinity, prio, pid, ppid: uintptr,
	}
	pbi: Pbi
	ret: u32
	if NtQueryInformationProcess(h, 0, &pbi, size_of(Pbi), &ret) != 0 || pbi.peb == 0 {return ""}
	params: uintptr
	n: uint
	if !win.ReadProcessMemory(h, rawptr(pbi.peb + 0x20), &params, size_of(uintptr), &n) || params == 0 {return ""}
	US :: struct {
		len, max: u16,
		buf:      uintptr,
	}
	us: US
	if !win.ReadProcessMemory(h, rawptr(params + 0x70), &us, size_of(US), &n) || us.len == 0 || us.buf == 0 {return ""}
	chars := make([]u16, int(us.len) / 2, context.temp_allocator)
	if !win.ReadProcessMemory(h, rawptr(us.buf), raw_data(chars), uint(us.len), &n) {return ""}
	s, _ := win.utf16_to_utf8(chars, context.temp_allocator)
	return s
}

// Processes that are a media job: lr.exe --convert-movies, python upscale_movies.py / upscale_textures.py, powershell prepare_movies.ps1.
// Returns how many and the first one's id.
find_job_processes :: proc() -> (count: int, first: u32) {
	snap := win.CreateToolhelp32Snapshot(win.TH32CS_SNAPPROCESS, 0)
	if snap == win.INVALID_HANDLE_VALUE {return}
	defer win.CloseHandle(snap)
	e := win.PROCESSENTRY32W{dwSize = size_of(win.PROCESSENTRY32W)}
	more := win.Process32FirstW(snap, &e)
	for more {
		name, _ := win.utf16_to_utf8(e.szExeFile[:], context.temp_allocator)
		name = strings.to_lower(strings.trim_right_null(name), context.temp_allocator)
		want := ""
		switch name {
		case "lr.exe":
			want = "--convert-movies"
		case "python.exe", "pythonw.exe":
			want = "upscale_" // (upscale_movies.py, upscale_textures.py: one GPU job at a time)
		case "powershell.exe", "pwsh.exe":
			want = "prepare_movies.ps1"
		}
		if want != "" {
			cl := strings.to_lower(process_cmdline(u32(e.th32ProcessID)), context.temp_allocator)
			if strings.contains(cl, want) {
				count += 1
				if first == 0 {first = u32(e.th32ProcessID)}
			}
		}
		more = win.Process32NextW(snap, &e)
	}
	return
}

// a web page (or a folder) in the shell
open_url :: proc(url: string) {
	win.ShellExecuteW(nil, win.L("open"), win.utf8_to_wstring(url, context.temp_allocator), nil, nil, win.SW_SHOWNORMAL)
}
