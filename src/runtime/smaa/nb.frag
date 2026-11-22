#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
#define SMAA_INCLUDE_VS 0
#include "SMAA.hlsl"
layout(set = 0, binding = 2) uniform sampler2D colorTex;
layout(set = 0, binding = 3) uniform sampler2D blendTex;
layout(location = 0) in vec2 tc;
layout(location = 1) in vec4 off;
layout(location = 0) out vec4 o;
void main() { o = SMAANeighborhoodBlendingPS(tc, off, colorTex, blendTex); }
