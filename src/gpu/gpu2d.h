// System 24 decoded-map rendering, independent of the host graphics API.
#pragma once
#include <cstdint>
#include <cstddef>
#include <memory>
#include <vector>

struct gpu2d_rect {
    int x0,y0,x1,y1,sx,sy,map,mask,win,priority;
    bool opaque;
    bool line_scroll=false;
};

// The paired-map modes apply a horizontal offset and a map selection per row.
struct gpu2d_scroll_row {
    uint8_t low_x, high_x, left_map, right_map;
};
static_assert(sizeof(gpu2d_scroll_row)==4);
inline gpu2d_scroll_row gpu2d_decode_scroll_row(const uint16_t *tile_ram,unsigned layer,unsigned y)
{
    const uint16_t scroll=tile_ram[0x4000+0x200*layer+y];
    const unsigned x=scroll&511;
    const uint16_t vertical=tile_ram[0x5004+layer];
    unsigned map=layer;
    if((vertical&0x6000)==0x2000){
        const unsigned split=unsigned(-int(vertical))&511;
        map^=unsigned(((-int(vertical))&0x200)==0)^unsigned(y>=split);
        return {uint8_t(x),uint8_t(x>>8),uint8_t(map),uint8_t(map)};
    }
    map^=unsigned((scroll&0x200)==0);
    return {uint8_t(x),uint8_t(x>>8),uint8_t(map),uint8_t(map^1u)};
}
struct gpu2d_frame {
    const uint16_t *pixels[4]={};
    const uint8_t *flags[4]={};
    const uint16_t *tile_ram=nullptr;
    const uint32_t *pens=nullptr;
    const uint16_t *dirty=nullptr;
    size_t dirty_count=0;
    uint64_t version=0;
    std::vector<gpu2d_rect> passes[3]; // backdrop, below 3D, above 3D
};
class gpu2d {
public:
    virtual ~gpu2d()=default;
    virtual bool submit(const gpu2d_frame &frame)=0;
    // Upload/cache frame data without drawing it. paint() then uses only owned
    // GPU data, allowing the board to advance concurrently. Target 0 means
    // the framebuffer already bound; otherwise target is a 496x384 texture.
    virtual bool prepare(const gpu2d_frame &frame)=0;
    // CPU-only capture, followed later by context-thread upload. stage() owns
    // all inputs needed by upload(), so the board may advance between them.
    virtual bool stage(const gpu2d_frame &frame)=0;
    virtual bool upload()=0;
    virtual bool paint(unsigned pass,uintptr_t target,bool flip_y,bool clear)=0;
    // RGBA texture, row zero at v=0 as with the host's CPU layer uploads.
    virtual uintptr_t target_handle(unsigned pass)const=0;
};
std::unique_ptr<gpu2d> gpu2d_create_gles2();
