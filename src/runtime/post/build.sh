#!/bin/sh
# Compiles vk_post.c's shaders to SPIR-V as C arrays (post_spv.h). Needs glslang (set GLSLANG to its path).
set -e
cd "$(dirname "$0")"
G="${GLSLANG:-glslang}"
: > post_spv.h
for s in post.vert post.frag; do
	n=post_$(echo $s | tr . _)
	$G -V --target-env vulkan1.1 --vn $n -o $n.h $s
	cat $n.h >> post_spv.h; rm $n.h
done
