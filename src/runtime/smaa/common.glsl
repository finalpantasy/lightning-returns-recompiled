// SMAA 1x (Jorge Jimenez et al., MIT: LICENSE.txt) for the Vulkan backend's anti-aliasing. Push constants: rt = (1/w, 1/h, w, h).
layout(push_constant) uniform PC { vec4 rt; vec4 pad; } pc;
#define SMAA_RT_METRICS pc.rt
#define SMAA_GLSL_4
#define SMAA_PRESET_ULTRA
