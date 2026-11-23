// lr.cfg, the file lr.exe reads at startup (NAME=value lines become environment variables; '#' lines are comments).
// The game's F10 menu rewrites lines of the same file, so the launcher edits it the same way: line by line, keeping
// every line it does not own (unknown keys, comments, blank lines), and writes it whole to a temporary file that is
// moved over the original (atomic). A key is always read from disk fresh before a write: the game may have changed it.
package launcher

import "core:os"
import "core:strings"
import win "core:sys/windows"

Cfg_Edit :: struct {
	key, val: string, // val "" removes the key
}

// the lines of the text (the final newline does not make an empty last line)
cfg_split :: proc(text: string, allocator := context.temp_allocator) -> [dynamic]string {
	lines := make([dynamic]string, allocator)
	if text == "" {return lines}
	parts := strings.split(text, "\n", allocator)
	n := len(parts)
	if n > 0 && parts[n - 1] == "" {n -= 1}
	for i in 0 ..< n {append(&lines, parts[i])}
	return lines
}

cfg_join :: proc(lines: []string, allocator := context.temp_allocator) -> string {
	b := strings.builder_make(allocator)
	for l in lines {
		strings.write_string(&b, l)
		strings.write_byte(&b, '\n')
	}
	return strings.to_string(b)
}

@(private = "file")
line_is_key :: proc(line, key: string) -> bool {
	return len(line) > len(key) && line[len(key)] == '=' && line[:len(key)] == key && line[0] != '#'
}

// the value of key (last '\r' trimmed) in the text; found false when it has no line
cfg_value :: proc(lines: []string, key: string) -> (val: string, found: bool) {
	for l in lines {
		if line_is_key(l, key) {return strings.trim_right(l[len(key) + 1:], "\r\n"), true}
	}
	return "", false
}

// key=val replaced in place (the first line of the key; later duplicates are dropped), or added at the end; val "" removes it
cfg_apply :: proc(lines: []string, edits: []Cfg_Edit, allocator := context.temp_allocator) -> [dynamic]string {
	out := make([dynamic]string, allocator)
	for l in lines {append(&out, l)}
	for e in edits {
		next := make([dynamic]string, allocator)
		done := false
		for l in out {
			if line_is_key(l, e.key) {
				if !done && e.val != "" {append(&next, strings.concatenate({e.key, "=", e.val}, allocator))}
				done = true
				continue
			}
			append(&next, l)
		}
		if !done && e.val != "" {append(&next, strings.concatenate({e.key, "=", e.val}, allocator))}
		out = next
	}
	return out
}

cfg_read :: proc(path: string, allocator := context.temp_allocator) -> [dynamic]string {
	d, err := os.read_entire_file(path, context.temp_allocator)
	if err != nil {return make([dynamic]string, allocator)}
	return cfg_split(string(d), allocator)
}

// text to path through path.tmp, then moved over it
write_file_atomic :: proc(path, text: string) -> bool {
	tmp := strings.concatenate({path, ".tmp"}, context.temp_allocator)
	if err := os.write_entire_file(tmp, transmute([]u8)text); err != nil {return false}
	wtmp := win.utf8_to_wstring(tmp, context.temp_allocator)
	wdst := win.utf8_to_wstring(path, context.temp_allocator)
	if !win.MoveFileExW(wtmp, wdst, win.MOVEFILE_REPLACE_EXISTING | win.MOVEFILE_WRITE_THROUGH) {
		win.DeleteFileW(wtmp)
		return false
	}
	return true
}

// read the file now, apply the edits, write it atomically
cfg_write :: proc(path: string, edits: []Cfg_Edit) -> bool {
	lines := cfg_read(path)
	out := cfg_apply(lines[:], edits)
	return write_file_atomic(path, cfg_join(out[:]))
}
