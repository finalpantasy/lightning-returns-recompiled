// Lightning Returns Recomp Launcher: starts lr.exe (the static recompilation of LIGHTNING RETURNS: FINAL FANTASY XIII) and
// edits its lr.cfg. Tabs: Play, Graphics, Saves (profiles), Media (HD cinematics).
//
// Based on ps4star's WhiteRecomp launcher (Odin; its window, widgets and iv_* UI libraries). This one does not build the
// game: lr.exe is prebuilt, beside this program.
//
// Test and screenshot modes (no game is started):
//   LRLauncher.exe --selftest <tmpdir>                     the lr.cfg editor, the profiles, the movie counts: PASS / FAIL
//   LRLauncher.exe --shot <play|graphics|saves|media> <out.png> [--cfg <file>] [--profiles <dir>]
//                                                          shows the window for about 3 seconds, saves a picture of the tab
package launcher

import "core:fmt"
import "core:os"
import "core:strconv"
import "core:strings"
import "core:sync"
import "core:time"
import input "shared:iv_input"
import iv "shared:iv_system"
import "shared:iv_system/platform"
import "shared:iv_system/ren"
import ren_assets "shared:iv_system/ren/assets"
import ui "shared:iv_ui"
import win "core:sys/windows"

Signal :: enum {
	None,
}

WINDOW_TITLE :: "Lightning Returns Recomp Launcher"

Tab :: enum {
	Play,
	Graphics,
	Saves,
	Media,
}

// ---------------------------------------------------------------------------------------------- state
tab: Tab
game_dir: string
lr_exe: string
cfg_file: string
cfg_cache: []string
profiles_dir: string
profiles_cache: []Profile
note_msg: string // a one-line message at the bottom of the window
note_until: time.Tick
res_custom: bool // Output resolution: Custom is picked
cust_w, cust_h: string
monitor_w, monitor_h: int
frame_wheel: int

// ---------------------------------------------------------------------------------------------- launcher settings (the game folder)
settings_path :: proc() -> string {return pjoin({data_dir(), "launcher.txt"})}

load_game_dir :: proc() {
	d, err := os.read_entire_file(settings_path(), context.temp_allocator)
	if err == nil {
		for line in strings.split_lines(string(d), context.temp_allocator) {
			if strings.has_prefix(line, "game_dir=") {game_dir = strings.clone(strings.trim_space(line[len("game_dir="):]))}
		}
	}
	if !is_game_dir(game_dir) {
		if g := find_game(); g != "" {
			game_dir = g
			save_game_dir()
		}
	}
}

save_game_dir :: proc() {
	_ = os.make_directory_all(data_dir())
	_ = write_file_atomic(settings_path(), fmt.tprintf("game_dir=%s\n", game_dir))
}

set_note :: proc(s: string) {
	delete(note_msg)
	note_msg = strings.clone(s)
	note_until = time.tick_now()
}

// ---------------------------------------------------------------------------------------------- lr.cfg
cfg_reload :: proc() {
	lines := cfg_read(cfg_file)
	fresh := make([]string, len(lines))
	for l, i in lines {fresh[i] = strings.clone(l)}
	for l in cfg_cache {delete(l)}
	delete(cfg_cache)
	cfg_cache = fresh
}

cfg_get :: proc(key: string) -> string {
	v, _ := cfg_value(cfg_cache, key)
	return v
}

// edits go to the file as it is on disk now (the game may have changed it), then the cache is read again
cfg_put :: proc(edits: []Cfg_Edit) {
	if !cfg_write(cfg_file, edits) {set_note("Could not write lr.cfg.")}
	cfg_reload()
}

// ---------------------------------------------------------------------------------------------- profiles
profiles_refresh :: proc() {
	list := profile_list(profiles_dir, game_dir)
	fresh := make([]Profile, len(list))
	for p, i in list {
		fresh[i] = p
		fresh[i].name = strings.clone(p.name)
		fresh[i].path = strings.clone(p.path)
	}
	for p in profiles_cache {
		delete(p.name)
		delete(p.path)
	}
	delete(profiles_cache)
	profiles_cache = fresh
}

// the profile XRC_SAVE_DIR names: its index in the list (0: Steam Cloud, no line); -1: a folder that is not a profile
active_profile :: proc() -> (idx: int, other: string) {
	d := cfg_get("XRC_SAVE_DIR")
	if d == "" {return 0, ""}
	for p, i in profiles_cache {
		if !p.cloud && norm_path(p.path) == norm_path(d) {return i, ""}
	}
	return -1, d
}

select_profile :: proc(i: int) {
	if i < 0 || i >= len(profiles_cache) {return}
	p := profiles_cache[i]
	cfg_put({{"XRC_SAVE_DIR", p.cloud ? "" : p.path}})
}

// ---------------------------------------------------------------------------------------------- main
main :: proc() {
	args := os.args
	mode_shot, shot_tab, shot_png, tmpdir := false, "", "", ""
	selftest := false
	cfg_override := ""
	for i in 1 ..< len(args) {
		switch args[i] {
		case "--selftest":
			selftest = true
			if i + 1 < len(args) {tmpdir = args[i + 1]}
		case "--shot":
			if i + 2 < len(args) {mode_shot, shot_tab, shot_png = true, args[i + 1], args[i + 2]}
		case "--cfg":
			if i + 1 < len(args) {cfg_override = args[i + 1]}
		case "--profiles":
			if i + 1 < len(args) {profiles_dir = args[i + 1]}
		}
	}
	if selftest {os.exit(run_selftest(tmpdir))}

	lr_exe = pjoin({exe_dir(), "lr.exe"}, context.allocator)
	cfg_file = cfg_override != "" ? cfg_override : pjoin({exe_dir(), "lr.cfg"}, context.allocator)
	if profiles_dir == "" {profiles_dir = profiles_root()}
	profiles_dir = strings.clone(profiles_dir)
	_ = os.make_directory_all(data_dir())
	load_game_dir()
	monitor_w, monitor_h = monitor_size()
	cfg_reload()
	// the default output resolution is the monitor's: written once, so lr.exe uses what the window shows
	if !mode_shot && cfg_get("XRC_WIDTH") == "" && cfg_get("XRC_HEIGHT") == "" {
		cfg_put({{"XRC_WIDTH", itoa(monitor_w)}, {"XRC_HEIGHT", itoa(monitor_h)}})
	}
	profiles_refresh()
	media_start(game_dir)
	defer media_shutdown()
	if mode_shot {
		switch shot_tab {
		case "graphics": tab = .Graphics
		case "saves": tab = .Saves
		case "media": tab = .Media
		case: tab = .Play
		}
	}

	app, ok := iv.open({title = WINDOW_TITLE, width = 1040, height = 780, min_width = 900, min_height = 700, background = [3]u8{WIN_BG.r, WIN_BG.g, WIN_BG.b}})
	if !ok {
		fmt.eprintln("LRLauncher: could not open a window")
		os.exit(1)
	}
	defer iv.close(app)
	// the window's size is in pixels and the layout scales with the dpi: the window follows, so it fits the same
	if ds := app.window.dpi_scale; ds > 1.01 {
		app.window.desc.min_width, app.window.desc.min_height = int(900 * ds), int(700 * ds)
		platform.set_size(app.window, int(1040 * ds), int(780 * ds))
	}

	// Segoe UI (Windows' own) where it is, else the bundled Noto Sans / Liberation Sans
	lib: ren_assets.Library
	ren_assets.library_init(&lib)
	ren_assets.add_builtin_font(&lib, "noto_sans_regular")
	ren_assets.add_builtin_font(&lib, "liberation_sans_bold")
	regular, semibold := "noto_sans_regular", "liberation_sans_bold"
	fonts := pjoin({os.get_env("WINDIR", context.temp_allocator), "Fonts"}, context.temp_allocator)
	if ren_assets.font_add_file(&lib, "segoe", pjoin({fonts, "segoeui.ttf"}, context.temp_allocator)) {regular = "segoe"}
	if ren_assets.font_add_file(&lib, "segoe_sb", pjoin({fonts, "seguisb.ttf"}, context.temp_allocator)) {semibold = "segoe_sb"}
	ctx: input.Context(Signal)
	g := Gui{s = max(app.window.dpi_scale, 1), win = app.window, ctx = &ctx}
	g.font = ren_assets.font_make(&lib, &app.renderer, regular, 14 * g.s)
	g.small = ren_assets.font_make(&lib, &app.renderer, regular, 12 * g.s)
	g.bold = ren_assets.font_make(&lib, &app.renderer, semibold, 14 * g.s)
	g.big = ren_assets.font_make(&lib, &app.renderer, semibold, 20 * g.s)

	// The frame rate: 60 a second for a second after anything happened, else 20.
	started := time.tick_now()
	last_active := started
	shot_done := false
	for !app.window.close_requested {
		frame_start := time.tick_now()
		win_events := iv.pump(app, &ctx)
		if len(app.window.input) > 0 || win_events != {} || dlg != .None {last_active = frame_start}
		if .Focus_Gained in win_events {
			cfg_reload() // (the game's F10 menu may have changed it)
			profiles_refresh()
		}
		mx, my := input.mouse_position(&ctx)
		g.mouse = {int(mx), int(my)}
		g.clicked = input.mouse_pressed(&ctx, .Left)
		g.down = input.mouse_down(&ctx, .Left)
		g.modal = dlg != .None || browse_modal
		_, wy := input.wheel(&ctx)
		frame_wheel = int(wy * 3)

		g.dl = ren.begin_frame(&app.renderer, WIN_BG)
		g.screen = {0, 0, app.window.size.x, app.window.size.y}
		gui_begin(&g)
		draw_window(&g, app.window)
		gui_end(&g)
		ren.end_frame(&app.renderer)
		free_all(context.temp_allocator)

		if mode_shot {
			secs := time.duration_seconds(time.tick_since(started))
			if !shot_done && secs > 2.5 && capture_window(app.window, shot_png) {
				shot_done = true
				fmt.println("shot saved", shot_png)
				app.window.close_requested = true
			}
			if secs > 12 {
				fmt.println("shot FAILED", shot_png)
				os.exit(3)
			}
		}
		period := time.tick_since(last_active) < time.Second ? 16667 * time.Microsecond : 50 * time.Millisecond
		if mode_shot {period = 16667 * time.Microsecond}
		if spent := time.tick_since(frame_start); spent < period {time.sleep(period - spent)}
	}
}

browse_modal: bool

draw_window :: proc(g: ^Gui, w: ^platform.Window) {
	body := ui.inset_xy(g.screen, px(g, 24), px(g, 12))
	foot_h := px(g, 26)
	body_h := body.h - foot_h
	bar, rest := ui.cut_top({body.x, body.y, body.w, body_h}, px(g, 40))
	prev := tab
	tab = Tab(tabs(g, bar, {"Play", "Graphics", "Saves", "Media"}, int(tab)))
	if tab != prev {profiles_refresh(); edit_blur()}
	_, rest = ui.cut_top(rest, px(g, 14))
	switch tab {
	case .Play:
		draw_play(g, w, rest)
	case .Graphics:
		draw_graphics(g, rest)
	case .Saves:
		draw_saves(g, rest)
	case .Media:
		draw_media(g, rest)
	}
	// the foot: a message, and where lr.cfg is
	foot := ui.Rect{body.x, body.y + body_h, body.w, foot_h}
	if note_msg != "" && time.duration_seconds(time.tick_since(note_until)) < 8 {
		label(g, g.small, note_msg, foot, BAD)
	} else {
		label(g, g.small, fmt.tprintf("Settings file: %s", cfg_file), foot, FAINT)
	}
	switch dlg {
	case .None:
	case .New, .Copy, .Rename:
		draw_name_dialog(g)
	case .Delete:
		draw_delete_dialog(g)
	}
}

// ---------------------------------------------------------------------------------------------- Play
draw_play :: proc(g: ^Gui, w: ^platform.Window, r: ui.Rect) {
	running := sync.atomic_load(&game_running)
	game_ok := is_game_dir(game_dir)
	lr_ok := file_exists(lr_exe)

	// Save profile
	c1, rest := ui.cut_top(r, px(g, 92))
	card(g, c1)
	in1 := ui.inset_xy(c1, px(g, 20), px(g, 14))
	right, left := ui.cut_right(in1, px(g, 340), px(g, 12))
	l1, l2 := ui.cut_top(left, left.h / 2)
	label(g, g.bold, "Save profile", l1, TEXT)
	idx, other := active_profile()
	desc := "Saves go to Steam Cloud (the folder Steam syncs)"
	if idx > 0 {desc = fmt.tprintf("%d saves, last changed %s", profiles_cache[idx].saves, format_file_time(profiles_cache[idx].modified))} else if idx == 0 {
		p := profiles_cache[0]
		desc = p.exists ? fmt.tprintf("Steam Cloud: %d saves, last changed %s", p.saves, format_file_time(p.modified)) : "Steam Cloud (its folder was not found)"
	} else {desc = fmt.tprintf("A folder outside the profiles: %s", other)}
	label(g, g.font, desc, l2, DIM)
	names := make([dynamic]string, context.temp_allocator)
	for p in profiles_cache {append(&names, p.name)}
	sel := idx
	if idx < 0 {
		append(&names, fmt.tprintf("Other: %s", other))
		sel = len(names) - 1
	}
	dd := ui.Rect{right.x, right.y + (right.h - px(g, 34)) / 2, right.w - px(g, 110), px(g, 34)}
	if n, ch := dropdown(g, "play_profile", dd, names[:], sel); ch && n < len(profiles_cache) {select_profile(n)}
	if button(g, {right.x + right.w - px(g, 98), dd.y, px(g, 98), dd.h}, "Manage…") {tab = .Saves}
	_, rest = ui.cut_top(rest, px(g, 12))

	// Game folder
	c2, rest2 := ui.cut_top(rest, px(g, 92))
	card(g, c2)
	in2 := ui.inset_xy(c2, px(g, 20), px(g, 14))
	btns, left2 := ui.cut_right(in2, px(g, 220), px(g, 12))
	m1, m2 := ui.cut_top(left2, left2.h / 2)
	dot(g, m1.x + px(g, 5), m1.y + m1.h / 2, game_ok ? GOOD : BAD)
	label(g, g.bold, "Game folder", {m1.x + px(g, 20), m1.y, m1.w - px(g, 20), m1.h}, TEXT)
	loc := game_dir != "" ? game_dir : "not found: choose the folder with LRFF13.exe"
	if game_dir != "" && !game_ok {loc = fmt.tprintf("%s (no %s here)", game_dir, GAME_EXE)}
	label(g, g.font, loc, m2, game_ok ? DIM : BAD)
	by := btns.y + (btns.h - px(g, 34)) / 2
	if button(g, {btns.x, by, px(g, 104), px(g, 34)}, "Browse…") {browse_game(w)}
	if button(g, {btns.x + px(g, 112), by, px(g, 108), px(g, 34)}, "Detect") {
		if d := find_game(); d != "" {use_game_dir(d)} else {set_note("Steam's library has no Lightning Returns install.")}
	}
	tooltip_zone(g, "detect", {btns.x + px(g, 112), by, px(g, 108), px(g, 34)}, "Looks for Steam app 345350 in Steam's libraries.")
	_, rest2 = ui.cut_top(rest2, px(g, 12))

	// the runtime
	c3, rest3 := ui.cut_top(rest2, px(g, 76))
	card(g, c3)
	in3 := ui.inset_xy(c3, px(g, 20), px(g, 12))
	n1, n2 := ui.cut_top(in3, in3.h / 2)
	dot(g, n1.x + px(g, 5), n1.y + n1.h / 2, lr_ok ? GOOD : BAD)
	label(g, g.bold, "Runtime (lr.exe)", {n1.x + px(g, 20), n1.y, n1.w - px(g, 20), n1.h}, TEXT)
	label(g, g.font, lr_ok ? lr_exe : fmt.tprintf("%s not found: it must be in the same folder as this launcher", lr_exe), n2, lr_ok ? DIM : BAD)

	// Play
	_, bottom := ui.cut_top(rest3, px(g, 28))
	pr := ui.Rect{bottom.x, bottom.y, px(g, 220), px(g, 56)}
	can := game_ok && lr_ok && !running
	if button(g, pr, running ? "Running…" : "Play", can, f = g.big, green = true) {
		if !start_game(lr_exe, game_dir) {set_note("Could not start lr.exe.")}
	}
	tip := !game_ok ? "Choose the game folder first." : !lr_ok ? "lr.exe was not found beside the launcher." : running ? "The game started from this launcher is running." : fmt.tprintf("Starts: lr.exe \"%s\\%s\"", game_dir, GAME_EXE)
	tooltip_zone(g, "play", pr, tip)
	label(g, g.font, "Press F10 in the game for settings", {pr.x + pr.w + px(g, 20), pr.y, bottom.w - pr.w - px(g, 20), pr.h}, DIM)
}

use_game_dir :: proc(d: string) {
	delete(game_dir)
	game_dir = strings.clone(d)
	save_game_dir()
	media_shutdown()
	media_start(game_dir)
	profiles_refresh()
}

browse_game :: proc(w: ^platform.Window) {
	if p, picked := platform.pick_folder(w, "Choose the LIGHTNING RETURNS FINAL FANTASY XIII folder"); picked {
		if is_game_dir(p) {use_game_dir(p)} else {set_note(fmt.tprintf("%s has no %s.", p, GAME_EXE))}
	}
}

// ---------------------------------------------------------------------------------------------- Graphics
Choice :: struct {
	label, key: string,
	vals, names: []string,
	note: string,
}

ADV_STD_V := []string{"", "Advanced", "Standard"}
WINDOW_V := []string{"", "windowed"} // XRC_WINDOW (unset: borderless fullscreen, the runtime's default)
WINDOW_N := []string{"Borderless fullscreen", "Windowed"}
FXAA_V := []string{"", "2", "4", "8", "16"} // XRC_FXAA (the 3D image only: the HUD stays sharp)
FXAA_N := []string{"Off", "FXAA low", "FXAA medium", "FXAA high", "SMAA (sharper)"}
ADV_STD_N := []string{"As saved by the game", "Advanced", "Standard"}
INTERNAL_V := []string{"", "3840x2160", "3200x1800", "2880x1620", "2560x1440", "2400x1350", "2160x1215", "1920x1080", "1600x900", "1440x810", "1366x768", "1280x720"}
INTERNAL_N := []string{"As saved by the game", "3840 x 2160 (4K)", "3200 x 1800", "2880 x 1620", "2560 x 1440 (1440p)", "2400 x 1350", "2160 x 1215", "1920 x 1080 (1080p)", "1600 x 900", "1440 x 810", "1366 x 768", "1280 x 720 (720p)"}
SCALING_V := []string{"", "Advanced", "Standard", "None"}
SCALING_N := []string{"As saved by the game", "Advanced", "Standard", "None"}
FPS_V := []string{"", "Variable", "Stability"}
FPS_N := []string{"As saved by the game", "Variable", "Stable (30 fps)"}
CONFIRM_V := []string{"", "Normal", "Opposite"}
CONFIRM_N := []string{"As saved by the game", "Normal", "Opposite (Japanese layout)"}

Res :: struct {
	w, h: int,
	name: string,
}
OUT_RES := [?]Res{{1280, 720, "1280 x 720"}, {1920, 1080, "1920 x 1080"}, {2560, 1440, "2560 x 1440"}, {3440, 1440, "3440 x 1440 (ultrawide)"}, {3840, 2160, "3840 x 2160"}}

Rows :: struct {
	oi:      ui.Rect,
	row_h:   int,
	label_w: int,
	ctrl_w:  int,
	n:       int,
}

next_row :: proc(g: ^Gui, rs: ^Rows, name: string) -> (whole, ctrl, note: ui.Rect) {
	y := rs.oi.y + rs.n * rs.row_h
	whole = {rs.oi.x, y, rs.oi.w, rs.row_h}
	if rs.n > 0 {ren.draw_rect(g.dl, {rs.oi.x, y, rs.oi.w, 1}, CARD_BORDER)}
	label(g, g.font, name, {rs.oi.x, y, rs.label_w, rs.row_h}, TEXT)
	ctrl = {rs.oi.x + rs.label_w, y + (rs.row_h - px(g, 32)) / 2, rs.ctrl_w, px(g, 32)}
	note = {ctrl.x + rs.ctrl_w + px(g, 16), y, rs.oi.w - rs.label_w - rs.ctrl_w - px(g, 16), rs.row_h}
	rs.n += 1
	return
}

choice_row :: proc(g: ^Gui, rs: ^Rows, c: Choice) {
	_, ctrl, note := next_row(g, rs, c.label)
	cur := cfg_get(c.key)
	sel := -1
	for v, i in c.vals {if v == cur {sel = i}}
	items := make([dynamic]string, context.temp_allocator)
	append(&items, ..c.names)
	if sel < 0 { // (a value the launcher has no name for, set by hand or by the game: kept as it is)
		append(&items, fmt.tprintf("%s (custom)", cur))
		sel = len(items) - 1
	}
	if n, ch := dropdown(g, c.key, ctrl, items[:], sel); ch {
		v := n < len(c.vals) ? c.vals[n] : cur
		cfg_put({{c.key, v}})
	}
	label(g, g.small, c.note, note, DIM)
}

parse_dim :: proc(s: string) -> int {
	n, ok := strconv.parse_int(s)
	return ok ? n : 0
}

draw_graphics :: proc(g: ^Gui, r: ui.Rect) {
	head, rest := ui.cut_top(r, px(g, 22))
	label(g, g.font, "Applied the next time the game starts. Every choice is saved to lr.cfg at once.", head, DIM)
	_, rest = ui.cut_top(rest, px(g, 8))
	nrows := 17
	row_h := clamp((rest.h - px(g, 24)) / nrows, px(g, 30), px(g, 40))
	oc, _ := ui.cut_top(rest, nrows * row_h + px(g, 16))
	card(g, oc)
	rs := Rows{oi = ui.inset_xy(oc, px(g, 20), px(g, 8)), row_h = row_h, label_w = px(g, 230), ctrl_w = px(g, 290)}

	// Output resolution: the window / back buffer (XRC_WIDTH, XRC_HEIGHT)
	{
		_, ctrl, note := next_row(g, &rs, "Output resolution:")
		w, h := parse_dim(cfg_get("XRC_WIDTH")), parse_dim(cfg_get("XRC_HEIGHT"))
		if w <= 0 || h <= 0 {w, h = monitor_w, monitor_h}
		sel := len(OUT_RES) // Custom
		for p, i in OUT_RES {if p.w == w && p.h == h {sel = i}}
		if res_custom {sel = len(OUT_RES)}
		names := make([dynamic]string, context.temp_allocator)
		for p in OUT_RES {append(&names, p.name)}
		append(&names, "Custom")
		if n, ch := dropdown(g, "out_res", ctrl, names[:], sel); ch {
			if n < len(OUT_RES) {
				res_custom = false
				cfg_put({{"XRC_WIDTH", itoa(OUT_RES[n].w)}, {"XRC_HEIGHT", itoa(OUT_RES[n].h)}})
			} else {
				res_custom = true
				delete(cust_w)
				delete(cust_h)
				cust_w, cust_h = strings.clone(itoa(w)), strings.clone(itoa(h))
			}
		}
		if sel == len(OUT_RES) {
			if cust_w == "" {cust_w, cust_h = strings.clone(itoa(w)), strings.clone(itoa(h))}
			fw := ui.Rect{note.x, ctrl.y, px(g, 84), ctrl.h}
			fh := ui.Rect{note.x + px(g, 112), ctrl.y, px(g, 84), ctrl.h}
			nw, c1 := text_field(g, "cust_w", fw, cust_w, digits = true, max_len = 5)
			label(g, g.font, "x", {note.x + px(g, 88), ctrl.y, px(g, 20), ctrl.h}, DIM, center = true)
			nh, c2 := text_field(g, "cust_h", fh, cust_h, digits = true, max_len = 5)
			if c1 {delete(cust_w); cust_w = strings.clone(nw)}
			if c2 {delete(cust_h); cust_h = strings.clone(nh)}
			if c1 || c2 {
				cw, ch2 := parse_dim(cust_w), parse_dim(cust_h)
				if cw >= 640 && cw <= 16384 && ch2 >= 360 && ch2 <= 16384 {
					cfg_put({{"XRC_WIDTH", itoa(cw)}, {"XRC_HEIGHT", itoa(ch2)}})
				}
			}
			label(g, g.small, "pixels (640 x 360 and up)", {note.x + px(g, 206), note.y, note.w - px(g, 206), note.h}, DIM)
		} else {
			label(g, g.small, fmt.tprintf("The game window. Your monitor: %d x %d", monitor_w, monitor_h), note, DIM)
		}
	}
	choice_row(g, &rs, {"Display mode:", "XRC_WINDOW", WINDOW_V, WINDOW_N, "F11 or Alt+Enter switches in the game."})
	choice_row(g, &rs, {"Internal resolution:", "XRC_CFG_Graphics_Resolution", INTERNAL_V, INTERNAL_N, "Higher than the output = smoother edges."})
	choice_row(g, &rs, {"Anti-aliasing:", "XRC_FXAA", FXAA_V, FXAA_N, "Smooths 3D edges; the HUD stays sharp. Changes live (F10)."})
	choice_row(g, &rs, {"Scaling:", "XRC_CFG_Graphics_Scaling", SCALING_V, SCALING_N, ""})
	choice_row(g, &rs, {"Color correction:", "XRC_CFG_Graphics_ColorCorrection", ADV_STD_V, ADV_STD_N, ""})
	choice_row(g, &rs, {"Glare:", "XRC_CFG_Graphics_Glare", ADV_STD_V, ADV_STD_N, ""})
	choice_row(g, &rs, {"Depth of field:", "XRC_CFG_Graphics_DepthOfField", ADV_STD_V, ADV_STD_N, ""})
	choice_row(g, &rs, {"Shadows:", "XRC_CFG_Graphics_Shadowing", ADV_STD_V, ADV_STD_N, ""})
	choice_row(g, &rs, {"Lighting:", "XRC_CFG_Graphics_Lighting", ADV_STD_V, ADV_STD_N, ""})
	choice_row(g, &rs, {"Texture filtering:", "XRC_CFG_Graphics_TextureFiltering", ADV_STD_V, ADV_STD_N, ""})
	choice_row(g, &rs, {"Frame rate:", "XRC_CFG_Graphics_FrameRate", FPS_V, FPS_N, ""})
	choice_row(g, &rs, {"Confirm button layout:", "XRC_CFG_Control_ConfirmButtonLayout", CONFIRM_V, CONFIRM_N, ""})
	// HD cinematics: XRC_MOVIE_HD=0 is Off; On is no line
	{
		_, ctrl, note := next_row(g, &rs, "HD cinematics:")
		on := cfg_get("XRC_MOVIE_HD") != "0"
		if v, ch := toggle(g, ctrl, on); ch {cfg_put({{"XRC_MOVIE_HD", v ? "" : "0"}})}
		label(g, g.small, on ? "On: plays the upscaled movies once they are prepared (Media tab)" : "Off: the original 720p movies", note, DIM)
	}
	// Audio: XRC_NO_AUDIO=1 is Off; On is no line
	{
		_, ctrl, note := next_row(g, &rs, "Audio:")
		on := cfg_get("XRC_NO_AUDIO") == ""
		if v, ch := toggle(g, ctrl, on); ch {cfg_put({{"XRC_NO_AUDIO", v ? "" : "1"}})}
		label(g, g.small, on ? "Music, sound and voices" : "Muted", note, DIM)
	}
	// HD textures: XRC_TEX_HD=0 is Off; On is no line (also F10 in the game, at once)
	{
		_, ctrl, note := next_row(g, &rs, "HD textures:")
		on := cfg_get("XRC_TEX_HD") != "0"
		if v, ch := toggle(g, ctrl, on); ch {cfg_put({{"XRC_TEX_HD", v ? "" : "0"}})}
		label(g, g.small, on ? "On: AI-upscaled textures (up to 4K) once made on the Media tab" : "Off: the original textures", note, DIM)
	}
	// Collect textures: XRC_TEX_DUMP=<tex_collected beside lr.exe> while on
	{
		_, ctrl, note := next_row(g, &rs, "Collect textures:")
		on := cfg_get("XRC_TEX_DUMP") != ""
		if v, ch := toggle(g, ctrl, on); ch {cfg_put({{"XRC_TEX_DUMP", v ? tex_collect_dir() : ""}})}
		label(g, g.small, on ? "On: each new texture the game shows is saved for the upscaler" : "Off", note, DIM)
	}
}

// ---------------------------------------------------------------------------------------------- Saves
Dlg :: enum {
	None,
	New,
	Copy,
	Rename,
	Delete,
}
dlg: Dlg
dlg_name: string // what is typed
dlg_target: string // the profile being renamed or deleted
dlg_err: string

open_dlg :: proc(kind: Dlg, target := "", initial := "") {
	dlg = kind
	delete(dlg_target)
	dlg_target = strings.clone(target)
	delete(dlg_name)
	dlg_name = strings.clone(initial)
	dlg_err = ""
	if kind != .Delete {edit_focus("dlg_name", initial)} else {edit_blur()}
}

draw_saves :: proc(g: ^Gui, r: ui.Rect) {
	idx, other := active_profile()
	head, rest := ui.cut_top(r, px(g, 22))
	label(g, g.font, "The selected profile is where the game reads and writes its saves.", head, DIM)
	_, rest = ui.cut_top(rest, px(g, 8))
	bar, list := ui.cut_bottom(rest, px(g, 40))
	_, list = ui.cut_bottom(list, px(g, 10))

	// the list
	card(g, list)
	li := ui.inset_xy(list, px(g, 12), px(g, 10))
	hy := li.y
	label(g, g.small, "PROFILE", {li.x + px(g, 44), hy, px(g, 300), px(g, 18)}, FAINT)
	label(g, g.small, "SAVES", {li.x + li.w - px(g, 330), hy, px(g, 80), px(g, 18)}, FAINT)
	label(g, g.small, "LAST MODIFIED", {li.x + li.w - px(g, 220), hy, px(g, 200), px(g, 18)}, FAINT)
	rows := ui.Rect{li.x, li.y + px(g, 24), li.w, li.h - px(g, 24)}
	row_h := px(g, 56)
	clicked_row := -1
	for p, i in profiles_cache {
		rr := ui.Rect{rows.x, rows.y + i * row_h, rows.w, row_h - px(g, 4)}
		if rr.y + rr.h > rows.y + rows.h {break}
		active := i == idx
		hot := live(g) && inside(rr, g.mouse)
		fill_round(g, rr, px(g, 6), active ? ren.Color{0, 95, 184, 22} : hot ? ren.Color{0, 0, 0, 10} : ren.Color{0, 0, 0, 0})
		if active {fill_round(g, {rr.x, rr.y + rr.h / 2 - px(g, 11), px(g, 3), px(g, 22)}, px(g, 2), ACCENT)}
		// the radio dot
		cx, cy := rr.x + px(g, 24), rr.y + rr.h / 2
		circle(g, cx, cy, px(g, 18), active ? ACCENT : BTN_BORDER_BOTTOM)
		circle(g, cx, cy, px(g, 14), CARD)
		if active {circle(g, cx, cy, px(g, 8), ACCENT)}
		sub := p.cloud ? "Steam's own folder: only read, never changed here" : p.path
		nm := ui.Rect{rr.x + px(g, 44), rr.y + px(g, 6), rr.w - px(g, 400), px(g, 22)}
		label(g, g.bold, p.name, nm, TEXT)
		label(g, g.small, sub, {nm.x, nm.y + px(g, 22), nm.w, px(g, 18)}, DIM)
		if p.exists {
			label(g, g.font, p.saves == 1 ? "1 save" : fmt.tprintf("%d saves", p.saves), {rr.x + rr.w - px(g, 330), rr.y, px(g, 100), rr.h}, TEXT)
			label(g, g.font, format_file_time(p.modified), {rr.x + rr.w - px(g, 220), rr.y, px(g, 200), rr.h}, TEXT)
		} else {
			label(g, g.font, "folder not found", {rr.x + rr.w - px(g, 330), rr.y, px(g, 320), rr.h}, FAINT)
		}
		if hot {g.cursor = .Hand}
		if hot && g.clicked {clicked_row = i}
	}
	if idx < 0 {
		i := len(profiles_cache)
		rr := ui.Rect{rows.x, rows.y + i * row_h, rows.w, row_h - px(g, 4)}
		fill_round(g, rr, px(g, 6), ren.Color{0, 95, 184, 22})
		label(g, g.bold, "Other folder (set by hand in lr.cfg)", {rr.x + px(g, 44), rr.y + px(g, 6), rr.w - px(g, 60), px(g, 22)}, TEXT)
		label(g, g.small, other, {rr.x + px(g, 44), rr.y + px(g, 28), rr.w - px(g, 60), px(g, 18)}, DIM)
	}
	if clicked_row >= 0 && clicked_row != idx {select_profile(clicked_row)}

	// the buttons
	cloud := profiles_cache[0]
	can_edit := idx > 0
	x := bar.x
	bw := [5]int{px(g, 110), px(g, 210), px(g, 110), px(g, 110), px(g, 140)}
	if button(g, {x, bar.y, bw[0], bar.h}, "New") {open_dlg(.New)}
	tooltip_zone(g, "b_new", {x, bar.y, bw[0], bar.h}, "A new empty profile: a new game.")
	x += bw[0] + px(g, 8)
	if button(g, {x, bar.y, bw[1], bar.h}, "Copy from Steam Cloud", cloud.exists) {open_dlg(.Copy, "", "Steam Cloud copy")}
	tooltip_zone(g, "b_copy", {x, bar.y, bw[1], bar.h}, cloud.exists ? "Copies every save file of Steam Cloud into a new profile. Steam's folder is only read." : "Steam Cloud's save folder was not found.")
	x += bw[1] + px(g, 8)
	if button(g, {x, bar.y, bw[2], bar.h}, "Rename", can_edit) {open_dlg(.Rename, profiles_cache[idx].name, profiles_cache[idx].name)}
	x += bw[2] + px(g, 8)
	if button(g, {x, bar.y, bw[3], bar.h}, "Delete", can_edit) {open_dlg(.Delete, profiles_cache[idx].name)}
	tooltip_zone(g, "b_del", {x, bar.y, bw[3], bar.h}, "Moves the profile folder to the Recycle Bin.")
	x += bw[3] + px(g, 8)
	open_ok := idx >= 0 && profiles_cache[idx].exists
	if button(g, {x, bar.y, bw[4], bar.h}, "Open folder", open_ok) {open_folder(profiles_cache[idx].path)}
}

dialog_box :: proc(g: ^Gui, w, h: int) -> ui.Rect {
	ren.draw_rect(g.dl, g.screen, ren.Color{0, 0, 0, 77})
	box := ui.center(g.screen, w, h)
	for k in 1 ..= 6 {fill_round(g, ui.translate(ui.outset(box, k), 0, k), px(g, 8) + k, ren.Color{0, 0, 0, 6})}
	round_box(g, box, px(g, 8), ren.Color{255, 255, 255, 255}, ren.Color{200, 200, 200, 255})
	return box
}

dialog_buttons :: proc(g: ^Gui, box: ui.Rect) -> ui.Rect {
	strip := ui.Rect{box.x + 1, box.y + box.h - px(g, 80), box.w - 2, px(g, 80) - px(g, 8)}
	ren.draw_rect(g.dl, strip, WIN_BG)
	fill_round(g, {box.x + 1, box.y + box.h - px(g, 16), box.w - 2, px(g, 15)}, px(g, 7), WIN_BG)
	ren.draw_rect(g.dl, {box.x + 1, strip.y, box.w - 2, 1}, CARD_BORDER)
	return ui.Rect{box.x + px(g, 24), strip.y + px(g, 24), box.w - px(g, 48), px(g, 32)}
}

draw_name_dialog :: proc(g: ^Gui) {
	g.in_modal = true
	defer g.in_modal = false
	box := dialog_box(g, px(g, 520), px(g, 280))
	in_ := ui.inset_xy(box, px(g, 24), px(g, 22))
	title := dlg == .New ? "New profile" : dlg == .Copy ? "Copy from Steam Cloud" : "Rename profile"
	ren.draw_text(g.dl, g.big, title, {in_.x, in_.y}, TEXT)
	y := in_.y + g.big.line_height + px(g, 10)
	help := dlg == .New ? "An empty profile: the game starts a new game." : dlg == .Copy ? "Every save file of Steam Cloud is copied into the new profile." : fmt.tprintf("New name for \"%s\".", dlg_target)
	y += paragraph(g, g.font, help, {in_.x, y, in_.w, 0}, DIM, px(g, 2)) + px(g, 10)
	f := ui.Rect{in_.x, y, in_.w, px(g, 36)}
	nm, ch := text_field(g, "dlg_name", f, dlg_name, max_len = 60, hint = "Profile name")
	if ch || edit_has("dlg_name") {
		if nm != dlg_name {
			delete(dlg_name)
			dlg_name = strings.clone(nm)
		}
		if ch {dlg_err = ""}
	}
	if dlg_err != "" {label(g, g.small, dlg_err, {in_.x, y + px(g, 42), in_.w, px(g, 20)}, BAD)}
	btns := dialog_buttons(g, box)
	ok_text := dlg == .New ? "Create" : dlg == .Copy ? "Copy" : "Rename"
	enter := input.key_pressed(g.ctx, .Enter)
	if button(g, ui.columns(btns, 0, 2, px(g, 8)), ok_text, primary = true) || enter {finish_name_dialog()}
	if button(g, ui.columns(btns, 1, 2, px(g, 8)), "Cancel") || input.key_pressed(g.ctx, .Escape) {dlg = .None; edit_blur()}
}

finish_name_dialog :: proc() {
	name := strings.trim_space(dlg_name)
	path, err := "", ""
	switch dlg {
	case .New:
		path, err = profile_create(profiles_dir, name)
	case .Copy:
		path, err = profile_copy_from(profiles_dir, name, profiles_cache[0].path)
	case .Rename:
		was := active_profile_name() == dlg_target
		path, err = profile_rename(profiles_dir, dlg_target, name)
		if err == "" && was {cfg_put({{"XRC_SAVE_DIR", path}})}
	case .Delete, .None:
	}
	if err != "" {
		dlg_err = strings.clone(err)
		if path == "" {return}
		// (a copy that left some files behind: the profile exists; say so, and keep the dialog's message)
		set_note(err)
	}
	if dlg == .New || dlg == .Copy {cfg_put({{"XRC_SAVE_DIR", path}})}
	dlg = .None
	edit_blur()
	profiles_refresh()
}

active_profile_name :: proc() -> string {
	i, _ := active_profile()
	if i > 0 {return profiles_cache[i].name}
	return ""
}

draw_delete_dialog :: proc(g: ^Gui) {
	g.in_modal = true
	defer g.in_modal = false
	box := dialog_box(g, px(g, 520), px(g, 250))
	in_ := ui.inset_xy(box, px(g, 24), px(g, 22))
	ren.draw_text(g.dl, g.big, "Delete this profile?", {in_.x, in_.y}, TEXT)
	y := in_.y + g.big.line_height + px(g, 12)
	text := fmt.tprintf("The folder of \"%s\" and its saves are moved to the Recycle Bin. You can restore it from there. Steam Cloud is not touched.", dlg_target)
	paragraph(g, g.font, text, {in_.x, y, in_.w, 0}, TEXT, px(g, 2))
	if dlg_err != "" {label(g, g.small, dlg_err, {in_.x, box.y + box.h - px(g, 110), in_.w, px(g, 20)}, BAD)}
	btns := dialog_buttons(g, box)
	if button(g, ui.columns(btns, 0, 2, px(g, 8)), "Move to Recycle Bin", primary = true) {
		was := active_profile_name() == dlg_target
		if err := profile_delete(profiles_dir, dlg_target); err != "" {
			dlg_err = strings.clone(err)
		} else {
			if was {cfg_put({{"XRC_SAVE_DIR", ""}})}
			dlg = .None
			profiles_refresh()
		}
	}
	if button(g, ui.columns(btns, 1, 2, px(g, 8)), "Keep it") || input.key_pressed(g.ctx, .Escape) {dlg = .None}
}

// ---------------------------------------------------------------------------------------------- Media
draw_media :: proc(g: ^Gui, r: ui.Rect) {
	m := get_media()
	running := m.external || m.started
	card_h := px(g, 330)
	c, rest := ui.cut_top(r, card_h, px(g, 16))
	card(g, c)
	in_ := ui.inset_xy(c, px(g, 24), px(g, 18))
	y := in_.y
	ren.draw_text(g.dl, g.big, "HD cinematics (AI 4K)", {in_.x, y}, TEXT)
	y += g.big.line_height + px(g, 8)
	y += paragraph(g, g.font, "Converts every cinematic to MP4, then upscales it with Real-ESRGAN to 4K. It runs in the background at below-normal priority, can be stopped and continued, and takes about a day. The game plays the HD version of a movie when it exists and HD cinematics is On in the Graphics tab.", {in_.x, y, in_.w, 0}, DIM, px(g, 3))
	y += px(g, 14)

	// the count and a bar
	total := max(m.base, m.hd)
	label(g, g.bold, "Upscaled movies", {in_.x, y, px(g, 200), px(g, 22)}, TEXT)
	label(g, g.font, total > 0 ? fmt.tprintf("%d of %d", m.hd, total) : fmt.tprintf("%d (no converted movies found yet)", m.hd), {in_.x + px(g, 200), y, in_.w - px(g, 200), px(g, 22)}, TEXT)
	y += px(g, 26)
	frac := total > 0 ? f32(m.hd) / f32(total) : 0
	progress_bar(g, {in_.x, y, in_.w, px(g, 4)}, frac, false)
	y += px(g, 18)

	// the status line
	label(g, g.bold, "Status", {in_.x, y, px(g, 200), px(g, 22)}, TEXT)
	st := m.status
	if st == "" {st = m.status_age < 0 ? "no status file yet" : "(empty)"}
	label(g, g.font, st, {in_.x + px(g, 200), y, in_.w - px(g, 200), px(g, 22)}, TEXT)
	y += px(g, 24)
	age_txt := m.status_age < 0 ? "" : m.status_age < 90 ? fmt.tprintf("updated %d s ago", int(m.status_age)) : m.status_age < 5400 ? fmt.tprintf("updated %d min ago", int(m.status_age / 60)) : fmt.tprintf("updated %.1f h ago", m.status_age / 3600)
	label(g, g.small, fmt.tprintf("%s   %s", age_txt, m.xrc_dir), {in_.x + px(g, 200), y, in_.w - px(g, 200), px(g, 18)}, FAINT)
	y += px(g, 26)

	// is a job running
	dot(g, in_.x + px(g, 5), y + px(g, 11), running ? GOOD : FAINT)
	jt := "No job is running."
	if m.started {
		jt = fmt.tprintf("A job started by this launcher is running (process %d).", m.started_pid)
	} else if m.external {
		jt = "A job is running that this launcher did not start. Its status is shown above; a second job will not be started."
	}
	label(g, g.font, jt, {in_.x + px(g, 20), y, in_.w - px(g, 20), px(g, 22)}, running ? GOOD : DIM)
	y += px(g, 34)

	// the buttons
	game_ok := is_game_dir(game_dir)
	can_start := !running && game_ok && m.script_ok && file_exists(lr_exe)
	pb := ui.Rect{in_.x, y, px(g, 430), px(g, 44)}
	if button(g, pb, "Prepare HD cinematics (AI 4K, slow: about a day)", can_start, primary = true, f = g.bold) {
		if err := prepare_start(game_dir, lr_exe); err != "" {set_note(err)}
	}
	ptip := running ? "A job is already running: only one at a time." : !game_ok ? "Choose the game folder on the Play tab first." : !m.script_ok ? fmt.tprintf("Not found: %s", m.script) : "Runs prepare_movies.ps1 for this game: converts the movies, then upscales them."
	tooltip_zone(g, "prepare", pb, ptip)
	sb := ui.Rect{pb.x + pb.w + px(g, 12), y, px(g, 110), px(g, 44)}
	if button(g, sb, "Stop", m.started) {prepare_stop()}
	tooltip_zone(g, "stop", sb, m.started ? "Ends the job this launcher started (its processes, by id). It can be continued later." : "Only a job started by this launcher can be stopped here.")
	y += px(g, 56)
	label(g, g.small, fmt.tprintf("Script: %s", m.script), {in_.x, y, in_.w, px(g, 18)}, m.script_ok ? FAINT : BAD)

	// HD textures: collected while playing (Graphics tab: Collect textures), upscaled here into hd_textures
	{
		c2, _ := ui.cut_top(rest, px(g, 250))
		card(g, c2)
		i2 := ui.inset_xy(c2, px(g, 24), px(g, 18))
		y2 := i2.y
		ren.draw_text(g.dl, g.big, "HD textures (AI, up to 4K)", {i2.x, y2}, TEXT)
		y2 += g.big.line_height + px(g, 8)
		y2 += paragraph(g, g.font, "With Collect textures on (Graphics tab), the game saves each texture it shows. This upscales them with Real-ESRGAN: 4x up to 512 pixels, 2x above, at most 4096. About 3 s a texture, in the background; it can be stopped and continued. Run it again after playing new areas.", {i2.x, y2, i2.w, 0}, DIM, px(g, 3))
		y2 += px(g, 14)
		label(g, g.bold, "Upscaled textures", {i2.x, y2, px(g, 200), px(g, 22)}, TEXT)
		label(g, g.font, fmt.tprintf("%d of %d collected", min(m.tex_hd, m.tex_collected), m.tex_collected), {i2.x + px(g, 200), y2, i2.w - px(g, 200), px(g, 22)}, TEXT)
		y2 += px(g, 26)
		progress_bar(g, {i2.x, y2, i2.w, px(g, 4)}, m.tex_collected > 0 ? f32(min(m.tex_hd, m.tex_collected)) / f32(m.tex_collected) : 0, false)
		y2 += px(g, 18)
		tb := ui.Rect{i2.x, y2, px(g, 430), px(g, 44)}
		can := !running && m.tex_ok && m.tex_collected > m.tex_hd
		if button(g, tb, "Upscale collected textures", can, primary = true, f = g.bold) {
			if err := tex_start(); err != "" {set_note(err)}
		}
		tooltip_zone(g, "tex_up", tb, running ? "A job is already running: only one at a time." : !m.tex_ok ? fmt.tprintf("Not found: %s", tex_script()) : m.tex_collected == 0 ? "Nothing collected yet: turn on Collect textures and play." : m.tex_collected <= m.tex_hd ? "Every collected texture is upscaled." : "Runs upscale_textures.py on the collected textures.")
		fb := ui.Rect{tb.x + tb.w + px(g, 12), y2, px(g, 180), px(g, 44)}
		if button(g, fb, "Open folder", dir_exists(tex_hd_dir())) {open_url(tex_hd_dir())}
	}
}

// ---------------------------------------------------------------------------------------------- the window's picture
// The window's client area as a PNG (for the README and for checking the layout): PrintWindow, else the screen.
capture_window :: proc(w: ^platform.Window, path: string) -> bool {
	rc: win.RECT
	win.GetClientRect(w.hwnd, &rc)
	width, height := int(rc.right - rc.left), int(rc.bottom - rc.top)
	if width <= 0 || height <= 0 {return false}
	screen := win.GetDC(nil)
	defer win.ReleaseDC(nil, screen)
	mem := win.CreateCompatibleDC(screen)
	defer win.DeleteDC(mem)
	bmp := win.CreateCompatibleBitmap(screen, i32(width), i32(height))
	defer win.DeleteObject(win.HGDIOBJ(bmp))
	old := win.SelectObject(mem, win.HGDIOBJ(bmp))
	defer win.SelectObject(mem, old)
	pt := win.POINT{0, 0}
	win.ClientToScreen(w.hwnd, &pt)
	// PrintWindow asks the window itself for its picture (PW_CLIENTONLY | PW_RENDERFULLCONTENT): nothing else on the screen is copied
	if !PrintWindow(w.hwnd, mem, 1 | 2) {return false}
	bi := win.BITMAPINFO{bmiHeader = {biSize = size_of(win.BITMAPINFOHEADER), biWidth = i32(width), biHeight = -i32(height), biPlanes = 1, biBitCount = 32, biCompression = win.BI_RGB}}
	px_bgra := make([][4]u8, width * height, context.temp_allocator)
	if win.GetDIBits(mem, bmp, 0, u32(height), raw_data(px_bgra), &bi, win.DIB_RGB_COLORS) == 0 {return false}
	for &p in px_bgra {p = {p[2], p[1], p[0], 255}}
	// (PrintWindow sometimes hands back an empty window: the caller tries again on a later frame)
	busy := 0
	for p in px_bgra {if p != {243, 243, 243, 255} {busy += 1}}
	if busy < 4000 {return false}
	return save_png(px_bgra, width, height, path)
}
