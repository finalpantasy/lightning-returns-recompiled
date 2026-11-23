/* xrc_tool_env.h - the debugging and profiling switches (XRC_PROFILE, XRC_TRACE_*, logs, stats, self-checks,
 * scripted input, screenshots): read with xrc_tool_env, which a build with -DXRC_NO_TOOLS (the launcher's
 * "Debug & Profile Tools: OFF") makes always 0, so none of them can be turned on and their code is left out. */
#ifndef XRC_TOOL_ENV_H
#define XRC_TOOL_ENV_H
#include <stdlib.h>
#ifdef XRC_NO_TOOLS
#define xrc_tool_env(name) ((void)(name), (char *)0)
#else
#define xrc_tool_env(name) getenv(name)
#endif
#endif
