/* vk_cs.h -- the render thread of the Vulkan backend (included by vk_d3d9.c, after its device and swap chain globals).
 *
 * XRC_VK_THREAD=1: the command buffer's recording, the submits and the presents run on a thread of their own. The
 * backend itself (the HLE's state, the guest memory read into the upload ring, pipelines, resources) stays on the guest
 * thread that calls it; each vkCmd* / submit call there becomes a packet in a command stream (cs_*), which the render
 * thread replays against the real driver functions (vkr_*), in order. The driver's recording work -- most of a draw's
 * cost below the backend -- and the waits in acquire / present leave the game's main thread.
 *
 * The wrappers below have the Vulkan names, so the backend's calls are unchanged; with the thread off they call the
 * real function at once. Every packet is self-contained (arrays copied, never a pointer into the caller's state); the
 * handles in it stay valid until the frame that used them is done, which the backend already ensures (resources die
 * at their frame's next begin, after its fence) -- once the render thread has issued that frame's submit, which
 * cs_wait_frame waits for before any fence is waited on. Anything that needs the GPU idle (vkDeviceWaitIdle: swap
 * chain rebuilds) drains the stream first; the render thread is then idle, and the guest thread may use the queue.
 *
 * The present: the swap chain image is acquired on the render thread, so the backend records its blit, clear, copy and
 * barriers with placeholders (CS_SWAP_IMG, CS_SWAP_DONE) that the replay replaces with the acquired image and its
 * semaphore. A failed acquire or an out-of-date present only flags the swap chain (swap_ok = 0, rebuilt at the next
 * present); that frame's swap chain commands are dropped. */

static PFN_vkCmdBeginRendering vkr_vkCmdBeginRendering;
static PFN_vkCmdEndRendering vkr_vkCmdEndRendering;
static PFN_vkCmdBindPipeline vkr_vkCmdBindPipeline;
static PFN_vkCmdBindVertexBuffers vkr_vkCmdBindVertexBuffers;
static PFN_vkCmdBindIndexBuffer vkr_vkCmdBindIndexBuffer;
static PFN_vkCmdDraw vkr_vkCmdDraw;
static PFN_vkCmdDrawIndexed vkr_vkCmdDrawIndexed;
static PFN_vkCmdSetViewport vkr_vkCmdSetViewport;
static PFN_vkCmdSetScissor vkr_vkCmdSetScissor;
static PFN_vkCmdSetStencilCompareMask vkr_vkCmdSetStencilCompareMask;
static PFN_vkCmdSetStencilWriteMask vkr_vkCmdSetStencilWriteMask;
static PFN_vkCmdSetStencilReference vkr_vkCmdSetStencilReference;
static PFN_vkCmdSetBlendConstants vkr_vkCmdSetBlendConstants;
static PFN_vkCmdSetDepthBias vkr_vkCmdSetDepthBias;
static PFN_vkCmdPushConstants vkr_vkCmdPushConstants;
static PFN_vkCmdPushDescriptorSetKHR vkr_vkCmdPushDescriptorSetKHR;
static PFN_vkCmdPipelineBarrier vkr_vkCmdPipelineBarrier;
static PFN_vkCmdCopyBuffer vkr_vkCmdCopyBuffer;
static PFN_vkCmdCopyBufferToImage vkr_vkCmdCopyBufferToImage;
static PFN_vkCmdCopyImageToBuffer vkr_vkCmdCopyImageToBuffer;
static PFN_vkCmdCopyImage vkr_vkCmdCopyImage;
static PFN_vkCmdResolveImage vkr_vkCmdResolveImage;
static PFN_vkCmdBlitImage vkr_vkCmdBlitImage;
static PFN_vkCmdClearColorImage vkr_vkCmdClearColorImage;
static PFN_vkCmdClearDepthStencilImage vkr_vkCmdClearDepthStencilImage;
static PFN_vkCmdClearAttachments vkr_vkCmdClearAttachments;
static PFN_vkCmdResetQueryPool vkr_vkCmdResetQueryPool;
static PFN_vkCmdWriteTimestamp vkr_vkCmdWriteTimestamp;
static PFN_vkResetCommandBuffer vkr_vkResetCommandBuffer;
static PFN_vkBeginCommandBuffer vkr_vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer vkr_vkEndCommandBuffer;
static PFN_vkQueueSubmit vkr_vkQueueSubmit;
static PFN_vkDeviceWaitIdle vkr_vkDeviceWaitIdle;
#define CS_FNS(X) \
	X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
	X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetStencilCompareMask) \
	X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetBlendConstants) X(vkCmdSetDepthBias) \
	X(vkCmdPushConstants) X(vkCmdPushDescriptorSetKHR) X(vkCmdPipelineBarrier) X(vkCmdCopyBuffer) \
	X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdResolveImage) X(vkCmdBlitImage) \
	X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdClearAttachments) X(vkCmdResetQueryPool) \
	X(vkCmdWriteTimestamp) X(vkResetCommandBuffer) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) \
	X(vkDeviceWaitIdle)

/* placeholders for the swap chain image being presented and the semaphore its present waits for */
static char cs_swap_img_, cs_swap_done_;
#define CS_SWAP_IMG ((VkImage)(uintptr_t)&cs_swap_img_)
#define CS_SWAP_DONE ((VkSemaphore)(uintptr_t)&cs_swap_done_)

/* ---------------------------------------------------------------------------------------- the stream */
static int cs_on;                       /* the render thread runs */
#define CS_SIZE (32u << 20)
static unsigned char *cs_buf;
static size_t cs_head, cs_tail;         /* (positions; the producer writes head, the render thread tail) */
static size_t cs_wpos, cs_kicked;       /* (producer only) */
static unsigned long long cs_serial;    /* (producer) the last serial handed out */
static unsigned long long cs_done;      /* (render thread) the last serial it has done */
static int cs_sleeping, cs_waiting;
static pthread_mutex_t cs_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cs_cv_work = PTHREAD_COND_INITIALIZER, cs_cv_done = PTHREAD_COND_INITIALIZER;
static u32 rt_img;                      /* (render thread) the acquired swap chain image */
static int rt_acq_ok;                   /* (render thread) this frame's acquire succeeded */
static unsigned long long cs_packets, cs_bytes;

enum {
	CS_WRAP = 1, CS_SYNC, CS_BEGINR, CS_ENDR, CS_PIPE, CS_VB, CS_IB, CS_DRAW, CS_DRAWI, CS_VP, CS_SC, CS_STCMP, CS_STWR,
	CS_STREF, CS_BLEND, CS_DBIAS, CS_PUSHC, CS_PUSHD, CS_BAR, CS_CPB, CS_CPBI, CS_CPIB, CS_CPI, CS_RES, CS_BLIT, CS_CLRC,
	CS_CLRD, CS_CLRA, CS_QRESET, CS_TS, CS_CBRESET, CS_CBBEGIN, CS_CBEND, CS_SUBMIT, CS_ACQUIRE, CS_PRESENT
};
typedef struct { u32 op, size; VkCommandBuffer cb; } CsHdr; /* (size: the whole packet, a multiple of 16) */

static void cs_kick(void) {
	cs_kicked = cs_wpos;
	if (__atomic_load_n(&cs_sleeping, __ATOMIC_SEQ_CST)) {
		pthread_mutex_lock(&cs_mu);
		pthread_cond_signal(&cs_cv_work);
		pthread_mutex_unlock(&cs_mu);
	}
}
/* room for a packet ending at `end` (a full stream: the render thread is far behind -- wait for it) */
static void cs_reserve(size_t end) {
	while (end - __atomic_load_n(&cs_tail, __ATOMIC_ACQUIRE) > CS_SIZE) {
		struct timespec ts = {0, 50000};
		cs_kick();
		nanosleep(&ts, 0);
	}
}
/* a packet of `size` bytes after its header: written by the caller, published by cs_commit */
static void *cs_put(u32 op, VkCommandBuffer c, size_t size) {
	size_t need = (sizeof(CsHdr) + size + 15) & ~(size_t)15, h = cs_wpos, off = h % CS_SIZE;
	CsHdr *p;
	if (off + need > CS_SIZE) { /* (packets never wrap: the rest of the buffer is skipped) */
		size_t skip = CS_SIZE - off;
		cs_reserve(h + skip + need);
		p = (CsHdr *)(cs_buf + off);
		p->op = CS_WRAP; p->size = (u32)skip; p->cb = 0;
		h += skip; off = 0;
	} else cs_reserve(h + need);
	p = (CsHdr *)(cs_buf + off);
	p->op = op; p->size = (u32)need; p->cb = c;
	cs_wpos = h + need;
	return p + 1;
}
static void cs_commit(void) {
	__atomic_store_n(&cs_head, cs_wpos, __ATOMIC_SEQ_CST);
	if (cs_wpos - cs_kicked >= (64u << 10)) cs_kick();
}
/* the guest thread waits until the render thread has done serial s (without the GIL: other guest threads go on) */
static void cs_wait(unsigned long long s) {
	if (!cs_on || __atomic_load_n(&cs_done, __ATOMIC_ACQUIRE) >= s) return;
	cs_kick();
	xrc_gil_release();
	pthread_mutex_lock(&cs_mu);
	cs_waiting = 1;
	while (__atomic_load_n(&cs_done, __ATOMIC_ACQUIRE) < s) pthread_cond_wait(&cs_cv_done, &cs_mu);
	cs_waiting = 0;
	pthread_mutex_unlock(&cs_mu);
	xrc_gil_acquire();
}
/* everything put so far: done by the render thread, which is then idle */
static void cs_drain(void) {
	unsigned long long *s;
	if (!cs_on) return;
	s = (unsigned long long *)cs_put(CS_SYNC, 0, sizeof *s);
	*s = ++cs_serial;
	cs_commit();
	cs_wait(cs_serial);
}

/* ---------------------------------------------------------------------------------------- packets */
typedef struct { VkRenderingInfo ri; VkRenderingAttachmentInfo ca[8], da, sa; } PBeginR;
typedef struct { VkPipelineBindPoint bp; VkPipeline p; } PPipe;
typedef struct { u32 first, n; VkBuffer b[16]; VkDeviceSize o[16]; } PVb;
typedef struct { VkBuffer b; VkDeviceSize o; VkIndexType t; } PIb;
typedef struct { u32 a, b, c; int32_t d; u32 e; } PDraw;
typedef struct { u32 first, n; VkViewport v[4]; } PVp;
typedef struct { u32 first, n; VkRect2D r[4]; } PSc;
typedef struct { VkStencilFaceFlags f; u32 v; } PSt;
typedef struct { float f[4]; } PF4;
typedef struct { VkPipelineLayout l; VkShaderStageFlags st; u32 off, size; unsigned char d[]; } PPushC;
typedef struct { u32 binding, type; VkBuffer buf; VkDeviceSize off, range; VkSampler smp; VkImageView view; VkImageLayout lay; } PDw;
typedef struct { VkPipelineBindPoint bp; VkPipelineLayout l; u32 set, n; PDw w[]; } PPushD;
typedef struct { VkPipelineStageFlags src, dst; VkDependencyFlags dep; u32 nm, nb, ni; } PBar; /* + the 3 arrays */
typedef struct { VkBuffer s, d; u32 n; VkBufferCopy r[]; } PCpB;
typedef struct { VkBuffer b; VkImage i; VkImageLayout l; u32 n; VkBufferImageCopy r[]; } PCpBI;
typedef struct { VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; u32 n; VkFilter f; unsigned char r[]; } PImg2; /* copy / resolve / blit */
typedef struct { VkImage i; VkImageLayout l; VkClearValue v; u32 n; VkImageSubresourceRange r[]; } PClr;
typedef struct { u32 na, nr; VkClearAttachment a[8]; VkClearRect r[]; } PClrA;
typedef struct { VkQueryPool q; u32 a, b; VkPipelineStageFlagBits st; } PQ;
typedef struct { VkFlags flags; } PFl;
typedef struct { VkQueue q; VkFence f; unsigned long long serial; VkCommandBuffer cb; u32 nw, ns; VkSemaphore w; VkPipelineStageFlags ws; VkSemaphore s; } PSubmit;
typedef struct { VkSwapchainKHR sc; VkSemaphore sem; } PAcq;
typedef struct { VkQueue q; VkSwapchainKHR sc; VkSemaphore wait; } PPresent;

#define CS_P(T, op, c, extra) T *p = (T *)cs_put(op, c, sizeof(T) + (extra))
#define CS_GO() cs_commit()
#define CS_ON() (cs_on)

static void vkCmdBeginRendering(VkCommandBuffer c, const VkRenderingInfo *ri) {
	u32 k;
	if (!CS_ON()) { vkr_vkCmdBeginRendering(c, ri); return; }
	CS_P(PBeginR, CS_BEGINR, c, 0);
	p->ri = *ri;
	for (k = 0; k < ri->colorAttachmentCount && k < 8; k++) p->ca[k] = ri->pColorAttachments[k];
	if (ri->pDepthAttachment) p->da = *ri->pDepthAttachment;
	if (ri->pStencilAttachment) p->sa = *ri->pStencilAttachment;
	CS_GO();
}
static void vkCmdEndRendering(VkCommandBuffer c) {
	if (!CS_ON()) { vkr_vkCmdEndRendering(c); return; }
	cs_put(CS_ENDR, c, 0); CS_GO();
}
static void vkCmdBindPipeline(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipeline pl) {
	if (!CS_ON()) { vkr_vkCmdBindPipeline(c, bp, pl); return; }
	{ CS_P(PPipe, CS_PIPE, c, 0); p->bp = bp; p->p = pl; CS_GO(); }
}
static void vkCmdBindVertexBuffers(VkCommandBuffer c, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
	if (!CS_ON() || n > 16) { if (CS_ON()) cs_drain(); vkr_vkCmdBindVertexBuffers(c, first, n, b, o); return; }
	{ CS_P(PVb, CS_VB, c, 0); p->first = first; p->n = n; memcpy(p->b, b, n * sizeof *b); memcpy(p->o, o, n * sizeof *o); CS_GO(); }
}
static void vkCmdBindIndexBuffer(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkIndexType t) {
	if (!CS_ON()) { vkr_vkCmdBindIndexBuffer(c, b, o, t); return; }
	{ CS_P(PIb, CS_IB, c, 0); p->b = b; p->o = o; p->t = t; CS_GO(); }
}
static void vkCmdDraw(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, uint32_t e) {
	if (!CS_ON()) { vkr_vkCmdDraw(c, a, b, d, e); return; }
	{ CS_P(PDraw, CS_DRAW, c, 0); p->a = a; p->b = b; p->c = d; p->d = 0; p->e = e; CS_GO(); }
}
static void vkCmdDrawIndexed(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, int32_t vo, uint32_t e) {
	if (!CS_ON()) { vkr_vkCmdDrawIndexed(c, a, b, d, vo, e); return; }
	{ CS_P(PDraw, CS_DRAWI, c, 0); p->a = a; p->b = b; p->c = d; p->d = vo; p->e = e; CS_GO(); }
}
static void vkCmdSetViewport(VkCommandBuffer c, uint32_t first, uint32_t n, const VkViewport *v) {
	if (!CS_ON() || n > 4) { if (CS_ON()) cs_drain(); vkr_vkCmdSetViewport(c, first, n, v); return; }
	{ CS_P(PVp, CS_VP, c, 0); p->first = first; p->n = n; memcpy(p->v, v, n * sizeof *v); CS_GO(); }
}
static void vkCmdSetScissor(VkCommandBuffer c, uint32_t first, uint32_t n, const VkRect2D *r) {
	if (!CS_ON() || n > 4) { if (CS_ON()) cs_drain(); vkr_vkCmdSetScissor(c, first, n, r); return; }
	{ CS_P(PSc, CS_SC, c, 0); p->first = first; p->n = n; memcpy(p->r, r, n * sizeof *r); CS_GO(); }
}
#define CS_STENCIL(name, op) \
	static void name(VkCommandBuffer c, VkStencilFaceFlags f, uint32_t v) { \
		if (!CS_ON()) { vkr_##name(c, f, v); return; } \
		{ CS_P(PSt, op, c, 0); p->f = f; p->v = v; CS_GO(); } \
	}
CS_STENCIL(vkCmdSetStencilCompareMask, CS_STCMP)
CS_STENCIL(vkCmdSetStencilWriteMask, CS_STWR)
CS_STENCIL(vkCmdSetStencilReference, CS_STREF)
static void vkCmdSetBlendConstants(VkCommandBuffer c, const float bc[4]) {
	if (!CS_ON()) { vkr_vkCmdSetBlendConstants(c, bc); return; }
	{ CS_P(PF4, CS_BLEND, c, 0); memcpy(p->f, bc, 16); CS_GO(); }
}
static void vkCmdSetDepthBias(VkCommandBuffer c, float a, float b, float d) {
	if (!CS_ON()) { vkr_vkCmdSetDepthBias(c, a, b, d); return; }
	{ CS_P(PF4, CS_DBIAS, c, 0); p->f[0] = a; p->f[1] = b; p->f[2] = d; p->f[3] = 0; CS_GO(); }
}
static void vkCmdPushConstants(VkCommandBuffer c, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *d) {
	if (!CS_ON()) { vkr_vkCmdPushConstants(c, l, st, off, size, d); return; }
	{ CS_P(PPushC, CS_PUSHC, c, size); p->l = l; p->st = st; p->off = off; p->size = size; memcpy(p->d, d, size); CS_GO(); }
}
/* (the backend's writes: one buffer or one image per write -- anything else is replayed at once, after a drain) */
static void vkCmdPushDescriptorSetKHR(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set, uint32_t n, const VkWriteDescriptorSet *w) {
	u32 k;
	if (CS_ON()) for (k = 0; k < n; k++) if (w[k].descriptorCount != 1 || w[k].pNext || w[k].dstArrayElement || (!w[k].pBufferInfo && !w[k].pImageInfo)) { cs_drain(); break; }
	if (!CS_ON() || k < n) { vkr_vkCmdPushDescriptorSetKHR(c, bp, l, set, n, w); return; }
	{
		CS_P(PPushD, CS_PUSHD, c, n * sizeof(PDw));
		p->bp = bp; p->l = l; p->set = set; p->n = n;
		for (k = 0; k < n; k++) {
			PDw *d = &p->w[k];
			memset(d, 0, sizeof *d);
			d->binding = w[k].dstBinding; d->type = (u32)w[k].descriptorType;
			if (w[k].pImageInfo) { d->smp = w[k].pImageInfo->sampler; d->view = w[k].pImageInfo->imageView; d->lay = w[k].pImageInfo->imageLayout; }
			else { d->buf = w[k].pBufferInfo->buffer; d->off = w[k].pBufferInfo->offset; d->range = w[k].pBufferInfo->range; }
		}
		CS_GO();
	}
}
static void vkCmdPipelineBarrier(VkCommandBuffer c, VkPipelineStageFlags src, VkPipelineStageFlags dst, VkDependencyFlags dep, uint32_t nm,
                                 const VkMemoryBarrier *m, uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
	unsigned char *q;
	if (!CS_ON()) { vkr_vkCmdPipelineBarrier(c, src, dst, dep, nm, m, nb, b, ni, im); return; }
	{
		CS_P(PBar, CS_BAR, c, nm * sizeof *m + nb * sizeof *b + ni * sizeof *im);
		p->src = src; p->dst = dst; p->dep = dep; p->nm = nm; p->nb = nb; p->ni = ni;
		q = (unsigned char *)(p + 1);
		memcpy(q, m, nm * sizeof *m); q += nm * sizeof *m;
		memcpy(q, b, nb * sizeof *b); q += nb * sizeof *b;
		memcpy(q, im, ni * sizeof *im);
		CS_GO();
	}
}
static void vkCmdCopyBuffer(VkCommandBuffer c, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) {
	if (!CS_ON()) { vkr_vkCmdCopyBuffer(c, s, d, n, r); return; }
	{ CS_P(PCpB, CS_CPB, c, n * sizeof *r); p->s = s; p->d = d; p->n = n; memcpy(p->r, r, n * sizeof *r); CS_GO(); }
}
static void vkCmdCopyBufferToImage(VkCommandBuffer c, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) {
	if (!CS_ON()) { vkr_vkCmdCopyBufferToImage(c, b, i, l, n, r); return; }
	{ CS_P(PCpBI, CS_CPBI, c, n * sizeof *r); p->b = b; p->i = i; p->l = l; p->n = n; memcpy(p->r, r, n * sizeof *r); CS_GO(); }
}
static void vkCmdCopyImageToBuffer(VkCommandBuffer c, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *r) {
	if (!CS_ON()) { vkr_vkCmdCopyImageToBuffer(c, i, l, b, n, r); return; }
	{ CS_P(PCpBI, CS_CPIB, c, n * sizeof *r); p->b = b; p->i = i; p->l = l; p->n = n; memcpy(p->r, r, n * sizeof *r); CS_GO(); }
}
#define CS_IMG2(name, op, T, extra_param, extra_arg, fset) \
	static void name(VkCommandBuffer c, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const T *r extra_param) { \
		if (!CS_ON()) { vkr_##name(c, s, sl, d, dl, n, r extra_arg); return; } \
		{ CS_P(PImg2, op, c, n * sizeof *r); p->s = s; p->sl = sl; p->d = d; p->dl = dl; p->n = n; fset; memcpy(p->r, r, n * sizeof *r); CS_GO(); } \
	}
CS_IMG2(vkCmdCopyImage, CS_CPI, VkImageCopy, , , p->f = VK_FILTER_NEAREST)
CS_IMG2(vkCmdResolveImage, CS_RES, VkImageResolve, , , p->f = VK_FILTER_NEAREST)
CS_IMG2(vkCmdBlitImage, CS_BLIT, VkImageBlit, VK_COMMA_ VkFilter filter, VK_COMMA_ filter, p->f = filter)
static void vkCmdClearColorImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *r) {
	if (!CS_ON()) { vkr_vkCmdClearColorImage(c, i, l, v, n, r); return; }
	{ CS_P(PClr, CS_CLRC, c, n * sizeof *r); p->i = i; p->l = l; p->v.color = *v; p->n = n; memcpy(p->r, r, n * sizeof *r); CS_GO(); }
}
static void vkCmdClearDepthStencilImage(VkCommandBuffer c, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *r) {
	if (!CS_ON()) { vkr_vkCmdClearDepthStencilImage(c, i, l, v, n, r); return; }
	{ CS_P(PClr, CS_CLRD, c, n * sizeof *r); p->i = i; p->l = l; p->v.depthStencil = *v; p->n = n; memcpy(p->r, r, n * sizeof *r); CS_GO(); }
}
static void vkCmdClearAttachments(VkCommandBuffer c, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *r) {
	if (!CS_ON() || na > 8) { if (CS_ON()) cs_drain(); vkr_vkCmdClearAttachments(c, na, a, nr, r); return; }
	{ CS_P(PClrA, CS_CLRA, c, nr * sizeof *r); p->na = na; p->nr = nr; memcpy(p->a, a, na * sizeof *a); memcpy(p->r, r, nr * sizeof *r); CS_GO(); }
}
static __attribute__((unused)) void vkCmdResetQueryPool(VkCommandBuffer c, VkQueryPool q, uint32_t a, uint32_t b) {
	if (!CS_ON()) { vkr_vkCmdResetQueryPool(c, q, a, b); return; }
	{ CS_P(PQ, CS_QRESET, c, 0); p->q = q; p->a = a; p->b = b; CS_GO(); }
}
static __attribute__((unused)) void vkCmdWriteTimestamp(VkCommandBuffer c, VkPipelineStageFlagBits st, VkQueryPool q, uint32_t a) {
	if (!CS_ON()) { vkr_vkCmdWriteTimestamp(c, st, q, a); return; }
	{ CS_P(PQ, CS_TS, c, 0); p->q = q; p->a = a; p->st = st; CS_GO(); }
}
static VkResult vkResetCommandBuffer(VkCommandBuffer c, VkCommandBufferResetFlags f) {
	if (!CS_ON()) return vkr_vkResetCommandBuffer(c, f);
	{ CS_P(PFl, CS_CBRESET, c, 0); p->flags = f; CS_GO(); }
	return VK_SUCCESS;
}
static VkResult vkBeginCommandBuffer(VkCommandBuffer c, const VkCommandBufferBeginInfo *bi) {
	if (!CS_ON() || bi->pNext || bi->pInheritanceInfo) { if (CS_ON()) cs_drain(); return vkr_vkBeginCommandBuffer(c, bi); }
	{ CS_P(PFl, CS_CBBEGIN, c, 0); p->flags = bi->flags; CS_GO(); }
	return VK_SUCCESS;
}
static VkResult vkEndCommandBuffer(VkCommandBuffer c) {
	if (!CS_ON()) return vkr_vkEndCommandBuffer(c);
	cs_put(CS_CBEND, c, 0); CS_GO();
	return VK_SUCCESS;
}
/* (the backend's submits: one command buffer, at most one wait and one signal semaphore) */
static VkResult vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence f) {
	if (CS_ON() && (n != 1 || si->commandBufferCount != 1 || si->waitSemaphoreCount > 1 || si->signalSemaphoreCount > 1 || si->pNext)) cs_drain();
	else if (CS_ON()) {
		CS_P(PSubmit, CS_SUBMIT, 0, 0);
		p->q = q; p->f = f; p->cb = si->pCommandBuffers[0];
		p->nw = si->waitSemaphoreCount; p->w = p->nw ? si->pWaitSemaphores[0] : VK_NULL_HANDLE; p->ws = p->nw ? si->pWaitDstStageMask[0] : 0;
		p->ns = si->signalSemaphoreCount; p->s = p->ns ? si->pSignalSemaphores[0] : VK_NULL_HANDLE;
		p->serial = ++cs_serial;
		CS_GO();
		cs_kick();
		return VK_SUCCESS;
	}
	return vkr_vkQueueSubmit(q, n, si, f);
}
static VkResult vkDeviceWaitIdle(VkDevice d) {
	cs_drain();
	return vkr_vkDeviceWaitIdle(d);
}
/* the present, on the render thread: acquire (before the frame's swap chain commands), present (after its submit) */
static void cs_acquire(VkSwapchainKHR sc, VkSemaphore sem) {
	CS_P(PAcq, CS_ACQUIRE, 0, 0); p->sc = sc; p->sem = sem; CS_GO();
}
static void cs_present(VkQueue q, VkSwapchainKHR sc) {
	CS_P(PPresent, CS_PRESENT, 0, 0); p->q = q; p->sc = sc; p->wait = CS_SWAP_DONE; CS_GO(); cs_kick();
}

/* ---------------------------------------------------------------------------------------- the render thread */
static double rt_present_ms, rt_busy_ms; /* (statistics, read racily by the logs) */
#define RIMG(i) ((i) == CS_SWAP_IMG ? swap_img[rt_img] : (i))
#define SWAP_SKIP(i) ((i) == CS_SWAP_IMG && !rt_acq_ok)
static void cs_exec(CsHdr *h) {
	VkCommandBuffer c = h->cb;
	void *a = h + 1;
	switch (h->op) {
	case CS_SYNC: {
		pthread_mutex_lock(&cs_mu);
		__atomic_store_n(&cs_done, *(unsigned long long *)a, __ATOMIC_RELEASE);
		if (cs_waiting) pthread_cond_broadcast(&cs_cv_done);
		pthread_mutex_unlock(&cs_mu);
		break;
	}
	case CS_BEGINR: {
		PBeginR *p = (PBeginR *)a;
		p->ri.pColorAttachments = p->ca;
		if (p->ri.pDepthAttachment) p->ri.pDepthAttachment = &p->da;
		if (p->ri.pStencilAttachment) p->ri.pStencilAttachment = &p->sa;
		vkr_vkCmdBeginRendering(c, &p->ri);
		break;
	}
	case CS_ENDR: vkr_vkCmdEndRendering(c); break;
	case CS_PIPE: { PPipe *p = (PPipe *)a; vkr_vkCmdBindPipeline(c, p->bp, p->p); break; }
	case CS_VB: { PVb *p = (PVb *)a; vkr_vkCmdBindVertexBuffers(c, p->first, p->n, p->b, p->o); break; }
	case CS_IB: { PIb *p = (PIb *)a; vkr_vkCmdBindIndexBuffer(c, p->b, p->o, p->t); break; }
	case CS_DRAW: { PDraw *p = (PDraw *)a; vkr_vkCmdDraw(c, p->a, p->b, p->c, p->e); break; }
	case CS_DRAWI: { PDraw *p = (PDraw *)a; vkr_vkCmdDrawIndexed(c, p->a, p->b, p->c, p->d, p->e); break; }
	case CS_VP: { PVp *p = (PVp *)a; vkr_vkCmdSetViewport(c, p->first, p->n, p->v); break; }
	case CS_SC: { PSc *p = (PSc *)a; vkr_vkCmdSetScissor(c, p->first, p->n, p->r); break; }
	case CS_STCMP: { PSt *p = (PSt *)a; vkr_vkCmdSetStencilCompareMask(c, p->f, p->v); break; }
	case CS_STWR: { PSt *p = (PSt *)a; vkr_vkCmdSetStencilWriteMask(c, p->f, p->v); break; }
	case CS_STREF: { PSt *p = (PSt *)a; vkr_vkCmdSetStencilReference(c, p->f, p->v); break; }
	case CS_BLEND: { PF4 *p = (PF4 *)a; vkr_vkCmdSetBlendConstants(c, p->f); break; }
	case CS_DBIAS: { PF4 *p = (PF4 *)a; vkr_vkCmdSetDepthBias(c, p->f[0], p->f[1], p->f[2]); break; }
	case CS_PUSHC: { PPushC *p = (PPushC *)a; vkr_vkCmdPushConstants(c, p->l, p->st, p->off, p->size, p->d); break; }
	case CS_PUSHD: {
		PPushD *p = (PPushD *)a;
		VkWriteDescriptorSet w[32];
		VkDescriptorBufferInfo bi[32];
		VkDescriptorImageInfo ii[32];
		u32 k, n = p->n > 32 ? 32 : p->n;
		for (k = 0; k < n; k++) {
			PDw *d = &p->w[k];
			memset(&w[k], 0, sizeof w[k]);
			w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			w[k].dstBinding = d->binding;
			w[k].descriptorCount = 1;
			w[k].descriptorType = (VkDescriptorType)d->type;
			if (d->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || d->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
				bi[k].buffer = d->buf; bi[k].offset = d->off; bi[k].range = d->range;
				w[k].pBufferInfo = &bi[k];
			} else {
				ii[k].sampler = d->smp; ii[k].imageView = d->view; ii[k].imageLayout = d->lay;
				w[k].pImageInfo = &ii[k];
			}
		}
		vkr_vkCmdPushDescriptorSetKHR(c, p->bp, p->l, p->set, n, w);
		break;
	}
	case CS_BAR: {
		PBar *p = (PBar *)a;
		VkMemoryBarrier *m = (VkMemoryBarrier *)(p + 1);
		VkBufferMemoryBarrier *b = (VkBufferMemoryBarrier *)(m + p->nm);
		VkImageMemoryBarrier *im = (VkImageMemoryBarrier *)(b + p->nb);
		u32 k, ni = 0;
		for (k = 0; k < p->ni; k++) { /* (the swap chain image's: its handle, or dropped when it was not acquired) */
			if (SWAP_SKIP(im[k].image)) continue;
			im[ni] = im[k];
			im[ni].image = RIMG(im[k].image);
			ni++;
		}
		if (p->nm || p->nb || ni) vkr_vkCmdPipelineBarrier(c, p->src, p->dst, p->dep, p->nm, m, p->nb, b, ni, im);
		break;
	}
	case CS_CPB: { PCpB *p = (PCpB *)a; vkr_vkCmdCopyBuffer(c, p->s, p->d, p->n, p->r); break; }
	case CS_CPBI: { PCpBI *p = (PCpBI *)a; if (!SWAP_SKIP(p->i)) vkr_vkCmdCopyBufferToImage(c, p->b, RIMG(p->i), p->l, p->n, p->r); break; }
	case CS_CPIB: { PCpBI *p = (PCpBI *)a; vkr_vkCmdCopyImageToBuffer(c, p->i, p->l, p->b, p->n, p->r); break; }
	case CS_CPI: { PImg2 *p = (PImg2 *)a; if (!SWAP_SKIP(p->d)) vkr_vkCmdCopyImage(c, p->s, p->sl, RIMG(p->d), p->dl, p->n, (const VkImageCopy *)p->r); break; }
	case CS_RES: { PImg2 *p = (PImg2 *)a; vkr_vkCmdResolveImage(c, p->s, p->sl, p->d, p->dl, p->n, (const VkImageResolve *)p->r); break; }
	case CS_BLIT: { PImg2 *p = (PImg2 *)a; if (!SWAP_SKIP(p->d)) vkr_vkCmdBlitImage(c, p->s, p->sl, RIMG(p->d), p->dl, p->n, (const VkImageBlit *)p->r, p->f); break; }
	case CS_CLRC: { PClr *p = (PClr *)a; if (!SWAP_SKIP(p->i)) vkr_vkCmdClearColorImage(c, RIMG(p->i), p->l, &p->v.color, p->n, p->r); break; }
	case CS_CLRD: { PClr *p = (PClr *)a; vkr_vkCmdClearDepthStencilImage(c, p->i, p->l, &p->v.depthStencil, p->n, p->r); break; }
	case CS_CLRA: { PClrA *p = (PClrA *)a; vkr_vkCmdClearAttachments(c, p->na, p->a, p->nr, p->r); break; }
	case CS_QRESET: { PQ *p = (PQ *)a; vkr_vkCmdResetQueryPool(c, p->q, p->a, p->b); break; }
	case CS_TS: { PQ *p = (PQ *)a; vkr_vkCmdWriteTimestamp(c, p->st, p->q, p->a); break; }
	case CS_CBRESET: { PFl *p = (PFl *)a; VKC(vkr_vkResetCommandBuffer(c, p->flags)); break; }
	case CS_CBBEGIN: {
		PFl *p = (PFl *)a;
		VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = p->flags;
		VKC(vkr_vkBeginCommandBuffer(c, &bi));
		break;
	}
	case CS_CBEND: VKC(vkr_vkEndCommandBuffer(c)); break;
	case CS_SUBMIT: {
		PSubmit *p = (PSubmit *)a;
		VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
		VkSemaphore sig = p->s == CS_SWAP_DONE ? swap_done[rt_img] : p->s;
		int swap = p->s == CS_SWAP_DONE; /* (the present's submit: waits for the acquire) */
		si.commandBufferCount = 1; si.pCommandBuffers = &p->cb;
		if (p->nw && (!swap || rt_acq_ok)) { si.waitSemaphoreCount = 1; si.pWaitSemaphores = &p->w; si.pWaitDstStageMask = &p->ws; }
		if (p->ns && (!swap || rt_acq_ok)) { si.signalSemaphoreCount = 1; si.pSignalSemaphores = &sig; }
		VKC(vkr_vkQueueSubmit(p->q, 1, &si, p->f));
		pthread_mutex_lock(&cs_mu);
		__atomic_store_n(&cs_done, p->serial, __ATOMIC_RELEASE);
		if (cs_waiting) pthread_cond_broadcast(&cs_cv_done);
		pthread_mutex_unlock(&cs_mu);
		break;
	}
	case CS_ACQUIRE: {
		PAcq *p = (PAcq *)a;
		double t0 = now_ms();
		VkResult r = vkAcquireNextImageKHR(vd, p->sc, ~0ull, p->sem, VK_NULL_HANDLE, &rt_img);
		rt_present_ms += now_ms() - t0;
		rt_acq_ok = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
		if (!rt_acq_ok) __atomic_store_n(&swap_ok, 0, __ATOMIC_RELEASE);
		break;
	}
	case CS_PRESENT: {
		PPresent *p = (PPresent *)a;
		if (rt_acq_ok) {
			VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
			VkSemaphore w = swap_done[rt_img];
			double t0 = now_ms();
			VkResult r;
			pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &w;
			pi.swapchainCount = 1; pi.pSwapchains = &p->sc; pi.pImageIndices = &rt_img;
			r = vkQueuePresentKHR(p->q, &pi);
			rt_present_ms += now_ms() - t0;
			if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) __atomic_store_n(&swap_ok, 0, __ATOMIC_RELEASE);
		}
		rt_acq_ok = 0;
		break;
	}
	default: break;
	}
}
static void *cs_thread(void *arg) {
	size_t t = 0;
	(void)arg;
#if !defined(_WIN32)
	pthread_setname_np(pthread_self(), "xrc-render");
#endif
	for (;;) {
		size_t h = __atomic_load_n(&cs_head, __ATOMIC_SEQ_CST);
		if (t == h) {
			int spin;
			for (spin = 0; spin < 2000 && t == h; spin++) {
#if defined(__x86_64__) || defined(__i386__)
				__builtin_ia32_pause();
#endif
				h = __atomic_load_n(&cs_head, __ATOMIC_SEQ_CST);
			}
			if (t == h) {
				pthread_mutex_lock(&cs_mu);
				__atomic_store_n(&cs_sleeping, 1, __ATOMIC_SEQ_CST);
				while ((h = __atomic_load_n(&cs_head, __ATOMIC_SEQ_CST)) == t) pthread_cond_wait(&cs_cv_work, &cs_mu);
				__atomic_store_n(&cs_sleeping, 0, __ATOMIC_SEQ_CST);
				pthread_mutex_unlock(&cs_mu);
			}
		}
		{
			double t0 = now_ms();
			while (t != h) {
				CsHdr *p = (CsHdr *)(cs_buf + t % CS_SIZE);
				if (p->op != CS_WRAP) cs_exec(p);
				t += p->size;
				__atomic_store_n(&cs_tail, t, __ATOMIC_RELEASE);
			}
			rt_busy_ms += now_ms() - t0;
		}
	}
	return 0;
}
/* XRC_VK_THREAD=1: start it (after the backend's setup: the stream takes over the command buffer being recorded) */
static void cs_start(void) {
	pthread_t th;
	const char *e = getenv("XRC_VK_THREAD");
	if (!e || !atoi(e)) return;
	cs_buf = (unsigned char *)malloc(CS_SIZE);
	if (!cs_buf) return;
	if (pthread_create(&th, 0, cs_thread, 0) != 0) { free(cs_buf); cs_buf = 0; return; }
	cs_on = 1;
	LOG("xrc: vulkan: the render thread is on (XRC_VK_THREAD)\n");
}
