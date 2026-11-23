// Save profiles. "Steam Cloud" (no XRC_SAVE_DIR line in lr.cfg) is the default; every other profile is a folder under
// %LOCALAPPDATA%\lr-recomp\profiles\<name>, and choosing one writes XRC_SAVE_DIR=<its absolute path>.
//
// Steam's own folder (userdata\<id>\345350\remote) is read-only for the launcher: it is only listed and copied FROM.
// Every write, rename and delete here goes through inside_root(): a path outside the profiles root is refused.
package launcher

import "core:fmt"
import "core:os"
import "core:slice"
import "core:strings"
import win "core:sys/windows"

STEAM_CLOUD :: "Steam Cloud"
APP_ID :: "345350"

Profile :: struct {
	name:     string,
	path:     string,
	cloud:    bool,
	saves:    int, // files named lrff13-NN.dat
	modified: u64, // newest write time of its files (a FILETIME; 0: none)
	exists:   bool,
}

profiles_root :: proc() -> string {
	return pjoin({os.get_env("LOCALAPPDATA", context.temp_allocator), "lr-recomp", "profiles"})
}

// ---------------------------------------------------------------------------------------------- paths
norm_path :: proc(p: string) -> string {
	s, _ := strings.replace_all(p, "/", "\\", context.temp_allocator)
	s = strings.trim_right(s, "\\")
	return strings.to_lower(s, context.temp_allocator)
}

// p is strictly below root
inside_root :: proc(root, p: string) -> bool {
	r, q := norm_path(root), norm_path(p)
	if r == "" || len(q) <= len(r) + 1 {return false}
	return strings.has_prefix(q, strings.concatenate({r, "\\"}, context.temp_allocator)) && !strings.contains(q, "\\..")
}

// ---------------------------------------------------------------------------------------------- file times
file_time :: proc(path: string) -> u64 {
	a: win.WIN32_FILE_ATTRIBUTE_DATA
	if !win.GetFileAttributesExW(win.utf8_to_wstring(path, context.temp_allocator), win.GetFileExInfoStandard, &a) {return 0}
	return u64(a.ftLastWriteTime.dwHighDateTime) << 32 | u64(a.ftLastWriteTime.dwLowDateTime)
}

format_file_time :: proc(t: u64) -> string {
	if t == 0 {return "-"}
	ft := win.FILETIME{dwLowDateTime = win.DWORD(t & 0xffffffff), dwHighDateTime = win.DWORD(t >> 32)}
	lt: win.FILETIME
	st: win.SYSTEMTIME
	if !FileTimeToLocalFileTime(&ft, &lt) || !win.FileTimeToSystemTime(&lt, &st) {return "-"}
	return fmt.tprintf("%04d-%02d-%02d %02d:%02d", st.year, st.month, st.day, st.hour, st.minute)
}

// lrff13-NN.dat
is_save_name :: proc(name: string) -> bool {
	l := strings.to_lower(name, context.temp_allocator)
	if !strings.has_prefix(l, "lrff13-") || !strings.has_suffix(l, ".dat") {return false}
	mid := l[len("lrff13-"):len(l) - len(".dat")]
	if len(mid) == 0 {return false}
	for c in mid {if c < '0' || c > '9' {return false}}
	return true
}

// the save files of a folder and the newest write time among them (the folder's own when it has none)
scan_saves :: proc(dir: string) -> (saves: int, newest: u64, exists: bool) {
	if !os.is_dir(dir) {return 0, 0, false}
	list, err := os.read_all_directory_by_path(dir, context.temp_allocator)
	if err == nil {
		for e in list {
			if e.type != .Directory && is_save_name(e.name) {
				saves += 1
				newest = max(newest, file_time(e.fullpath))
			}
		}
	}
	if newest == 0 {newest = file_time(dir)}
	return saves, newest, true
}

// ---------------------------------------------------------------------------------------------- Steam Cloud (read only)
// Steam's folder: the registry, the install's own place (steamapps\common\<game> is two levels below steamapps), the usual places
steam_roots :: proc(game_dir: string) -> [dynamic]string {
	out := make([dynamic]string, context.temp_allocator)
	if r := registry_steam_path(); r != "" {append(&out, r)}
	if game_dir != "" {
		n, _ := strings.replace_all(game_dir, "/", "\\", context.temp_allocator)
		if i := strings.index(strings.to_lower(n, context.temp_allocator), "\\steamapps\\"); i > 0 {append(&out, n[:i])}
	}
	append(&out, `C:\Program Files (x86)\Steam`, `C:\Steam`)
	return out
}

// userdata\*\345350\remote: the one with the newest save when several accounts have one
steam_cloud_dir :: proc(game_dir: string) -> string {
	best, best_t := "", u64(0)
	for root in steam_roots(game_dir) {
		ud := pjoin({root, "userdata"})
		list, err := os.read_all_directory_by_path(ud, context.temp_allocator)
		if err != nil {continue}
		for e in list {
			if e.type != .Directory {continue}
			remote := pjoin({e.fullpath, APP_ID, "remote"})
			if !os.is_dir(remote) {continue}
			_, t, _ := scan_saves(remote)
			if best == "" || t > best_t {best, best_t = remote, t}
		}
		if best != "" {break}
	}
	return best
}

// ---------------------------------------------------------------------------------------------- listing
profile_names :: proc(root: string) -> []string {
	names := make([dynamic]string, context.temp_allocator)
	list, err := os.read_all_directory_by_path(root, context.temp_allocator)
	if err == nil {
		for e in list {if e.type == .Directory {append(&names, e.name)}}
	}
	slice.sort_by(names[:], proc(a, b: string) -> bool {return strings.to_lower(a, context.temp_allocator) < strings.to_lower(b, context.temp_allocator)})
	return names[:]
}

// Steam Cloud first, then the folders of root
profile_list :: proc(root, game_dir: string) -> [dynamic]Profile {
	out := make([dynamic]Profile, context.temp_allocator)
	cloud := steam_cloud_dir(game_dir)
	p := Profile{name = STEAM_CLOUD, path = cloud, cloud = true}
	if cloud != "" {p.saves, p.modified, p.exists = scan_saves(cloud)}
	append(&out, p)
	for n in profile_names(root) {
		q := Profile{name = n, path = pjoin({root, n})}
		q.saves, q.modified, q.exists = scan_saves(q.path)
		append(&out, q)
	}
	return out
}

// ---------------------------------------------------------------------------------------------- names
// "" when name is fine for a new profile in root
validate_profile_name :: proc(root, name: string, ignore := "") -> string {
	n := strings.trim_space(name)
	if n == "" {return "Type a name."}
	if len(n) > 60 {return "That name is too long."}
	if strings.equal_fold(n, STEAM_CLOUD) {return "That name is taken by the default profile."}
	for c in n {
		if c < 32 || strings.contains_rune(`<>:"/\|?*`, c) {return `A name cannot contain < > : " / \ | ? *`}
	}
	if strings.has_suffix(n, ".") || strings.has_suffix(n, " ") {return "A name cannot end with a dot or a space."}
	base := strings.to_upper(strings.split(n, ".", context.temp_allocator)[0], context.temp_allocator)
	for r in ([]string{"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3"}) {
		if base == r {return "Windows does not allow that name."}
	}
	if !strings.equal_fold(n, ignore) && os.exists(pjoin({root, n})) {return "A profile with that name exists."}
	return ""
}

// ---------------------------------------------------------------------------------------------- operations
// A new, empty profile (a new game). Returns its path, or "" and why.
profile_create :: proc(root, name: string) -> (path, err: string) {
	n := strings.trim_space(name)
	if e := validate_profile_name(root, n); e != "" {return "", e}
	p := pjoin({root, n})
	if !inside_root(root, p) {return "", "Bad profile path."}
	if e := os.make_directory_all(p); e != nil {return "", "Could not create the folder."}
	return p, ""
}

// every file and subfolder of src copied into dst (dst is inside the profiles root; src is only read)
copy_tree :: proc(root, src, dst: string) -> bool {
	if !inside_root(root, dst) {return false}
	if err := os.make_directory_all(dst); err != nil {return false}
	list, err := os.read_all_directory_by_path(src, context.temp_allocator)
	if err != nil {return false}
	ok := true
	for e in list {
		to := pjoin({dst, e.name})
		if e.type == .Directory {
			if !copy_tree(root, e.fullpath, to) {ok = false}
		} else {
			if !win.CopyFileW(win.utf8_to_wstring(e.fullpath, context.temp_allocator), win.utf8_to_wstring(to, context.temp_allocator), true) {ok = false}
		}
	}
	return ok
}

// A new profile holding a copy of everything in src (Steam Cloud's remote folder).
profile_copy_from :: proc(root, name, src: string) -> (path, err: string) {
	n := strings.trim_space(name)
	if src == "" || !os.is_dir(src) {return "", "The Steam Cloud saves were not found."}
	if e := validate_profile_name(root, n); e != "" {return "", e}
	p := pjoin({root, n})
	if !copy_tree(root, src, p) {return p, "Some files could not be copied."}
	return p, ""
}

profile_rename :: proc(root, old, new_name: string) -> (path, err: string) {
	n := strings.trim_space(new_name)
	if e := validate_profile_name(root, n, old); e != "" {return "", e}
	from, to := pjoin({root, old}), pjoin({root, n})
	if !inside_root(root, from) || !inside_root(root, to) || !os.is_dir(from) {return "", "Bad profile path."}
	if strings.equal_fold(old, n) && old == n {return from, ""}
	if !win.MoveFileExW(win.utf8_to_wstring(from, context.temp_allocator), win.utf8_to_wstring(to, context.temp_allocator), 0) {return "", "Could not rename the folder (is the game using it?)."}
	return to, ""
}

// moved to the Recycle Bin, never deleted for good
profile_delete :: proc(root, name: string) -> string {
	p := pjoin({root, name})
	if !inside_root(root, p) || !os.is_dir(p) {return "Bad profile path."}
	w := win.utf8_to_utf16(p, context.temp_allocator)
	buf := make([]u16, len(w) + 2, context.temp_allocator) // (a double-NUL-terminated list)
	copy(buf, w)
	op := win.SHFILEOPSTRUCTW {
		wFunc  = win.FO_DELETE,
		pFrom  = win.wstring(raw_data(buf)),
		fFlags = win.FOF_ALLOWUNDO | win.FOF_NOCONFIRMATION | win.FOF_NOERRORUI | win.FOF_SILENT,
	}
	if win.SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted {return "Windows could not move it to the Recycle Bin."}
	if os.exists(p) {return "The folder is still there."}
	return ""
}

open_folder :: proc(path: string) {
	win.ShellExecuteW(nil, win.L("open"), win.utf8_to_wstring(path, context.temp_allocator), nil, nil, win.SW_SHOWNORMAL)
}
