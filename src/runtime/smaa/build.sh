#!/bin/sh
# Compiles the SMAA passes to SPIR-V as C arrays (smaa_spv.h). Needs glslang (set GLSLANG to its path).
set -e
cd "$(dirname "$0")"
G="${GLSLANG:-glslang}"
: > smaa_spv.h
for s in edge.vert edge.frag blend.vert blend.frag nb.vert nb.frag; do
	n=smaa_$(echo $s | tr . _)
	$G -V --target-env vulkan1.1 --vn $n -o $n.h $s
	cat $n.h >> smaa_spv.h; rm $n.h
done
