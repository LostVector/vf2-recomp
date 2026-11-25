// Render-target size for the 3D picture and the tile underlay.
//
// The arcade picture is 496x384. VF2_RENDER_SIZE=WxH renders both the polygon
// layer and the tile underlay into a WxH target instead. Because the compositor
// maps the whole target to the 640x480 (rotated) panel area, W=640 H=480 gives
// one rendered pixel per panel pixel; the source art is still 496x384, so the
// 2D gains nothing but the 3D is rasterized at the higher resolution.
#pragma once
#include <cstdio>
#include <cstdlib>

struct vf2_render_size { int w = 496, h = 384; };

inline const vf2_render_size &vf2_render_target_size()
{
    static const vf2_render_size size = [] {
        vf2_render_size s;
        const char *p = std::getenv("VF2_RENDER_SIZE");
        if (!p) return s;
        int w = 0, h = 0;
        if (std::sscanf(p, "%dx%d", &w, &h) != 2 || w < 64 || h < 64 || w > 4096 || h > 4096) {
            std::fprintf(stderr, "VF2_RENDER_SIZE expects WxH (64..4096), ignoring \"%s\"\n", p);
            return s;
        }
        return vf2_render_size{ w, h };
    }();
    return size;
}
