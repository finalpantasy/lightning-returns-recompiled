/* the runtime's texture key (vk_d3d9.c tex_hash), for the offline extractor */
#include <string.h>
#include <stddef.h>
__declspec(dllexport) unsigned long long tex_hash(const unsigned char *p, size_t n, unsigned long long seed) {
	unsigned long long a = seed ^ 0x9E3779B97F4A7C15ull, b = seed + n, c = ~seed, d = seed * 0xff51afd7ed558ccdull, w;
	size_t i = 0;
	for (; i + 32 <= n; i += 32) {
		memcpy(&w, p + i, 8); a = (a ^ w) * 0xff51afd7ed558ccdull; a ^= a >> 31;
		memcpy(&w, p + i + 8, 8); b = (b ^ w) * 0xc4ceb9fe1a85ec53ull; b ^= b >> 29;
		memcpy(&w, p + i + 16, 8); c = (c ^ w) * 0xff51afd7ed558ccdull; c ^= c >> 33;
		memcpy(&w, p + i + 24, 8); d = (d ^ w) * 0xc4ceb9fe1a85ec53ull; d ^= d >> 30;
	}
	for (; i < n; i++) a = (a ^ p[i]) * 0x100000001b3ull;
	a ^= b * 0x9E3779B97F4A7C15ull; a ^= a >> 32; a ^= c * 0xff51afd7ed558ccdull; a ^= a >> 29; a ^= d * 0xc4ceb9fe1a85ec53ull;
	a ^= a >> 33; a *= 0xff51afd7ed558ccdull; a ^= a >> 33;
	return a;
}
