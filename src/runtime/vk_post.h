/* vk_post.h -- the Vulkan backend's last passes over the picture (vk_post.c): a Lanczos rescale (the game's StretchRect
 * into the back buffer and the back buffer onto the screen, when their sizes differ), contrast-adaptive sharpening, and
 * debanding / dither. vk_d3d9.c calls xrc_post_blit where it would otherwise vkCmdBlitImage / vkCmdCopyImage. */
#ifndef XRC_VK_POST_H
#define XRC_VK_POST_H
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

/* the device, and the backend's vkCmd* (its own: with its render thread they become stream packets) */
typedef struct {
	VkDevice dev;
	PFN_vkGetDeviceProcAddr gdpa;
	const VkPhysicalDeviceMemoryProperties *mem;
	float ts_period; /* (ns per timestamp tick: XRC_POST_BENCH) */
	PFN_vkCmdPipelineBarrier barrier;
	PFN_vkCmdBeginRendering begin;
	PFN_vkCmdEndRendering end;
	PFN_vkCmdBindPipeline bind;
	PFN_vkCmdPushDescriptorSetKHR push_desc;
	PFN_vkCmdPushConstants push_const;
	PFN_vkCmdSetViewport viewport;
	PFN_vkCmdSetScissor scissor;
	PFN_vkCmdDraw draw;
	PFN_vkCmdCopyImage copy;
	PFN_vkCmdResetQueryPool reset_q;
	PFN_vkCmdWriteTimestamp timestamp;
} XrcPostVk;
void xrc_post_init(const XrcPostVk *v);

/* would a copy of sw x sh source pixels into dw x dh be drawn here? stage 0: the game's StretchRect into the back buffer
 * (only a rescale is done there); 1: the back buffer onto the screen (rescale, sharpen, deband) */
int xrc_post_active(int stage, unsigned sw, unsigned sh, unsigned dw, unsigned dh);
/* b's source rectangle of simg (TRANSFER_SRC layout before and after; sview its sampled view) drawn into b's destination
 * rectangle of dimg (TRANSFER_DST layout, format dfmt). 1: done; 0: not done (the caller copies as before). */
int xrc_post_blit(VkCommandBuffer cb, VkImage simg, VkImageView sview, unsigned sw, unsigned sh, VkImage dimg, VkFormat dfmt,
                  const VkImageBlit *b, int stage, unsigned long long frame);

/* live settings (the F10 menu); env defaults in brackets, read at the first frame:
 *   xrc_vk_set_downscale  0 bilinear (as before), 1 Lanczos                      [XRC_DOWNSCALE, default 0]
 *   xrc_vk_set_cas        0 off, else sharpness 0.0..1.0                         [XRC_CAS, default 0.25]
 *   xrc_vk_set_deband     0 off, 1 dither only, 2 deband + dither                [XRC_DEBAND, default 0] */
void xrc_vk_set_downscale(int m);
void xrc_vk_set_cas(float s);
void xrc_vk_set_deband(int m);
/* (XRC_SHOT_AB=post: the A shot with all of the above off) */
void xrc_post_bypass(int on);
#endif
