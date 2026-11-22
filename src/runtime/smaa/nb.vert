#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#define SMAA_INCLUDE_PS 0
#include "SMAA.hlsl"
layout(location = 0) in vec4 ipos;
layout(location = 1) in vec4 itc;
layout(location = 0) out vec2 tc;
layout(location = 1) out vec4 off;
void main() { gl_Position = ipos; tc = itc.xy; SMAANeighborhoodBlendingVS(tc, off); }
