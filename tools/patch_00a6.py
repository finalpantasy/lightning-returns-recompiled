import os
# integ2: page_00a6 copy with only the shadow-stability hook. The visible-object gather 00a6c5b0 tests each object's AABB against the camera frustum
# (AabbViewTest 00a6ab50, two call sites); xrc_shadow_hold keeps an object that tested visible within the last N frames (off unless set).
s = open(os.path.join(os.environ["LR_GEN"], "src/page_00a6.c")).read()
for ra, site_, key_ in (("0xa6c5e0u", "1", "r_ebx"), ("0xa6c6b0u", "2", "r_edi")):
    k = "    XCALLD(0xa6ab50u, xf_00a6ab50);\n    if (r_esp >= x_s_) M32(x_s_ - 4) = %s; }\n" % ra
    assert s.count(k) == 1, ra
    s = s.replace(k, k + "    r_eax = xrc_shadow_hold(%s, %s, r_eax);\n" % (site_, key_))
s = 'unsigned xrc_shadow_hold(unsigned site, unsigned key, unsigned res);\n' + s
open(os.path.join(os.environ["LR_MATH_SRC"], "page_00a6.c"), "w").write(s)
