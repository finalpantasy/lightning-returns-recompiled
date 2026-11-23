// --selftest <tmpdir>: the lr.cfg editor, the profile operations and the movie counts, on copies in a temporary folder
// (nothing of the real lr.cfg, Steam's folder or the real profiles is touched). Prints PASS / FAIL per check.
package launcher

import "core:fmt"
import "core:os"
import "core:strings"
import win "core:sys/windows"

@(private = "file")
fails: int
@(private = "file")
checks: int
@(private = "file")
report: strings.Builder

@(private = "file")
check :: proc(name: string, ok: bool, detail := "") {
	checks += 1
	if !ok {fails += 1}
	line := fmt.tprintf("%s  %s%s%s", ok ? "PASS" : "FAIL", name, detail != "" ? "  -- " : "", detail)
	fmt.println(line)
	strings.write_string(&report, line)
	strings.write_byte(&report, '\n')
}

@(private = "file")
wr :: proc(path, text: string) {_ = os.write_entire_file(path, transmute([]u8)text)}

@(private = "file")
rd :: proc(path: string) -> string {
	d, err := os.read_entire_file(path, context.temp_allocator)
	return err == nil ? string(d) : "<unreadable>"
}

run_selftest :: proc(tmp: string) -> int {
	if tmp == "" {
		fmt.println("usage: LRLauncher.exe --selftest <tmpdir>")
		return 2
	}
	report = strings.builder_make()
	_ = os.make_directory_all(tmp)

	// ------------------------------------------------------------------ lr.cfg
	cfg := pjoin({tmp, "lr.cfg"})
	start := "# lr.cfg comment\nFOO=bar\n\nXRC_WIDTH=1280\nXRC_CFG_Graphics_Glare=Standard\nUNKNOWN_KEY=keep me\nXRC_WIDTH=999\n# trailing comment\n"
	wr(cfg, start)
	ok := cfg_write(cfg, {{"XRC_WIDTH", "1920"}, {"XRC_HEIGHT", "1080"}})
	want := "# lr.cfg comment\nFOO=bar\n\nXRC_WIDTH=1920\nXRC_CFG_Graphics_Glare=Standard\nUNKNOWN_KEY=keep me\n# trailing comment\nXRC_HEIGHT=1080\n"
	check("cfg: replace in place, drop duplicate, add new key at the end", ok && rd(cfg) == want, rd(cfg))
	lines := cfg_read(cfg)
	v, found := cfg_value(lines[:], "XRC_HEIGHT")
	check("cfg: read back a key", found && v == "1080")
	_, found = cfg_value(lines[:], "# lr.cfg comment")
	check("cfg: a comment line is not a key", !found)

	ok = cfg_write(cfg, {{"XRC_CFG_Graphics_Glare", ""}})
	want = "# lr.cfg comment\nFOO=bar\n\nXRC_WIDTH=1920\nUNKNOWN_KEY=keep me\n# trailing comment\nXRC_HEIGHT=1080\n"
	check("cfg: remove a key, keep every other line", ok && rd(cfg) == want, rd(cfg))

	ok = cfg_write(cfg, {{"NOT_THERE", ""}})
	check("cfg: removing a missing key changes nothing", ok && rd(cfg) == want)

	// the game rewrites a line between two launcher writes: the launcher reads the file fresh
	wr(cfg, strings.concatenate({rd(cfg), "XRC_CFG_Graphics_Lighting=Advanced\n"}, context.temp_allocator))
	ok = cfg_write(cfg, {{"XRC_NO_AUDIO", "1"}})
	check("cfg: a change made by the game in between is kept", ok && strings.contains(rd(cfg), "XRC_CFG_Graphics_Lighting=Advanced\n") && strings.contains(rd(cfg), "XRC_NO_AUDIO=1\n"))

	tmpf := strings.concatenate({cfg, ".tmp"}, context.temp_allocator)
	check("cfg: atomic write leaves no temporary file", !os.exists(tmpf))

	// a file with no final newline and Windows line ends
	wr(cfg, "A=1\r\nXRC_WIDTH=800\r\nB=2")
	ok = cfg_write(cfg, {{"XRC_WIDTH", "1024"}})
	check("cfg: no final newline / CRLF lines are kept", ok && rd(cfg) == "A=1\r\nXRC_WIDTH=1024\nB=2\n", rd(cfg))
	lines = cfg_read(cfg)
	v, found = cfg_value(lines[:], "A")
	check("cfg: a CRLF value is read without the CR", found && v == "1")

	// a key that is a prefix of another is not touched
	wr(cfg, "XRC_WIDTH_EXTRA=7\nXRC_WIDTH=5\n")
	ok = cfg_write(cfg, {{"XRC_WIDTH", ""}})
	check("cfg: a key does not match a longer key", ok && rd(cfg) == "XRC_WIDTH_EXTRA=7\n", rd(cfg))

	// a file that does not exist yet
	fresh := pjoin({tmp, "new.cfg"})
	ok = cfg_write(fresh, {{"XRC_SAVE_DIR", `C:\x\y`}})
	check("cfg: creates the file when it is missing", ok && rd(fresh) == "XRC_SAVE_DIR=C:\\x\\y\n", rd(fresh))

	// ------------------------------------------------------------------ profiles
	root := pjoin({tmp, "profiles"})
	steam := pjoin({tmp, "steam", "userdata", "1234", APP_ID, "remote"})
	_ = os.make_directory_all(pjoin({steam, "SquareEnix"}))
	wr(pjoin({steam, "lrff13-00.dat"}), "save zero")
	wr(pjoin({steam, "lrff13-01.dat"}), "save one!")
	wr(pjoin({steam, "SquareEnix", "x.bin"}), "nested")
	wr(pjoin({steam, "other.txt"}), "not a save")
	before0, before1 := rd(pjoin({steam, "lrff13-00.dat"})), file_time(pjoin({steam, "lrff13-00.dat"}))

	n, _, ex := scan_saves(steam)
	check("profiles: counts lrff13-NN.dat files only", ex && n == 2, fmt.tprintf("%d", n))
	check("profiles: is_save_name", is_save_name("lrff13-07.dat") && !is_save_name("lrff13-.dat") && !is_save_name("lrff13-0a.dat") && !is_save_name("x.dat"))

	check("profiles: name '' is refused", validate_profile_name(root, "") != "")
	check("profiles: name with a slash is refused", validate_profile_name(root, "a/b") != "")
	check("profiles: 'Steam Cloud' is reserved", validate_profile_name(root, "steam cloud") != "")
	check("profiles: device name is refused", validate_profile_name(root, "CON") != "")
	check("profiles: trailing dot is refused", validate_profile_name(root, "abc.") != "")

	p, err := profile_create(root, "Run A")
	check("profiles: New creates an empty folder", err == "" && dir_exists(p) && p == pjoin({root, "Run A"}), err)
	_, err = profile_create(root, "run a")
	check("profiles: a duplicate name is refused", err != "")

	p, err = profile_copy_from(root, "Copy B", steam)
	check("profiles: Copy from Steam Cloud", err == "" && dir_exists(p), err)
	n, _, _ = scan_saves(p)
	check("profiles: the copy has both saves", n == 2)
	check("profiles: the copy has the subfolder file", rd(pjoin({p, "SquareEnix", "x.bin"})) == "nested")
	check("profiles: the copy has the other file", rd(pjoin({p, "other.txt"})) == "not a save")
	n, _, _ = scan_saves(steam)
	check("profiles: the Steam folder is unchanged", n == 2 && rd(pjoin({steam, "lrff13-00.dat"})) == before0 && file_time(pjoin({steam, "lrff13-00.dat"})) == before1 && file_exists(pjoin({steam, "SquareEnix", "x.bin"})) && file_exists(pjoin({steam, "other.txt"})))
	_, err = profile_copy_from(root, "Copy B", steam)
	check("profiles: copy onto an existing name is refused", err != "")

	list := profile_list(root, "")
	names_ok := false
	for pr in list {if pr.name == "Copy B" && pr.saves == 2 && pr.modified != 0 {names_ok = true}}
	check("profiles: the list shows name, save count and time", names_ok && list[0].cloud && len(list) >= 3, fmt.tprintf("%d entries", len(list)))
	check("profiles: a time formats", len(format_file_time(file_time(steam))) == 16, format_file_time(file_time(steam)))

	p, err = profile_rename(root, "Copy B", "Renamed C")
	check("profiles: Rename", err == "" && dir_exists(p) && !dir_exists(pjoin({root, "Copy B"})) && file_exists(pjoin({p, "lrff13-01.dat"})), err)
	_, err = profile_rename(root, "Renamed C", "Run A")
	check("profiles: rename onto an existing name is refused", err != "")

	// the guards: nothing outside the profiles root is written or removed
	outside := pjoin({tmp, "outside"})
	_ = os.make_directory_all(outside)
	check("guard: a path outside the root is not inside it", !inside_root(root, outside) && !inside_root(root, root) && !inside_root(root, pjoin({root, "..", "outside"})) && inside_root(root, pjoin({root, "x"})))
	check("guard: copy_tree outside the root is refused", !copy_tree(root, steam, pjoin({tmp, "outside", "copy"})) && !dir_exists(pjoin({tmp, "outside", "copy"})))
	check("guard: delete of '' / '..' / the root is refused", profile_delete(root, "") != "" && profile_delete(root, "..") != "" && profile_delete(root, "..\\outside") != "" && dir_exists(outside) && dir_exists(root))
	check("guard: the Steam folder is not deletable as a profile", profile_delete(root, "..\\steam\\userdata") != "" && dir_exists(steam))

	err = profile_delete(root, "Renamed C")
	check("profiles: Delete moves the folder to the Recycle Bin", err == "" && !dir_exists(pjoin({root, "Renamed C"})), err)
	err = profile_delete(root, "Run A")
	check("profiles: Delete an empty profile", err == "" && !dir_exists(pjoin({root, "Run A"})), err)
	check("profiles: the Steam folder still has its files", file_exists(pjoin({steam, "lrff13-00.dat"})) && file_exists(pjoin({steam, "lrff13-01.dat"})))

	// choosing a profile in lr.cfg and going back to Steam Cloud
	sel := pjoin({tmp, "sel.cfg"})
	wr(sel, "KEEP=1\n")
	cfg_write(sel, {{"XRC_SAVE_DIR", pjoin({root, "Anything"})}})
	lines = cfg_read(sel)
	v, found = cfg_value(lines[:], "XRC_SAVE_DIR")
	check("select: a profile writes XRC_SAVE_DIR (absolute path)", found && v == pjoin({root, "Anything"}))
	cfg_write(sel, {{"XRC_SAVE_DIR", ""}})
	check("select: Steam Cloud removes the line", rd(sel) == "KEEP=1\n", rd(sel))

	// ------------------------------------------------------------------ movies
	mv := pjoin({tmp, "xrc"})
	_ = os.make_directory_all(mv)
	for f in ([]string{"a.mp4", "b.mp4", "a.hd.mp4", "c.mp4.raw.mp4", "d.tmp.mp4", "e.hd.tmp.mp4", "x.pcm", "f.mp4.raw.mp4.track0.tmp.pcm"}) {wr(pjoin({mv, f}), "x")}
	base, hd := count_movies(mv)
	check("media: counts base and .hd.mp4 movies, skips .raw. and .tmp.", base == 2 && hd == 1, fmt.tprintf("base %d hd %d", base, hd))
	wr(pjoin({mv, "upscale_status.txt"}), "upscaling 3/10\nsecond line")
	line, age := read_status(mv)
	check("media: status file: first line and a recent age", line == "upscaling 3/10" && age >= -5 && age < 120, fmt.tprintf("%q %.1f", line, age))
	check("media: a missing status file", func_status_missing(pjoin({tmp, "nope"})))

	// ------------------------------------------------------------------ small things
	check("util: command line quoting", win_cmdline({`C:\a b\lr.exe`, `D:\g\LRFF13.exe`, "x"}) == `"C:\a b\lr.exe" D:\g\LRFF13.exe x`, win_cmdline({`C:\a b\lr.exe`, `D:\g\LRFF13.exe`, "x"}))
	check("util: itoa", itoa(0) == "0" && itoa(1920) == "1920")
	mw, mh := monitor_size()
	check("util: monitor size", mw >= 640 && mh >= 360, fmt.tprintf("%dx%d", mw, mh))

	fmt.println(fails == 0 ? fmt.tprintf("SELFTEST PASS (%d checks)", checks) : fmt.tprintf("SELFTEST FAIL (%d of %d checks failed)", fails, checks))
	strings.write_string(&report, fails == 0 ? fmt.tprintf("SELFTEST PASS (%d checks)\n", checks) : fmt.tprintf("SELFTEST FAIL (%d of %d)\n", fails, checks))
	_ = os.write_entire_file(pjoin({tmp, "selftest.txt"}), transmute([]u8)strings.to_string(report))
	return fails == 0 ? 0 : 1
}

@(private = "file")
func_status_missing :: proc(dir: string) -> bool {
	l, a := read_status(dir)
	return l == "" && a < 0
}
