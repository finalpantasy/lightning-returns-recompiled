#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#define SMAA_INCLUDE_VS 0
#include "SMAA.hlsl"
layout(set = 0, binding = 2) uniform sampler2D edgesTex;
layout(set = 0, binding = 3) uniform sampler2D areaTex;
layout(set = 0, binding = 4) uniform sampler2D searchTex;
layout(location = 0) in vec2 tc;
layout(location = 1) in vec2 pix;
layout(location = 2) in vec4 off[3];
layout(location = 0) out vec4 o;
void main() { o = SMAABlendingWeightCalculationPS(tc, pix, off, edgesTex, areaTex, searchTex, vec4(0.0)); }
