#!/bin/sh
# Builds lr.exe (the "integ5" feature set) from this repo plus the translator's output for YOUR copy of the game.
# Nothing game-derived is in this repo: the pages (page_*.c) and xrc_table.c are generated from your own exe.
#
#   LR_GEN=<dir>        translator output: <dir>/src/page_*.c and <dir>/xrc_table.c        (required)
#   OUT=<dir>           build output folder, default ./out
#   LLVM_MINGW=<dir>    llvm-mingw root (x86_64-w64-mingw32-clang in <dir>/bin)             (required unless on PATH)
#   FFMPEG=<dir>        shared FFmpeg 7+/9 dev build (include/ lib/), LGPL                  (required)
#   VULKAN_HEADERS=<dir> KhronosGroup/Vulkan-Headers checkout (include/vulkan/vulkan.h)   (required)
#   JOBS                parallel compiles, default 4
#   step: rt | pages | link | all (default all)
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd); cd "$HERE"
: "${LR_GEN:?set LR_GEN}"; : "${FFMPEG:?set FFMPEG}"; : "${VULKAN_HEADERS:?set VULKAN_HEADERS}"
[ -n "$LLVM_MINGW" ] && PATH="$LLVM_MINGW/bin:$PATH"
OUT=${OUT:-$HERE/out}; JOBS=${JOBS:-4}; CC=x86_64-w64-mingw32-clang
mkdir -p "$OUT/rt" "$OUT/pages" "$OUT/math_src"
RTF="-m64 -O1 -march=native -ffast-math -fno-associative-math -mrecip=none -ffp-contract=off -Wno-overriding-option -Wall -DXRC_LR -DXRC_VULKAN -I$FFMPEG/include -I$VULKAN_HEADERS/include -Isrc/include -Isrc/runtime"
PGF="-m64 -march=native -ffast-math -fno-associative-math -mrecip=none -O1 -w -fno-strict-aliasing -fwrapv -DXRC_LR -Isrc/include"
MATH_FUNCS="00408210 0040acb0 0040ab00 0040b1e0 00416140 004164f0 00416020 00416250 00cf1a20 005a44e0 004bb0b0 004bada0 00437270 00408320 004bb1c0 00416a30 00aac130"
# pages replaced or recompiled with extra flags (the rest are compiled stock)
SPECIAL="0079 007b 0080 009e 00ab 00ac 0040 0041 0043 004b 005a 00aa 00cf 00a6 00b0 00b8"
step=${1:-all}

if [ $step = rt_one ]; then
	n=$2; src=src/runtime/$n.c; [ -f $src ] || src=src/lr_stubs/$n.c
	$CC $RTF -c $src -o "$OUT/rt/rt_$n.o" 2> "$OUT/rt/rt_$n.err" && echo "OK $n" || { echo "FAIL $n"; head -20 "$OUT/rt/rt_$n.err"; exit 1; }
	exit
fi
if [ $step = page_one ]; then
	p=$2; $CC $PGF $3 -c "$4" -o "$OUT/pages/page_$p.o" && echo "OK page_$p" || { echo "FAIL page_$p"; exit 1; }
	exit
fi
if [ $step = rt ] || [ $step = all ]; then
	for f in src/runtime/*.c src/lr_stubs/match_table.c; do n=$(basename $f .c); echo $n; done | xargs -P $JOBS -n 1 sh "$0" rt_one
fi
if [ $step = pages ] || [ $step = all ]; then
	pg() { sh "$0" page_one "$@"; }
	for p in $(ls "$LR_GEN"/src/page_*.c | sed 's#.*/page_\(.*\)\.c#\1#'); do
		case " $SPECIAL " in *" $p "*) continue;; esac
		echo "$p $LR_GEN/src/page_$p.c"
	done | xargs -P $JOBS -L 1 sh -c 'sh "$0" page_one $1 "" $2' "$0"
	for p in 0079 007b 0080 009e; do pg $p "-include src/runtime/ovr_fast_calls.h" "$LR_GEN/src/page_$p.c"; done
	for p in 00ab 00ac; do pg $p "-include src/runtime/ovr_frame_calls.h" "$LR_GEN/src/page_$p.c"; done
	# pages that define a math helper: the definition is renamed xrc_orig_<addr> so ovr_math.c's xf_<addr> is the one called
	for p in 0040 0041 0043 004b 005a 00aa 00cf 00b0 00b8; do
		cp "$LR_GEN/src/page_$p.c" "$OUT/math_src/page_$p.c"
		for a in $MATH_FUNCS 0040abc0 00b09d50 00b8edb0; do
			case $a in $p*) sed -i "s/^void XRC_CC xf_$a(XCtx \*c) {\$/void XRC_CC xrc_orig_$a(XCtx *c) {/" "$OUT/math_src/page_$p.c";; esac
		done
		case $p in 00b0|00b8) fl="";; *) fl="-ffp-contract=off";; esac
		pg $p "$fl" "$OUT/math_src/page_$p.c"
	done
	# page 00a6: the shadow-stability hook (xrc_shadow_hold)
	LR_MATH_SRC="$OUT/math_src" python -I tools/patch_00a6.py && pg 00a6 "" "$OUT/math_src/page_00a6.c"
fi
if [ $step = link ] || [ $step = all ]; then
	$CC -m64 -O0 -w -DXRC_LR -Isrc/include -Isrc/runtime -c "$LR_GEN/xrc_table.c" -o "$OUT/xrc_table.o"
	$CC -m64 -o "$OUT/lr.exe" "$OUT"/rt/rt_*.o "$OUT/xrc_table.o" "$OUT"/pages/page_*.o \
		-L"$FFMPEG/lib" -lavformat -lavcodec -lavutil -lswresample -lgdi32 -luser32 -lwinmm -ldwmapi -Wl,-Bstatic -lpthread -Wl,-Bdynamic
	echo "linked $OUT/lr.exe"
fi
