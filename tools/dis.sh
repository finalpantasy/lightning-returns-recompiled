#!/bin/bash
# dis.sh <hexaddr>: the disassembly comments of one generated function
cd "${LR_GEN:?set LR_GEN to the translator output dir (holds src/page_*.c)}/src"
a=$((16#${1#0x})); f=page_$(printf %04x $((a >> 16))).c
n=$(grep -n "^/\* ---- $(printf %08x $a) ---- \*/" $f | cut -d: -f1)
[ -z "$n" ] && { echo "no function at $1"; exit 1; }
awk -v s=$n 'NR>s && /\/\* ---- /{exit} NR>=s' $f | grep -o "/\* 00[0-9a-f]\{6\} [^*]*\*/" | sed 's#/\* ##;s# \*/##' | awk '{ $2=""; print }'
