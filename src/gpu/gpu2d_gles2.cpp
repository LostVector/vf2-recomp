// Cached indexed System 24 maps. Only changed 8x8 cells cross the bus.
#include "gpu2d.h"
#include "render_size.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace {
GLuint compile(GLenum kind,const char *source){
    GLuint s=glCreateShader(kind);glShaderSource(s,1,&source,nullptr);glCompileShader(s);
    GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[2048];glGetShaderInfoLog(s,sizeof(log),nullptr,log);std::fprintf(stderr,"GPU2D shader: %s\n",log);glDeleteShader(s);return 0;}return s;
}
class renderer final:public gpu2d {
    GLuint program=0,fbo=0,vbo=0,maps[8]={},palette=0,mask=0,targets[3]={};
    const bool quadrant_maps=std::getenv("VF2_QUADRANT_TILE_TEXTURES")!=nullptr;
    const bool rotate_maps=quadrant_maps&&std::getenv("VF2_ROTATE_TILE_TEXTURES");
    unsigned active_bank=0,staged_bank=0;
    bool bank_ready[2]={};
    std::vector<uint16_t> previous_changes,staged_changes,combined_changes;
    uint64_t map_upload_bytes=0,map_upload_calls=0,map_bank_switches=0;
    const bool paired_maps=quadrant_maps||std::getenv("VF2_PAIRED_TILE_TEXTURES");
    unsigned map_textures()const{return quadrant_maps?4:paired_maps?2:1;}
    const bool trace_map_uploads=std::getenv("VF2_TRACE_MAP_UPLOADS")!=nullptr;
    GLint flip_loc=-1;
    const bool gpu_line_scroll=std::getenv("VF2_GPU_LINE_SCROLL")!=nullptr;
    GLuint scroll_texture=0;
    unsigned scroll_pending=0;
    std::array<std::array<gpu2d_scroll_row,384>,2> scroll_rows{};
    uint64_t scroll_draws=0;
    std::array<unsigned,3> starts{},counts{};
    std::vector<uint8_t> staging,mask_pixels;
    std::vector<uint16_t> packed_staging;
    const bool packed_map=std::getenv("VF2_PACKED_TILE_MAP")!=nullptr;
    GLenum map_type()const{return packed_map?GL_UNSIGNED_SHORT_4_4_4_4:GL_UNSIGNED_BYTE;}
    struct map_upload {unsigned x,y,w,h;size_t offset;};
    std::vector<map_upload> map_uploads;
    bool palette_pending=false,mask_pending=false,staged=false;
    uint64_t staged_version=~0ull;
    std::array<uint32_t,4096> last_pens{};
    std::array<uint16_t,3072> last_mask{};
    uint64_t version=~0ull;
    struct vertex {float x,y,u,v,mx,my,opaque,priority,masked,win;};
    std::vector<vertex> vertices;
    const bool clip_empty_tiles=std::getenv("VF2_CLIP_EMPTY_TILES")!=nullptr;
    // A bit per decoded 8x8 cell that contains a pixel of the requested priority.
    uint64_t occupied[4][2][64]={};
    void update_coverage(const gpu2d_frame &f,unsigned map,unsigned x,unsigned y,unsigned w,unsigned h){
        constexpr uint64_t low_bits=0x0101010101010101ull;
        constexpr uint64_t coverage_bits=low_bits<<4;
        for(unsigned ty=y/8;ty<(y+h)/8;++ty){
            for(unsigned tx=x/8;tx<(x+w)/8;++tx){
                uint64_t priority0=0,priority1=0;
                for(unsigned row=0;row<8;++row){
                    uint64_t flags;
                    std::memcpy(&flags,f.flags[map]+(ty*8+row)*512+tx*8,sizeof(flags));
                    priority0|=flags&~(flags<<4)&coverage_bits;
                    priority1|=flags&low_bits;
                }
                const uint64_t bit=uint64_t(1)<<tx;
                auto &zero=occupied[map][0][ty], &one=occupied[map][1][ty];
                zero=priority0?zero|bit:zero&~bit;
                one=priority1?one|bit:one&~bit;
            }
        }
    }
    void emit_rect(const gpu2d_rect &r,int left,int top,int right,int bottom){
        for(int corner:{0,1,2,2,1,3}){
            const int x=(corner&1)?right:left,y=(corner&2)?bottom:top;
            const int dx=x-r.x0,dy=y-r.y0;
            vertices.push_back({float(x),float(y),float((r.map&1)*512+r.sx+dx)/1024.f,float((r.map>>1)*512+r.sy+dy)/1024.f,
                float(x)/512.f,float(std::max(0,r.mask)*384+y)/768.f,float(r.opaque),float(0x10|r.priority),float(r.mask>=0),float(r.win)});
        }
    }
    void emit_line_scroll(const gpu2d_rect &r,const uint16_t *tile_ram){
        const unsigned slot=unsigned(r.map)>>1;
        if(!(scroll_pending&(1u<<slot))){
            for(unsigned y=0;y<384;++y)
                scroll_rows[slot][y]=gpu2d_decode_scroll_row(tile_ram,unsigned(r.map),y);
            scroll_pending|=1u<<slot;
        }
        for(int corner:{0,1,2,2,1,3}){
            const int x=(corner&1)?r.x1:r.x0,y=(corner&2)?r.y1:r.y0;
            vertices.push_back({float(x),float(y),float(x)/1024.f,float(y)/1024.f,
                                0.f,float(y+r.sy)/1024.f,float(r.opaque),float(0x10|r.priority),
                                2.f,float(slot)});
        }
        ++scroll_draws;
    }
    void emit_covered_rect(const gpu2d_rect &r){
        const int end_x=r.sx+r.x1-r.x0,end_y=r.sy+r.y1-r.y0;
        if(r.opaque||r.sx<0||r.sy<0||end_x>512||end_y>512||r.priority<0||r.priority>1){
            emit_rect(r,r.x0,r.y0,r.x1,r.y1);
            return;
        }
        const auto &rows=occupied[r.map][r.priority];
        const uint64_t x_mask=(~uint64_t(0)<<(r.sx/8))&(~uint64_t(0)>>(63-(end_x-1)/8));
        // Include one source texel of margin for varying interpolation, and
        // retain map edges where an adjacent atlas map can be sampled.
        constexpr int guard=1;
        int band_left=0,band_right=0,band_top=r.sy;
        for(int y=r.sy;y<end_y;){
            const int next=std::min(end_y,(y|7)+1);
            const int tile_y=y/8;
            const int above=std::max(0,(r.map>>1)*64+tile_y-1);
            const int below=std::min(127,(r.map>>1)*64+tile_y+1);
            const auto padded_rows=[&](int column){
                const int upper=(above/64)*2+column,lower=(below/64)*2+column;
                return rows[tile_y]|occupied[upper][r.priority][above%64]|occupied[lower][r.priority][below%64];
            };
            const uint64_t inner=padded_rows(r.map&1);
            uint64_t bits=inner|(inner<<1)|(inner>>1);
            if(r.map&1){
                const uint64_t outer=occupied[r.map-1][r.priority][tile_y]|
                    occupied[(above/64)*2][r.priority][above%64]|occupied[(below/64)*2][r.priority][below%64];
                bits|=outer>>63;
            }else{
                const uint64_t outer=occupied[r.map+1][r.priority][tile_y]|
                    occupied[(above/64)*2+1][r.priority][above%64]|occupied[(below/64)*2+1][r.priority][below%64];
                bits|=(outer&1)<<63;
            }
            bits&=x_mask;
            int left=0,right=0;
            if(bits){
                left=std::max(r.sx,int(std::countr_zero(bits))*8-guard);
                right=std::min(end_x,512-int(std::countl_zero(bits))*8+guard);
            }
            if(left!=band_left||right!=band_right){
                if(band_left<band_right)
                    emit_rect(r,r.x0+band_left-r.sx,r.y0+band_top-r.sy,r.x0+band_right-r.sx,r.y0+y-r.sy);
                band_left=left;
                band_right=right;
                band_top=y;
            }
            y=next;
        }
        if(band_left<band_right)
            emit_rect(r,r.x0+band_left-r.sx,r.y0+band_top-r.sy,r.x0+band_right-r.sx,r.y1);
    }
    static void params(){
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    size_t encode_packed(const gpu2d_frame &f,unsigned map,unsigned x,unsigned y,unsigned w,unsigned h){
        // A decoded S24 pen is 12 bits. Store its high, middle and low
        // nibbles in RGB, with transparent, priority zero and priority one in A.
        const size_t offset=packed_staging.size();
        packed_staging.resize(offset+size_t(w)*h);
        for(unsigned row=0;row<h;++row){
            const size_t from=size_t(y+row)*512+x;
            const auto *pens=f.pixels[map]+from;
            const auto *flags=f.flags[map]+from;
            auto *target=packed_staging.data()+offset+size_t(row)*w;
            unsigned i=0;
#if defined(__ARM_NEON)
            const auto priority0=vdup_n_u8(0x10),priority1=vdup_n_u8(0x11);
            const auto one=vdup_n_u8(1),two=vdup_n_u8(2);
            for(;i+8<=w;i+=8){
                const auto src_flags=vld1_u8(flags+i);
                const auto alpha=vorr_u8(vand_u8(vceq_u8(src_flags,priority0),one),
                                         vand_u8(vceq_u8(src_flags,priority1),two));
                const auto rgb=vshlq_n_u16(vld1q_u16(pens+i),4);
                vst1q_u16(target+i,vorrq_u16(rgb,vmovl_u8(alpha)));
            }
#endif
            for(;i<w;++i){
                const unsigned alpha=flags[i]==0x10?1:flags[i]==0x11?2:0;
                target[i]=uint16_t(pens[i]<<4)|alpha;
            }
        }
        return offset;
    }
    size_t encode_rgba(const gpu2d_frame &f,unsigned map,unsigned x,unsigned y,unsigned w,unsigned h){
        const size_t offset=staging.size();staging.resize(offset+size_t(w)*h*4);
        for(unsigned v=0;v<h;v++){
            const size_t from=size_t(y+v)*512+x,to=offset+size_t(v)*w*4;
            const uint16_t *pens=f.pixels[map]+from;
            const uint8_t *flags=f.flags[map]+from;
            uint8_t *out=staging.data()+to;
            unsigned u=0;
#if defined(__ARM_NEON)
            // Pack eight indices and flags into the same RGBA byte layout.
            const auto alpha=vdup_n_u8(255);
            for(;u+8<=w;u+=8){
                const auto values=vld1q_u16(pens+u);
                uint8x8x4_t rgba;
                rgba.val[0]=vmovn_u16(values);
                rgba.val[1]=vshrn_n_u16(values,8);
                rgba.val[2]=vld1_u8(flags+u);
                rgba.val[3]=alpha;
                vst4_u8(out+4*u,rgba);
            }
#endif
            for(;u<w;u++){
                const unsigned pen=pens[u];
                out[u*4]=pen;out[u*4+1]=pen>>8;out[u*4+2]=flags[u];out[u*4+3]=255;
            }
        }
        return offset;
    }
    void encode(const gpu2d_frame &f,unsigned map,unsigned x,unsigned y,unsigned w,unsigned h){
        const size_t offset=packed_map?encode_packed(f,map,x,y,w,h):encode_rgba(f,map,x,y,w,h);
        if(clip_empty_tiles)update_coverage(f,map,x,y,w,h);
        map_uploads.push_back({(map&1)*512+x,(map>>1)*512+y,w,h,offset});
    }
    // Use one bounded rectangle per map for large updates. When the dirty
    // set is small, merge adjacent tiles without uploading clean cells.
    void encode_dirty_regions(const gpu2d_frame &f,std::span<const uint16_t> dirty){
        if(dirty.size()>128){
            struct bounds { unsigned x0=64,y0=64,x1=0,y1=0; };
            bounds maps[4];
            for(const unsigned cell:dirty){
                const unsigned x=cell&63,y=(cell>>6)&63;
                auto &b=maps[cell>>12];
                b.x0=std::min(b.x0,x);b.x1=std::max(b.x1,x+1);
                b.y0=std::min(b.y0,y);b.y1=std::max(b.y1,y+1);
            }
            for(unsigned map=0;map<4;++map){
                const auto &b=maps[map];
                if(b.x0<b.x1)encode(f,map,b.x0*8,b.y0*8,(b.x1-b.x0)*8,(b.y1-b.y0)*8);
            }
            return;
        }
        uint64_t cells[4][64]{};
        uint64_t rows[4]{};
        for(const unsigned cell:dirty){
            const unsigned map=cell>>12,y=(cell>>6)&63;
            cells[map][y]|=uint64_t(1)<<(cell&63);
            rows[map]|=uint64_t(1)<<y;
        }
        for(unsigned map=0;map<4;++map){
            while(rows[map]){
                const unsigned y=std::countr_zero(rows[map]);
                auto &row=cells[map][y];
                while(row){
                    const unsigned x=std::countr_zero(row);
                    const unsigned width=std::countr_one(row>>x);
                    const uint64_t mask=(~uint64_t(0)>>(64-width))<<x;
                    unsigned bottom=y;
                    do{
                        cells[map][bottom]&=~mask;
                        if(!cells[map][bottom])rows[map]&=~(uint64_t(1)<<bottom);
                        ++bottom;
                    }while(bottom<64&&(cells[map][bottom]&mask)==mask);
                    encode(f,map,x*8,y*8,width*8,(bottom-y)*8);
                }
            }
        }
    }
public:
    ~renderer()override{
        if(trace_map_uploads)
            std::fprintf(stderr,"Tile map totals: %llu bytes, %llu calls, %llu bank switches\n",
                (unsigned long long)map_upload_bytes,(unsigned long long)map_upload_calls,
                (unsigned long long)map_bank_switches);
        if(gpu_line_scroll&&std::getenv("VF2_GLES_STATS"))
            std::fprintf(stderr,"GPU2D paired scroll rects: %llu\n",(unsigned long long)scroll_draws);
        if(scroll_texture)glDeleteTextures(1,&scroll_texture);
        glDeleteTextures(3,targets);glDeleteTextures(map_textures()*(rotate_maps?2:1),maps);glDeleteTextures(1,&palette);glDeleteTextures(1,&mask);
        glDeleteFramebuffers(1,&fbo);glDeleteBuffers(1,&vbo);glDeleteProgram(program);
    }
    bool init(){
        const char *vs=R"(
attribute vec2 aPosition,aMap,aMask;
attribute vec4 aParams;
uniform float uFlipY;
varying mediump vec2 vMap,vMask;
varying mediump vec4 vParams;
// Store row zero at texture v=0, matching the CPU layer uploads. This also
// makes nearest-neighbour choices at exact upscale boundaries agree.
void main(){vec2 p=aPosition*vec2(2.0/496.0,2.0/384.0)-vec2(1.0);p.y*=uFlipY;gl_Position=vec4(p,0.0,1.0);vMap=aMap;vMask=aMask;vParams=aParams;}
)";
        const char *fs=R"(
precision mediump float;
uniform sampler2D uMap,uPalette,uMask;
#ifdef PAIRED_TILE_TEXTURES
uniform sampler2D uSecondMap;
#endif
#ifdef QUADRANT_TILE_TEXTURES
uniform sampler2D uThirdMap,uFourthMap;
#endif
#ifdef GPU_LINE_SCROLL
uniform sampler2D uLineScroll;
#endif
varying mediump vec2 vMap,vMask;
varying mediump vec4 vParams;
void main(){
    vec2 source=vMap;
#ifdef GPU_LINE_SCROLL
    if(vParams.z>1.5){
        vec2 pixel=floor(vMap*1024.0);
        vec4 info=floor(texture2D(uLineScroll,vec2((pixel.y+0.5)/512.0,(vParams.w+0.5)/2.0))*255.0+0.5);
        float offset=info.r+info.g*256.0;
        float map=pixel.x<offset?info.b:info.a;
        vec2 texel=vec2(mod(pixel.x-offset+512.0,512.0),mod(floor(vMask.y*1024.0),512.0));
        source=(vec2(mod(map,2.0),floor(map/2.0))*512.0+texel+0.5)/1024.0;
    }else
#endif
    if(vParams.z>0.5&&abs(texture2D(uMask,vMask).r-vParams.w)>0.5)discard;
#ifdef QUADRANT_TILE_TEXTURES
    vec4 data;
    vec2 local=source*2.0;
    if(source.y<0.5){
        if(source.x<0.5) data=texture2D(uMap,local);
        else data=texture2D(uSecondMap,local-vec2(1.0,0.0));
    }else{
        if(source.x<0.5) data=texture2D(uThirdMap,local-vec2(0.0,1.0));
        else data=texture2D(uFourthMap,local-vec2(1.0,1.0));
    }
#elif defined(PAIRED_TILE_TEXTURES)
    // Choose from the original atlas coordinate, including samples rounding
    // across the seam between pairs. Both maps in a scroll pair stay together.
    vec4 data;
    if(source.y<0.5)
        data=texture2D(uMap,vec2(source.x,source.y*2.0));
    else
        data=texture2D(uSecondMap,vec2(source.x,source.y*2.0-1.0));
#else
    vec4 data=texture2D(uMap,source);
#endif
#ifdef PACKED_TILE_MAP
    vec4 nibble=floor(data*15.0+0.5);
    if(vParams.x<0.5&&abs(nibble.a-(vParams.y-15.0))>0.5)discard;
    vec2 pen=vec2(nibble.b+16.0*nibble.g,nibble.r);
#else
    if(vParams.x<0.5&&abs(data.b*255.0-vParams.y)>0.5)discard;
    vec2 pen=floor(data.rg*255.0+0.5);
#endif
    gl_FragColor=texture2D(uPalette,(pen+0.5)/vec2(256.0,16.0)).bgra;
}
)";
        const auto fragment=std::string(packed_map?"#define PACKED_TILE_MAP\n":"")+
                            (gpu_line_scroll?"#define GPU_LINE_SCROLL\n":"")+
                            (paired_maps?"#define PAIRED_TILE_TEXTURES\n":"")+
                            (quadrant_maps?"#define QUADRANT_TILE_TEXTURES\n":"")+fs;
        GLuint a=compile(GL_VERTEX_SHADER,vs),b=compile(GL_FRAGMENT_SHADER,fragment.c_str());if(!a||!b)return false;
        program=glCreateProgram();glAttachShader(program,a);glAttachShader(program,b);
        const char *names[]={"aPosition","aMap","aMask","aParams"};for(unsigned i=0;i<4;i++)glBindAttribLocation(program,i,names[i]);
        glLinkProgram(program);glDeleteShader(a);glDeleteShader(b);GLint ok;glGetProgramiv(program,GL_LINK_STATUS,&ok);if(!ok)return false;
        glUseProgram(program);glUniform1i(glGetUniformLocation(program,"uMap"),0);glUniform1i(glGetUniformLocation(program,"uPalette"),1);glUniform1i(glGetUniformLocation(program,"uMask"),2);
        flip_loc=glGetUniformLocation(program,"uFlipY");
        glGenFramebuffers(1,&fbo);glGenBuffers(1,&vbo);glGenTextures(map_textures()*(rotate_maps?2:1),maps);glGenTextures(1,&palette);glGenTextures(1,&mask);
        if(paired_maps)glUniform1i(glGetUniformLocation(program,"uSecondMap"),4);
        if(quadrant_maps){
            glUniform1i(glGetUniformLocation(program,"uThirdMap"),5);
            glUniform1i(glGetUniformLocation(program,"uFourthMap"),6);
        }
        glActiveTexture(GL_TEXTURE0);
        for(unsigned pair=0;pair<map_textures()*(rotate_maps?2u:1u);++pair){
            glBindTexture(GL_TEXTURE_2D,maps[pair]);params();
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,quadrant_maps?512:1024,paired_maps?512:1024,0,GL_RGBA,map_type(),nullptr);
        }
        glBindTexture(GL_TEXTURE_2D,palette);params();glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,256,16,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glBindTexture(GL_TEXTURE_2D,mask);params();glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE,64,768,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,nullptr);
        if(gpu_line_scroll){
            glUniform1i(glGetUniformLocation(program,"uLineScroll"),3);
            glGenTextures(1,&scroll_texture);
            glBindTexture(GL_TEXTURE_2D,scroll_texture);params();
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,512,2,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        }
        return glGetError()==GL_NO_ERROR;
    }
    uintptr_t target_handle(unsigned pass)const override{return pass<3?targets[pass]:0;}
    bool stage(const gpu2d_frame &f)override{
        // A rejected frame must leave the pending uploads and residency intact.
        for(const auto &pass:f.passes)for(const auto &r:pass){
            if(r.line_scroll&&r.x1>r.x0&&r.y1>r.y0&&
               (!gpu_line_scroll||r.map<0||r.map>2||(r.map&1)))return false;
        }
        const bool first=version==~0ull||staged,full=first||f.version!=version+1;
        staging.clear();packed_staging.clear();map_uploads.clear();
        staged_bank=active_bank;
        if(rotate_maps&&full)bank_ready[0]=bank_ready[1]=false;
        if(rotate_maps&&(full||f.dirty_count)){
            staged_bank=active_bank^1;
            staged_changes.clear();
            if(f.dirty_count)staged_changes.assign(f.dirty,f.dirty+f.dirty_count);
        }
        if(full||(staged_bank!=active_bank&&!bank_ready[staged_bank])){
            for(unsigned map=0;map<4;++map)encode(f,map,0,0,512,512);
        }else if(f.dirty_count){
            std::span<const uint16_t> dirty{f.dirty,f.dirty_count};
            if(rotate_maps&&!previous_changes.empty()){
                // The inactive bank missed the last mutation, even if several
                // unchanged frames have since reused the current bank.
                combined_changes.assign(dirty.begin(),dirty.end());
                combined_changes.insert(combined_changes.end(),previous_changes.begin(),previous_changes.end());
                std::sort(combined_changes.begin(),combined_changes.end());
                combined_changes.erase(std::unique(combined_changes.begin(),combined_changes.end()),combined_changes.end());
                dirty=std::span<const uint16_t>{combined_changes};
            }
            encode_dirty_regions(f,dirty);
        }
        palette_pending=first||std::memcmp(last_pens.data(),f.pens,sizeof(last_pens));
        if(palette_pending)std::memcpy(last_pens.data(),f.pens,sizeof(last_pens));
        bool mask_changed=first;
        for(unsigned pair=0;pair<2;pair++){
            const uint16_t *source=f.tile_ram+0x6000+pair*0x800;
            auto *cached=last_mask.data()+pair*1536;
            if(std::memcmp(cached,source,1536*sizeof(uint16_t))){std::memcpy(cached,source,1536*sizeof(uint16_t));mask_changed=true;}
        }
        if(mask_changed){
            mask_pixels.resize(64*768);
            for(unsigned pair=0;pair<2;pair++)for(unsigned y=0;y<384;y++)for(unsigned x=0;x<64;x++)
                mask_pixels[(pair*384+y)*64+x]=(last_mask[pair*1536+y*4+x/16]&(0x8000>>(x&15)))?255:0;
        }
        mask_pending=mask_changed;
        scroll_pending=0;
        vertices.clear();
        for(unsigned pass=0;pass<3;pass++){
            starts[pass]=vertices.size();
            for(const auto &r:f.passes[pass]){
                if(r.x1<=r.x0||r.y1<=r.y0)continue;
                if(r.line_scroll){
                    emit_line_scroll(r,f.tile_ram);
                }else if(clip_empty_tiles)emit_covered_rect(r);
                else emit_rect(r,r.x0,r.y0,r.x1,r.y1);
            }
            counts[pass]=vertices.size()-starts[pass];
        }
        staged_version=f.version;staged=true;return true;
    }
    bool upload()override{
        if(!staged)return false;
        if(scroll_pending){
            glActiveTexture(GL_TEXTURE3);glBindTexture(GL_TEXTURE_2D,scroll_texture);
            for(unsigned slot=0;slot<scroll_rows.size();++slot)
                if(scroll_pending&(1u<<slot))
                    glTexSubImage2D(GL_TEXTURE_2D,0,0,slot,384,1,GL_RGBA,GL_UNSIGNED_BYTE,scroll_rows[slot].data());
        }
        glActiveTexture(GL_TEXTURE0);
        const unsigned bank_offset=staged_bank*map_textures();
        unsigned bound=0;
        glBindTexture(GL_TEXTURE_2D,maps[bank_offset]);
        for(const auto &u:map_uploads){
            const unsigned base_x=quadrant_maps?(u.x&512):0, base_y=paired_maps?(u.y&512):0;
            const unsigned texture=quadrant_maps?(base_y/256+base_x/512):base_y/512;
            if(texture!=bound){glBindTexture(GL_TEXTURE_2D,maps[bank_offset+texture]);bound=texture;}
            const void *pixels=packed_map?static_cast<const void *>(packed_staging.data()+u.offset):
                                            staging.data()+u.offset;
            using clock=std::chrono::steady_clock;
            const auto started=trace_map_uploads?clock::now():clock::time_point{};
            glTexSubImage2D(GL_TEXTURE_2D,0,u.x-base_x,u.y-base_y,u.w,u.h,GL_RGBA,map_type(),pixels);
            if(trace_map_uploads){
                map_upload_bytes+=uint64_t(u.w)*u.h*(packed_map?2:4);
                ++map_upload_calls;
                const double ms=std::chrono::duration<double,std::milli>(clock::now()-started).count();
                if(ms>=8)std::fprintf(stderr,"Slow map upload: version %llu x %u y %u size %ux%u %.3f ms\n",
                    (unsigned long long)staged_version,u.x,u.y,u.w,u.h,ms);
            }
        }
        if(palette_pending){
            glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,palette);
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,256,16,GL_RGBA,GL_UNSIGNED_BYTE,last_pens.data());
        }
        if(mask_pending){
            glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,mask);
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,64,768,GL_LUMINANCE,GL_UNSIGNED_BYTE,mask_pixels.data());
        }
        glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,vertices.size()*sizeof(vertex),vertices.data(),GL_STREAM_DRAW);glBindBuffer(GL_ARRAY_BUFFER,0);
        const bool ok=glGetError()==GL_NO_ERROR;
        if(ok&&staged_bank!=active_bank){
            bank_ready[staged_bank]=true;
            active_bank=staged_bank;
            previous_changes.swap(staged_changes);
            if(trace_map_uploads)++map_bank_switches;
        }
        version=ok?staged_version:~0ull;
        staged=false;
        return ok;
    }
    bool prepare(const gpu2d_frame &f)override{return stage(f)&&upload();}
    bool paint(unsigned pass,uintptr_t target,bool flip_y,bool clear,bool clear_depth)override{
        if(pass>=3||version==~0ull||staged)return false;
        GLint prior=0;
        if(target){
            glGetIntegerv(GL_FRAMEBUFFER_BINDING,&prior);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,GLuint(target),0);
            if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){glBindFramebuffer(GL_FRAMEBUFFER,prior);return false;}
        }
        glViewport(0,0,vf2_render_target_size().w,vf2_render_target_size().h);glDisable(GL_DEPTH_TEST);glDisable(GL_SCISSOR_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glDisable(GL_DITHER);
        if(clear){
            uint32_t pen=pass==0?last_pens[0]:0;
            glClearColor(((pen>>16)&255)/255.f,((pen>>8)&255)/255.f,(pen&255)/255.f,(pen>>24)/255.f);
            glClear(GL_COLOR_BUFFER_BIT|(clear_depth?GL_DEPTH_BUFFER_BIT:0));
        }
        glUseProgram(program);glUniform1f(flip_loc,flip_y?-1.f:1.f);
        if(gpu_line_scroll){glActiveTexture(GL_TEXTURE3);glBindTexture(GL_TEXTURE_2D,scroll_texture);}
        const unsigned bank_offset=active_bank*map_textures();
        for(unsigned index=1;index<map_textures();++index){
            glActiveTexture(GL_TEXTURE3+index);glBindTexture(GL_TEXTURE_2D,maps[bank_offset+index]);
        }
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,maps[bank_offset]);glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,palette);glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,mask);
        glBindBuffer(GL_ARRAY_BUFFER,vbo);
        const unsigned sizes[]={2,2,2,4},offsets[]={0,2,4,6};
        for(unsigned i=0;i<4;i++){glEnableVertexAttribArray(i);glVertexAttribPointer(i,sizes[i],GL_FLOAT,GL_FALSE,sizeof(vertex),reinterpret_cast<void*>(uintptr_t(offsets[i]*sizeof(float))));}
        if(counts[pass])glDrawArrays(GL_TRIANGLES,starts[pass],counts[pass]);
        for(unsigned i=0;i<4;i++)glDisableVertexAttribArray(i);glBindBuffer(GL_ARRAY_BUFFER,0);
        if(target)glBindFramebuffer(GL_FRAMEBUFFER,prior);return glGetError()==GL_NO_ERROR;
    }
    bool submit(const gpu2d_frame &f)override{
        if(!prepare(f))return false;
        if(!targets[0]){
            glGenTextures(3,targets);glActiveTexture(GL_TEXTURE0);
            for(auto target:targets){glBindTexture(GL_TEXTURE_2D,target);params();glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,vf2_render_target_size().w,vf2_render_target_size().h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);}
        }
        for(unsigned i=0;i<3;i++)if(!paint(i,targets[i],false,true,false))return false;
        return true;
    }
};
}
std::unique_ptr<gpu2d> gpu2d_create_gles2(){auto r=std::make_unique<renderer>();if(!r->init())return {};return r;}
