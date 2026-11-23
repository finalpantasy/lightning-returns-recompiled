/* vk_post.c -- the Vulkan backend's last passes over the picture (see vk_post.h).
 *
 * Where vk_d3d9.c would copy with vkCmdBlitImage (linear: two taps a side, so a shrink by 1.5x or more drops source pixels
 * and shimmers), the copy is drawn instead: a Lanczos-2 filter whose footprint widens with the shrink (every source pixel
 * counted), then -- on the way to the screen only -- contrast-adaptive sharpening (the method of AMD FidelityFX CAS, MIT
 * licence; written here from its description: a 3x3 min/max sets how much of a negative-lobe cross filter each pixel
 * gets, so already-sharp edges are left alone), and a deband / dither. The result goes into an image of the destination
 * rectangle's size and is copied into place, so the callers' layouts and barriers stay as they were.
 *
 * Shaders: post/post.vert, post/post.frag; post/build.sh makes post/post_spv.h with glslang.
 * Env: XRC_DOWNSCALE, XRC_CAS, XRC_DEBAND (vk_post.h); XRC_POST_BENCH=1 logs the passes' GPU time. */
#include "vk_post.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "post/post_spv.h"

typedef unsigned u32;
static XrcPostVk V;
static int state; /* 0 not set up, 1 ready, -1 failed */
static PFN_vkCreateShaderModule pCreateShaderModule;
static PFN_vkCreateDescriptorSetLayout pCreateDescriptorSetLayout;
static PFN_vkCreatePipelineLayout pCreatePipelineLayout;
static PFN_vkCreateGraphicsPipelines pCreateGraphicsPipelines;
static PFN_vkCreateSampler pCreateSampler;
static PFN_vkCreateImage pCreateImage;
static PFN_vkGetImageMemoryRequirements pGetImageMemoryRequirements;
static PFN_vkAllocateMemory pAllocateMemory;
static PFN_vkBindImageMemory pBindImageMemory;
static PFN_vkCreateImageView pCreateImageView;
static PFN_vkDestroyImage pDestroyImage;
static PFN_vkDestroyImageView pDestroyImageView;
static PFN_vkFreeMemory pFreeMemory;
static PFN_vkCreateQueryPool pCreateQueryPool;
static PFN_vkGetQueryPoolResults pGetQueryPoolResults;

static VkShaderModule vsm, fsm;
static VkDescriptorSetLayout dsl;
static VkPipelineLayout pl;
static VkSampler smp;
static struct { VkFormat f; VkPipeline p; } pipes[8];

/* settings (-1: from the env at the first use) */
static int set_down = -1, set_deband = -1, bypass;
static float set_cas = -1.0f;
void xrc_vk_set_downscale(int m) { set_down = m < 0 ? 0 : m; }
void xrc_vk_set_cas(float s) { set_cas = s < 0.0f ? 0.0f : s > 1.0f ? 1.0f : s; }
void xrc_vk_set_deband(int m) { set_deband = m < 0 ? 0 : m > 2 ? 2 : m; }
void xrc_post_bypass(int on) { bypass = on; }
static void settings(void) {
	const char *e;
	static int ab_read;
	if (!ab_read) { ab_read = 1; e = getenv("XRC_SHOT_AB"); if (e && !strcmp(e, "post")) bypass = 1; } /* (the A shots: off, from the start) */
	if (set_down < 0) { e = getenv("XRC_DOWNSCALE"); set_down = e ? atoi(e) : 1; if (set_down < 0) set_down = 0; fprintf(stderr, "xrc: vulkan: supersampling filter %s (%s)\n", set_down ? "Lanczos" : "bilinear", e ? "set by the user" : "default"); } /* (integ5: Lanczos by default; the menu saves bilinear as 0) */
	if (set_cas < 0.0f) { e = getenv("XRC_CAS"); xrc_vk_set_cas(e ? (float)atof(e) : 0.25f); }
	if (set_deband < 0) { e = getenv("XRC_DEBAND"); xrc_vk_set_deband(e ? atoi(e) : 0); }
}

/* own images: per stage the result (the destination's format) and, for two passes, the rescaled picture (RGBA16F) */
typedef struct { VkImage img; VkImageView view; VkDeviceMemory mem; u32 w, h; VkFormat f; } PImg;
static PImg imgs[2][2];
static struct { PImg i; unsigned long long frame; } dead[16];
static unsigned long long cur_frame;
static void img_free(PImg *i) {
	if (i->view) pDestroyImageView(V.dev, i->view, 0);
	if (i->img) pDestroyImage(V.dev, i->img, 0);
	if (i->mem) pFreeMemory(V.dev, i->mem, 0);
	memset(i, 0, sizeof *i);
}
static void img_drop(PImg *i) { /* (freed some frames later: the GPU may still use it) */
	int k;
	if (!i->img) return;
	for (k = 0; k < 16; k++) if (!dead[k].i.img) { dead[k].i = *i; dead[k].frame = cur_frame; memset(i, 0, sizeof *i); return; }
	memset(i, 0, sizeof *i); /* (no slot: leaked -- only when the size changes 16 times within a few frames) */
}
static void dead_sweep(void) {
	int k;
	for (k = 0; k < 16; k++) if (dead[k].i.img && cur_frame > dead[k].frame + 16) img_free(&dead[k].i);
}
static int img_get(PImg *i, u32 w, u32 h, VkFormat f) {
	VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	VkMemoryRequirements rq;
	u32 t;
	if (i->img && i->w == w && i->h == h && i->f == f) return 1;
	img_drop(i);
	ci.imageType = VK_IMAGE_TYPE_2D; ci.format = f;
	ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
	ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
	ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (pCreateImage(V.dev, &ci, 0, &i->img) != VK_SUCCESS) { i->img = 0; return 0; }
	pGetImageMemoryRequirements(V.dev, i->img, &rq);
	for (t = 0; t < V.mem->memoryTypeCount; t++)
		if ((rq.memoryTypeBits & (1u << t)) && (V.mem->memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) break;
	if (t == V.mem->memoryTypeCount) { img_free(i); return 0; }
	ai.allocationSize = rq.size; ai.memoryTypeIndex = t;
	if (pAllocateMemory(V.dev, &ai, 0, &i->mem) != VK_SUCCESS) { i->mem = 0; img_free(i); return 0; }
	if (pBindImageMemory(V.dev, i->img, i->mem, 0) != VK_SUCCESS) { img_free(i); return 0; }
	vi.image = i->img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
	vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
	if (pCreateImageView(V.dev, &vi, 0, &i->view) != VK_SUCCESS) { i->view = 0; img_free(i); return 0; }
	i->w = w; i->h = h; i->f = f;
	return 1;
}

/* XRC_POST_BENCH=1: GPU timestamps around each stage's passes, averaged in the log */
static VkQueryPool qp;
static int bench = -1;
static unsigned long long q_used[2][8];
static double b_ms[2]; static u32 b_n[2];

void xrc_post_init(const XrcPostVk *v) { V = *v; }
static int setup(void) {
	VkShaderModuleCreateInfo mi = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	VkDescriptorSetLayoutBinding bd;
	VkDescriptorSetLayoutCreateInfo di = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	VkPushConstantRange pr;
	VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	if (state) return state > 0;
	state = -1;
	if (!V.dev || !V.gdpa || !V.mem) return 0;
#define LD(f) if (!(p##f = (PFN_vk##f)V.gdpa(V.dev, "vk" #f))) { fprintf(stderr, "xrc: vulkan: post: no vk" #f "\n"); return 0; }
	LD(CreateShaderModule) LD(CreateDescriptorSetLayout) LD(CreatePipelineLayout) LD(CreateGraphicsPipelines) LD(CreateSampler)
	LD(CreateImage) LD(GetImageMemoryRequirements) LD(AllocateMemory) LD(BindImageMemory) LD(CreateImageView) LD(DestroyImage)
	LD(DestroyImageView) LD(FreeMemory) LD(CreateQueryPool) LD(GetQueryPoolResults)
#undef LD
	mi.codeSize = sizeof post_post_vert; mi.pCode = post_post_vert;
	if (pCreateShaderModule(V.dev, &mi, 0, &vsm) != VK_SUCCESS) return 0;
	mi.codeSize = sizeof post_post_frag; mi.pCode = post_post_frag;
	if (pCreateShaderModule(V.dev, &mi, 0, &fsm) != VK_SUCCESS) return 0;
	memset(&bd, 0, sizeof bd);
	bd.binding = 0; bd.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; bd.descriptorCount = 1; bd.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	di.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; di.bindingCount = 1; di.pBindings = &bd;
	if (pCreateDescriptorSetLayout(V.dev, &di, 0, &dsl) != VK_SUCCESS) return 0;
	pr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT; pr.offset = 0; pr.size = 64;
	li.setLayoutCount = 1; li.pSetLayouts = &dsl; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pr;
	if (pCreatePipelineLayout(V.dev, &li, 0, &pl) != VK_SUCCESS) return 0;
	si.magFilter = si.minFilter = VK_FILTER_NEAREST; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (pCreateSampler(V.dev, &si, 0, &smp) != VK_SUCCESS) return 0;
	{
		const char *e = getenv("XRC_POST_BENCH");
		bench = e && atoi(e) && V.timestamp && V.reset_q && V.ts_period > 0.0f;
		if (bench) {
			VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
			qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = 2 * 8 * 2;
			if (pCreateQueryPool(V.dev, &qi, 0, &qp) != VK_SUCCESS) bench = 0;
		}
	}
	settings();
	fprintf(stderr, "xrc: vulkan: post ready: downscale %s, sharpen %.2f, deband %d%s\n", set_down ? "Lanczos" : "bilinear", set_cas, set_deband, bench ? " (timed)" : "");
	state = 1;
	return 1;
}
static VkPipeline pipe_for(VkFormat f) {
	VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	VkPipelineShaderStageCreateInfo st[2];
	VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	VkPipelineColorBlendAttachmentState ba;
	VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	int k;
	for (k = 0; k < 8 && pipes[k].p; k++) if (pipes[k].f == f) return pipes[k].p;
	if (k == 8) return VK_NULL_HANDLE;
	memset(st, 0, sizeof st);
	st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vsm; st[0].pName = "main";
	st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fsm; st[1].pName = "main";
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	vp.viewportCount = 1; vp.scissorCount = 1;
	rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	memset(&ba, 0, sizeof ba);
	ba.colorWriteMask = 0xf;
	cb.attachmentCount = 1; cb.pAttachments = &ba;
	ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;
	ri.colorAttachmentCount = 1; ri.pColorAttachmentFormats = &f;
	gi.pNext = &ri;
	gi.stageCount = 2; gi.pStages = st;
	gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs;
	gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &ds;
	gi.layout = pl;
	if (pCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &gi, 0, &pipes[k].p) != VK_SUCCESS) { pipes[k].p = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
	pipes[k].f = f;
	return pipes[k].p;
}

static void bar(VkCommandBuffer cb, VkImage i, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags ss, VkAccessFlags sa,
                VkPipelineStageFlags dst, VkAccessFlags da, const VkImageSubresourceRange *sr) {
	VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	b.srcAccessMask = sa; b.dstAccessMask = da;
	b.oldLayout = from; b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = i;
	if (sr) b.subresourceRange = *sr;
	else { b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1; }
	V.barrier(cb, ss, dst, 0, 0, 0, 0, 0, 1, &b);
}
/* one pass: view (w x h source, SHADER_READ) into t (COLOR_ATTACHMENT) */
static int pass(VkCommandBuffer cb, VkImageView view, PImg *t, const float pc[16]) {
	VkPipeline p = pipe_for(t->f);
	VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
	VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
	VkDescriptorImageInfo ii;
	VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	VkViewport vp;
	VkRect2D sc;
	if (!p) return 0;
	ca.imageView = t->view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	ri.renderArea.extent.width = t->w; ri.renderArea.extent.height = t->h;
	ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
	V.begin(cb, &ri);
	V.bind(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
	ii.sampler = smp; ii.imageView = view; ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	wr.dstBinding = 0; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
	V.push_desc(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &wr);
	V.push_const(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
	vp.x = 0; vp.y = 0; vp.width = (float)t->w; vp.height = (float)t->h; vp.minDepth = 0; vp.maxDepth = 1;
	V.viewport(cb, 0, 1, &vp);
	sc.offset.x = sc.offset.y = 0; sc.extent.width = t->w; sc.extent.height = t->h;
	V.scissor(cb, 0, 1, &sc);
	V.draw(cb, 3, 1, 0, 0);
	V.end(cb);
	return 1;
}

static int fmt_ok(VkFormat f) {
	return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB ||
	       f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f == VK_FORMAT_A2R10G10B10_UNORM_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT;
}
int xrc_post_active(int stage, unsigned sw, unsigned sh, unsigned dw, unsigned dh) {
	int scale = sw != dw || sh != dh;
	settings();
	if (state < 0 || bypass) return 0;
	if (stage == 0) return scale && set_down;
	return (scale && set_down) || set_cas > 0.0f || set_deband > 0;
}

int xrc_post_blit(VkCommandBuffer cb, VkImage simg, VkImageView sview, unsigned sw, unsigned sh, VkImage dimg, VkFormat dfmt,
                  const VkImageBlit *b, int stage, unsigned long long frame) {
	int sx0 = b->srcOffsets[0].x, sy0 = b->srcOffsets[0].y, sx1 = b->srcOffsets[1].x, sy1 = b->srcOffsets[1].y;
	int dx0 = b->dstOffsets[0].x, dy0 = b->dstOffsets[0].y, dx1 = b->dstOffsets[1].x, dy1 = b->dstOffsets[1].y;
	u32 dw, dh, rw, rh;
	int scale, two, m1, slot = 0;
	float pc[16];
	PImg *out, *tmp;
	VkImageCopy ic;
	VkImageSubresourceRange ssr;
	stage = stage ? 1 : 0;
	if (sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0 || sx0 < 0 || sy0 < 0 || dx0 < 0 || dy0 < 0 || (u32)sx1 > sw || (u32)sy1 > sh) return 0;
	if (!sview || !fmt_ok(dfmt) || b->srcSubresource.mipLevel || b->srcSubresource.layerCount != 1) return 0;
	rw = (u32)(sx1 - sx0); rh = (u32)(sy1 - sy0); dw = (u32)(dx1 - dx0); dh = (u32)(dy1 - dy0);
	if (!xrc_post_active(stage, rw, rh, dw, dh) || !setup()) return 0;
	cur_frame = frame;
	dead_sweep();
	scale = rw != dw || rh != dh;
	m1 = stage ? (set_cas > 0.0f ? 2 : 0) | (set_deband >= 2 ? 4 : 0) | (set_deband >= 1 ? 8 : 0) : 0;
	two = scale && (m1 & 6);
	out = &imgs[stage][0]; tmp = &imgs[stage][1];
	if (!img_get(out, dw, dh, dfmt) || (two && !img_get(tmp, dw, dh, VK_FORMAT_R16G16B16A16_SFLOAT))) return 0;
	if (!pipe_for(dfmt) || (two && !pipe_for(VK_FORMAT_R16G16B16A16_SFLOAT))) return 0;
	if (bench) {
		slot = (stage * 8 + (int)(frame % 8)) * 2;
		if (q_used[stage][frame % 8]) { /* (this slot's last use: some frames ago, so done) */
			uint64_t ts[2];
			if (pGetQueryPoolResults(V.dev, qp, (u32)slot, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) {
				b_ms[stage] += (double)(ts[1] - ts[0]) * V.ts_period / 1e6; b_n[stage]++;
				if (b_n[stage] == 300) {
					fprintf(stderr, "xrc: vulkan: post stage %d: %.3f ms GPU per frame (%ux%u -> %ux%u, mode %d%s)\n", stage, b_ms[stage] / 300, rw, rh, dw, dh, m1 | (scale ? 1 : 0), two ? ", two passes" : "");
					b_ms[stage] = 0; b_n[stage] = 0;
				}
			}
		}
		V.reset_q(cb, qp, (u32)slot, 2);
		V.timestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, (u32)slot); /* (when all before is done: the passes' own time) */
		q_used[stage][frame % 8] = frame + 1;
	}
	memset(&ssr, 0, sizeof ssr);
	ssr.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ssr.baseArrayLayer = b->srcSubresource.baseArrayLayer; ssr.levelCount = 1; ssr.layerCount = 1;
	/* the source to be sampled; the target(s) to be drawn (their last use, a copy or a read, before) */
	bar(cb, simg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, &ssr);
	bar(cb, out->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
	    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
	if (two) bar(cb, tmp->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
	             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0);
	memset(pc, 0, sizeof pc);
	pc[0] = (float)sx0; pc[1] = (float)sy0; pc[2] = (float)rw / (float)dw; pc[3] = (float)rh / (float)dh;
	pc[4] = (float)sx0; pc[5] = (float)sy0; pc[6] = (float)sx1; pc[7] = (float)sy1;
	pc[8] = (float)(two ? 1 : (scale ? 1 : 0) | m1); pc[9] = set_cas;
	pc[10] = 2.5f / 255.0f; pc[11] = (float)(dh >= 1440 ? 16.0 * dh / 1440.0 : 12.0);
	pc[12] = (float)(frame % 4096); pc[13] = 1.0f;
	if (!pass(cb, sview, two ? tmp : out, pc)) return 0;
	if (two) {
		bar(cb, tmp->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, 0);
		pc[0] = pc[1] = 0.0f; pc[2] = pc[3] = 1.0f;
		pc[4] = pc[5] = 0.0f; pc[6] = (float)dw; pc[7] = (float)dh;
		pc[8] = (float)m1;
		if (!pass(cb, tmp->view, out, pc)) return 0;
	}
	/* back: the source as the caller left it; the result copied into place */
	bar(cb, simg, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
	    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, &ssr);
	bar(cb, out->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0);
	memset(&ic, 0, sizeof ic);
	ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ic.srcSubresource.layerCount = 1;
	ic.dstSubresource = b->dstSubresource;
	ic.dstOffset.x = dx0; ic.dstOffset.y = dy0;
	ic.extent.width = dw; ic.extent.height = dh; ic.extent.depth = 1;
	V.copy(cb, out->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
	if (bench) V.timestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, (u32)slot + 1);
	{
		static int said[2];
		if (said[stage]++ < 2) fprintf(stderr, "xrc: vulkan: post stage %d: %ux%u -> %ux%u (at %d,%d), mode %d%s\n", stage, rw, rh, dw, dh, dx0, dy0, m1 | (scale ? 1 : 0), two ? ", two passes" : "");
	}
	return 1;
}
