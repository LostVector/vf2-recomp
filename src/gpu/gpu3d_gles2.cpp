// GLES2 has no compute shaders, integer textures or shader bit operations.
// Unpack Model 2's texture sheets once per RAM generation on the CPU, then
// preserve its indexed filtering/luma/colour tables in a GLSL ES 1.00 shader.
#include "gpu3d.h"
#include "geometry_frame.h"
#include "gles2_tiles.h"
#include "gles2_tile_shaders.h"
#include "gles2_atlas.h"
#include "../machine/state_io.h"
#include "gles2_batch_shaders.h"
#include "palette_shades.h"
#include "../machine/parallel.h"
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <bit>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
const bool wide_palette_atlas=std::getenv("VF2_WIDE_PALETTE_ATLAS")!=nullptr;
const bool indexed_texture_cells=std::getenv("VF2_CELL_INDEX_TRANSFORMS")!=nullptr;
const bool retained_cell_bounds=std::getenv("VF2_RETAINED_FAN_CELL_BOUNDS")!=nullptr;
const bool origin_packet_fan=std::getenv("VF2_ORIGIN_PACKET_FAN")!=nullptr;
const bool limited_mip_scan=std::getenv("VF2_LIMIT_MIP_SCAN")!=nullptr;
template<class Polygon>
int max_texture_mip(const Polygon &p){
    if(limited_mip_scan)return vf2_packet_math::texture_mip_limit(p.texw,p.texh);
    return std::max(0,int(std::log2(std::min(p.texw,p.texh)))-1);
}
const char *vertex_source=R"(
attribute vec2 aPosition;
attribute vec3 aQ;
uniform vec2 uSize;
uniform vec2 uScale;
uniform highp float uSortDepth;
uniform VF2_PREC vec4 uTexture;
uniform highp vec2 uUVBase;
varying VF2_VARY_PREC vec3 vQ;
void main() {
    vec2 xy=aPosition*uScale/uSize;
#if VF2_LOW_PREC
    // Use the fixed-function perspective interpolator, rather than dividing
    // two already rounded mediump varyings in the fragment shader.
    float depth=1.0/max(aQ.x,0.000000000001);
    gl_Position=vec4(vec2(xy.x*2.0-1.0,1.0-xy.y*2.0)*depth,uSortDepth*depth,depth);
    vQ=vec3(aQ.yz/(aQ.x*max(uTexture.xy,vec2(1.0)))-uUVBase,depth);
#else
    gl_Position=vec4(xy.x*2.0-1.0,1.0-xy.y*2.0,uSortDepth,1.0);
    vQ=aQ;
#endif
}
)";
const char *fragment_source=R"(
precision VF2_PREC float;
varying VF2_VARY_PREC vec3 vQ;
uniform sampler2D uSheet00,uSheet01,uSheet10,uSheet11,uPalette;
uniform VF2_PREC vec4 uTexture; // width, height, atlas x, atlas y
uniform vec4 uWrap; // wrap x/y, mirror x/y
uniform vec4 uMicro; // enabled, min LOD, atlas x/y
uniform vec4 uColour,uOwner;
uniform vec2 uPaletteOffset;
uniform float uTextured,uTransparent,uChecker,uSheet,uLOD,uMaxLevel,uOwnerMode,uHeight;
float trunczero(float x) { return sign(x)*floor(abs(x)); }
float texel(float sheet,vec2 at) {
    vec2 pixel=mod(at,vec2(2048.0,1024.0));
    float right=step(1024.0,pixel.x);
    vec2 uv=(vec2(mod(pixel.x,1024.0),pixel.y)+0.5)/1024.0;
    float index;
    if (sheet<0.5) {
        if (right<0.5) index=texture2D(uSheet00,uv).r;
        else index=texture2D(uSheet01,uv).r;
    } else {
        if (right<0.5) index=texture2D(uSheet10,uv).r;
        else index=texture2D(uSheet11,uv).r;
    }
    return floor(index*255.0+0.5);
}
vec2 sample_index(vec2 fixedUV,float level) {
    vec2 size,origin;
    float sheet;
#if VF2_LOW_PREC
    // Keep phase in [0,2), instead of forming 16.8 fixed point coordinates
    // that can overflow a Mali-400 mediump register. Normalized projective
    // coordinates are prepared by the vertex shader.
    vec2 phase=mod(fixedUV,2.0);
    if (level<0.0) {
        size=vec2(128.0);origin=uMicro.zw;sheet=1.0-uSheet;
        phase=mod(phase*(uTexture.xy/128.0),2.0);
        phase=mod(phase*exp2(exp2(uMicro.y)),2.0);
    } else {
        float divisor=exp2(level);
        size=uTexture.xy/divisor;
        origin=mod(floor((uTexture.zw-vec2(2048.0,1024.0))/divisor),vec2(2048.0,1024.0));
        sheet=mod(uSheet+level,2.0);
    }
    if (uWrap.z>0.5 && phase.x>=1.0) phase.x=1.0-fract(phase.x)-(1.0/256.0)/size.x;
    if (uWrap.w>0.5 && phase.y>=1.0) phase.y=1.0-fract(phase.y)-(1.0/256.0)/size.y;
    vec2 pixel=fract(phase)*size-0.5;
    vec2 a=mod(floor(pixel),size),b=mod(a+1.0,size),f=fract(pixel);
#else
    if (level<0.0) {
        size=vec2(128.0); origin=uMicro.zw; sheet=1.0-uSheet;
        fixedUV*=exp2(exp2(uMicro.y));
    } else {
        float divisor=exp2(level);
        size=uTexture.xy/divisor;
        origin=mod(floor((uTexture.zw-vec2(2048.0,1024.0))/divisor),vec2(2048.0,1024.0));
        sheet=mod(uSheet+level,2.0);
        fixedUV=floor(fixedUV/divisor);
    }
    if (uWrap.z>0.5 && mod(floor(fixedUV.x/(size.x*256.0)),2.0)>0.5) fixedUV.x=-1.0-fixedUV.x;
    if (uWrap.w>0.5 && mod(floor(fixedUV.y/(size.y*256.0)),2.0)>0.5) fixedUV.y=-1.0-fixedUV.y;
    fixedUV-=128.0;
    vec2 a=mod(floor(fixedUV/256.0),size),b=mod(a+1.0,size),f=mod(fixedUV,256.0)/256.0;
#endif
    if (uWrap.x<0.5 && b.x==0.0) {
        if (f.x>=0.5) {a.x=b.x;b.x+=1.0;f.x=0.0;}
        else {b.x=a.x;a.x-=1.0;f.x=1.0;}
    }
    if (uWrap.y<0.5 && b.y==0.0) {
        if (f.y>=0.5) {a.y=0.0;b.y+=1.0;f.y=0.0;}
        else {b.y=a.y;a.y-=1.0;f.y=1.0;}
    }
    vec2 aa=vec2(texel(sheet,origin+a),128.0);
    vec2 ba=vec2(texel(sheet,origin+vec2(b.x,a.y)),128.0);
    vec2 ab=vec2(texel(sheet,origin+vec2(a.x,b.y)),128.0);
    vec2 bb=vec2(texel(sheet,origin+b),128.0);
    if (uTransparent>0.5) {
        if (aa.x==240.0) aa.y=0.0;
        if (ba.x==240.0) ba.y=0.0;
        if (ab.x==240.0) ab.y=0.0;
        if (bb.x==240.0) bb.y=0.0;
        if (aa.x==240.0) aa.x=ba.x;
        if (ba.x==240.0) ba.x=aa.x;
        if (ab.x==240.0) ab.x=bb.x;
        if (bb.x==240.0) bb.x=ab.x;
    }
    vec2 top=floor(mix(aa,ba,f.x)),bottom=floor(mix(ab,bb,f.x));
    if (uTransparent>0.5) {
        if (top.x==240.0 && top.y==0.0) top.x=bottom.x;
        if (bottom.x==240.0 && bottom.y==0.0) bottom.x=top.x;
    }
    return floor(mix(top,bottom,f.y));
}
void main() {
    if (uChecker>0.5 && mod(floor(gl_FragCoord.x)+uHeight-1.0-floor(gl_FragCoord.y),2.0)<0.5) discard;
    vec4 colour=uColour;
    if (uTextured>0.5) {
#if VF2_LOW_PREC
        float lod=log2(max(vQ.z,0.000001))*2.0-uLOD/128.0;
        float level=clamp(floor(lod),0.0,uMaxLevel);
        vec2 uv=vQ.xy;
        vec2 tx=sample_index(uv,level);
        if (lod>0.0 && level<uMaxLevel)
            tx=floor(mix(tx,sample_index(uv,level+1.0),fract(lod)));
        else if (uMicro.x>0.5 && lod<0.0)
            tx=floor(mix(tx,sample_index(uv,-1.0),min(-lod/(2.0*exp2(uMicro.y)),127.0/256.0)));
#else
        float z=1.0/vQ.x;
        float mml=floor(log2(max(z,0.0000001))*256.0)-uLOD;
        float level=clamp(floor(mml/128.0),0.0,uMaxLevel);
        vec2 uv=vec2(trunczero(vQ.y*z*256.0),trunczero(vQ.z*z*256.0));
        vec2 tx=sample_index(uv,level);
        if (mml>0.0 && level<uMaxLevel)
            tx=floor(mix(tx,sample_index(uv,level+1.0),mod(mml,128.0)/128.0));
        else if (uMicro.x>0.5 && mml<0.0)
            tx=floor(mix(tx,sample_index(uv,-1.0),min(floor(-mml/exp2(uMicro.y)),127.0)/256.0));
#endif
        if (uTransparent>0.5 && tx.y<64.0) discard;
        colour=texture2D(uPalette,vec2((tx.x+0.5)/VF2_PALETTE_X_RANGE+uPaletteOffset.x,uPaletteOffset.y));
    }
    gl_FragColor=uOwnerMode>0.5?uOwner:colour;
}
)";
GLuint shader(GLenum type,const char *body,bool highp,bool low_coordinates,bool mesh=false,unsigned batch_mode=0,bool compact_meta=false) {
    GLuint s=glCreateShader(type);
    std::string prefix=highp?"#define VF2_PREC highp\n":"#define VF2_PREC mediump\n";
    prefix+=low_coordinates?"#define VF2_LOW_PREC 1\n":"#define VF2_LOW_PREC 0\n";
    prefix+=std::getenv("VF2_FLAT_FRAGMENT_PROBE")?"#define VF2_FLAT_FRAGMENT_PROBE 1\n":"#define VF2_FLAT_FRAGMENT_PROBE 0\n";
    if(std::getenv("VF2_GLES_INTERPOLATOR_HIGHP"))prefix+="#define VF2_VARY_PREC highp\n";
    else prefix+=std::getenv("VF2_GLES_MEDIUMP_VARYINGS")?"#define VF2_VARY_PREC mediump\n":"#define VF2_VARY_PREC VF2_PREC\n";
    prefix+="#define VF2_BATCH_MODE "+std::to_string(batch_mode)+"\n";
    if(compact_meta)prefix+="#define VF2_PROBE_COMPACT_META 1\n";
    if(mesh)prefix+="#define VF2_MESH_VERTEX 1\n";
    prefix+=wide_palette_atlas?"#define VF2_PALETTE_X_RANGE 1024.0\n":"#define VF2_PALETTE_X_RANGE 512.0\n";
    const char *sources[]={prefix.c_str(),body};
    glShaderSource(s,2,sources,nullptr);glCompileShader(s);
    GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
    if (!ok) {char log[4096];glGetShaderInfoLog(s,sizeof(log),nullptr,log);std::fprintf(stderr,"GLES2 shader: %s\n",log);glDeleteShader(s);return 0;}
    return s;
}
struct vertex {float x,y,z,u,v;};
struct material_hash {
    size_t operator()(uint64_t key)const noexcept {
        // std::hash<uint64_t> truncates on ARMv7, losing palette luma and
        // texture sheet/wrap bits. Mix both halves to avoid long hash chains.
        return uint32_t(key)^(uint32_t(key>>32)*0x9e3779b9u);
    }
};
#define VF2_GLES_UNIFORMS(X) \
    X(uSize) X(uScale) X(uHeight) X(uOwnerMode) X(uSheet00) X(uSheet01) X(uSheet10) X(uSheet11) \
    X(uPalette) X(uPaletteOffset) X(uTextured) X(uTransparent) X(uChecker) X(uColour) X(uOwner) X(uTexture) X(uWrap) X(uSortDepth) \
    X(uMicro) X(uSheet) X(uLOD) X(uMaxLevel) X(uUVBase) X(uTile0) X(uTile1) X(uBand) X(uBlendMode) X(uMicroMinLOD)
class gles2 final:public gpu3d {
    GLuint program=0,fbo=0,depth_buffer=0,vbo=0,tile_vbo=0,palette_atlas=0,palette_spare=0,sheets[4]={};
    PFNGLDISCARDFRAMEBUFFEREXTPROC discard_framebuffer=nullptr;
    PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC attach_msaa = nullptr;
    PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC depth_msaa = nullptr;
    const bool edge_aa = std::getenv("VF2_GLES_EDGE_AA") != nullptr;
    bool edge_target_verified = false;
    static constexpr GLsizei edge_samples = 4;

    void attach_color(int target) {
        if (edge_aa)
            attach_msaa(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets[target], 0,
                        edge_samples);
        else
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets[target], 0);
    }

    bool verify_edge_target() {
        if (!edge_aa || edge_target_verified)
            return true;
        GLint samples = 0;
        const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_SAMPLES_EXT, &samples);
        if (status != GL_FRAMEBUFFER_COMPLETE || samples != edge_samples) {
            std::fprintf(stderr, "VF2 edge AA framebuffer unavailable (status %x, %d samples)\n",
                         status, samples);
            return false;
        }
        edge_target_verified = true;
        std::fprintf(stderr, "VF2 GLES edge AA: %dx MSAA, %dx%d\n", samples, width, height);
        return true;
    }
    bool direct_tiles=false;
    bool use_batches=false,indexed_batches=false,frame_indexed=false,index_overflow=false,stack_packets=false;
    GLuint tile_ibo=0;
    const bool client_arrays=std::getenv("VF2_CLIENT_ARRAYS")!=nullptr;
    const bool mesh_render=std::getenv("VF2_MESH_RENDER")!=nullptr;
    const bool compact_meta=std::getenv("DAYTONA_COMPACT40")&&!mesh_render&&
        !std::getenv("VF2_DRAW_STREAM_HASH")&&!std::getenv("VF2_CHECK_DIRECT_PACK");
    const bool specialize_batches=std::getenv("VF2_HOMOGENEOUS_ATLAS_SHADERS")&&!mesh_render&&
        !std::getenv("VF2_FLAT_FRAGMENT_PROBE");
    GLint mesh_rows_location=-1;
    std::vector<std::array<float,64*12>> mesh_rows;
    std::vector<std::array<float,2>> mesh_centers;
    std::vector<uint8_t> mesh_ready;
    uint64_t mesh_drawn_polygons=0;

    vf2_batch::tile_atlas batch_atlas;
    struct batch_material {
        float depth;
        uint8_t info0[4];int16_t info1[4];uint16_t palette[2];uint8_t colour[4];
    };
    struct batch_vertex {
        float position_q[3],uv0[2],uv1[2];
        batch_material material;
    };
    static_assert(sizeof(batch_material)==24 && sizeof(batch_vertex)==52);
    static_assert(offsetof(batch_vertex,material)==28);
    struct compact_material {
        float depth;
        int16_t texlod;
        uint16_t palette_flags;
        uint8_t mip;
        uint8_t colour[3];
    };
    struct compact_vertex {
        float position_q[3],uv0[2],uv1[2];
        compact_material material;
    };
    static_assert(sizeof(compact_vertex)==40 && offsetof(compact_vertex,material)==28);
    static bool can_compact(const batch_material &m){
        return m.info0[0]<=1&&m.info0[1]<=1&&m.info0[2]<=1&&!m.info0[3]&&
            m.palette[0]<=768&&m.palette[0]%256==0&&m.palette[1]<1024&&
            m.info1[1]>=-1&&m.info1[1]<=14&&m.info1[2]>=0&&m.info1[2]<=2&&
            m.info1[3]>=0&&m.info1[3]<=3&&m.colour[3]==255;
    }
    static compact_material compact(const batch_material &m){
        const unsigned flags=m.info0[0]+2*m.info0[1]+4*m.info0[2];
        return {m.depth,m.info1[0],uint16_t(m.palette[0]*4+m.palette[1]+4096*flags),
            uint8_t(m.info1[1]+1+16*m.info1[2]+64*m.info1[3]),
            {m.colour[0],m.colour[1],m.colour[2]}};
    }
    std::vector<batch_vertex> batch_vertices;
    std::vector<compact_vertex> compact_vertices;
    bool compact_ready=false;
    size_t compact_frames=0,wide_fallback_frames=0;
    const bool test_compact_fallback=std::getenv("DAYTONA_COMPACT40_TEST_FALLBACK")!=nullptr;
    std::vector<uint16_t> batch_indices;
    // The vectors keep their high-water capacity so growth never re-zeroes
    // elements the pack will overwrite; these track the live span each frame.
    size_t batch_vertex_count=0,batch_index_count=0;
    struct batch_bucket {std::vector<batch_vertex> vertices;std::vector<uint32_t> indices;};
    std::unordered_map<uint64_t,batch_bucket,vf2_batch::tile_hash> batch_buckets;
    struct direct_bucket {size_t vertices=0,indices=0,first_vertex=0,first_index=0;unsigned features=0;};
    std::unordered_map<uint64_t,direct_bucket,vf2_batch::tile_hash> direct_buckets;
    struct draw_work {
        direct_bucket *bucket=nullptr;
        size_t first_vertex=0,first_index=0,n=0,ni=0;
        const vf2_tiles::vertex *vertices=nullptr;
        const vf2_tiles::ordinary_vertex *ordinary=nullptr;
        const vf2_tiles::retained_fan *fan=nullptr;
        const uint32_t *indices=nullptr;
        batch_material material{};
        size_t map_slot=0;
        unsigned texw=0,texh=0;
        bool power_of_two=false;

        void set_packet(const vf2_tiles::indexed_packet &packet){
            fan=nullptr;vertices=packet.vertices.data();n=packet.vertex_count();
            ordinary=packet.ordinary.empty()?nullptr:packet.ordinary.data();
            indices=packet.indices.data();ni=packet.indices.size();
        }
        void set_packet(const vf2_tiles::retained_fan &source){
            fan=&source;vertices=nullptr;ordinary=nullptr;n=source.n;
            indices=vf2_tiles::fan_indices;ni=source.index_count();
        }
    };
    struct direct_work : draw_work {
        uint64_t bucket_key=0;
        const gpu3d_mesh_polygon *mesh=nullptr;
        std::vector<vf2_tiles::vertex> flat;
        std::vector<uint32_t> flat_indices;
    };
    std::vector<direct_work> direct_jobs;
    std::vector<std::array<float,8>> draw_maps;
    background_job pack_worker;
    struct batch_draw {unsigned page_a,page_b,first,count,mesh_group=0;};
    std::vector<batch_draw> batch_draws;
    std::vector<uint8_t> batch_modes;
    std::array<uint64_t,4> mode_draws{},mode_indices{};
    struct prepared_polygon {
        int max_level=0,first_indexed=-1,indexed_count=0;
        uint32_t active_bands=0;
        size_t spare_bytes=0;
        bool flat_packet=false;
        vf2_tiles::retained_fan fan;
        std::vector<std::vector<vf2_tiles::vertex>> bands;
        std::vector<vf2_tiles::indexed_packet> indexed;

        const vf2_tiles::indexed_packet *indexed_at(int band)const{
            const int slot=band-first_indexed;
            if(slot<0||slot>=indexed_count)return nullptr;
            return &indexed[slot];
        }
    };
    std::vector<prepared_polygon> batch_packets;
    struct texture_material {
        uint16_t texw,texh,texx,texy,utexx,utexy;
        uint8_t sheet,renderer,wrapx,wrapy,utex;
        bool operator==(const texture_material &) const = default;
    };
    struct atlas_pair_entry {
        texture_material texture;
        int8_t band,max_level;
        bool native_repeat;
        size_t map_slot;
        uint64_t page_key;
        direct_bucket *cached_bucket=nullptr;
    };
    std::vector<atlas_pair_entry> atlas_pair_cache;
    struct prepared_draw {
        draw_work work;
        texture_material texture;
        size_t polygon;
        int8_t band,max_level;
        bool native_repeat;

        prepared_draw(size_t at,size_t count,texture_material source,const auto &p,int level,int max):
            texture(source),polygon(at),band(level),max_level(max),native_repeat(false)
        {
            work.texw=source.texw;work.texh=source.texh;
            auto &material=work.material;
            material.depth=float((double(at)+1)/(count+1)*2-1);
            material.info0[0]=source.texw!=0;
            material.info0[1]=source.renderer&1;
            material.info0[2]=p.checker;
            material.info1[0]=int16_t(p.texlod);
            material.info1[3]=p.utexminlod;
            material.colour[0]=(p.colour>>16)&255;material.colour[1]=(p.colour>>8)&255;
            material.colour[2]=p.colour&255;material.colour[3]=255;
            if(source.texw){
                material.info1[1]=level;
                material.info1[2]=level<0?(source.utex?2:0):(level<max?1:0);
                work.power_of_two=!(source.texw&(source.texw-1))&&!(source.texh&(source.texh-1));
            }
        }
        template<class Geometry> prepared_draw(const Geometry &packet,size_t at,size_t count,
                      texture_material source,const auto &p,int level,int max):
            prepared_draw(at,count,source,p,level,max)
        {
            if constexpr(std::is_same_v<Geometry,vf2_tiles::indexed_packet>)native_repeat=packet.native_repeat;
            work.set_packet(packet);
        }
    };
    struct packet_location {
        enum kind { full, ordinary, fan };
        size_t vertex,index;
        kind source;
    };
    struct prepared_draw_range {
        std::vector<prepared_draw> draws;
        vf2_tiles::packet_storage storage;
        std::vector<vf2_tiles::retained_fan> fans;
        std::vector<packet_location> locations;
        size_t first_job=0;
        size_t spare_bytes=0;
        bool supported=true;
        unsigned retained=0;
        unsigned sampleless=0;
    };
    std::vector<prepared_draw_range> prepared_draws;
    bool draw_lists_ready=false;
    uint64_t retained_fans=0;
    uint64_t prepared_draw_frames=0,prepared_draw_jobs=0;
    uint64_t ordinary_cell_vertices=0,full_cell_vertices=0;
    uint64_t sampleless_polygons=0;
    GLint raster_subpixel_bits=0;
    const bool cull_sampleless=std::getenv("VF2_CULL_SAMPLELESS_BOUNDS")!=nullptr;
    const bool compact_cell_vertices=std::getenv("VF2_COMPACT_CELL_VERTICES")!=nullptr;
    const bool keep_inactive_slots=std::getenv("VF2_KEEP_INACTIVE_PACKET_SLOTS")!=nullptr;
    const bool fused_uv_bounds=std::getenv("VF2_FUSED_UV_BOUNDS")!=nullptr;
    const bool packet_chunk_capacity=std::getenv("VF2_PACKET_CHUNK_CAPACITY")!=nullptr;
    const bool shared_packet_storage=std::getenv("VF2_SHARED_PACKET_STORAGE")!=nullptr;
    bool shared_packets_active=false;
    uint64_t shared_packet_frames=0;
    const bool packet_trim_test=std::getenv("VF2_PACKET_TRIM_TEST")!=nullptr;
    uint64_t packet_trims=0,packet_trim_checksum=0;
    const bool cell_vertex_stats=std::getenv("VF2_CELL_VERTEX_STATS")!=nullptr;
    const bool use_prepared_draws=std::getenv("VF2_PREPARED_DRAWS")!=nullptr;

    const bool compact_packets=std::getenv("VF2_COMPACT_PACKETS")!=nullptr;
    bool packets_ready=false;
    const bool stage_materials=std::getenv("VF2_STAGE_MATERIALS")!=nullptr;
    bool staged_plan=false,staged_uploads=false;
    const void *staged_polys=nullptr;
    bool staged_projected=false;
    size_t staged_count=0;
    gpu3d_frame staged_frame{};
    std::vector<unsigned> staged_rows;
    std::vector<GLuint> retired_tiles;
    double staged_cpu_ms=0,staged_cpu_total=0;
    uint64_t staged_frames=0;

    struct batch_program {
        GLuint name=0;
        GLint size,scale,height,owners,atlas_a,atlas_b,palette;
        int configured_width=0,configured_height=0,configured_owner=-1;
    };
    std::array<batch_program,4> batch_programs{},compact_programs{};
    std::unordered_map<uint64_t,GLuint,material_hash> tiles;
    size_t tile_bytes=0;
    uint64_t tile_hits=0,tile_misses=0,tile_uploads=0,tile_clears=0,draw_calls=0,submitted_vertices=0,painted_frames=0;
    std::vector<GLuint> targets;
    int width=0,height=0;
    gpu3d_rams rams{};
    bool have_rams=false,pending=false;
    std::array<uint8_t,256> gamma{};
    static constexpr unsigned palettes_per_bank=2048;
    static constexpr unsigned palette_height=palettes_per_bank/2;
    const unsigned palette_banks=wide_palette_atlas?2:1;
    const unsigned palette_capacity=palette_banks*palettes_per_bank;
    static unsigned palette_bank(unsigned row){return row/palettes_per_bank;}
    static unsigned palette_y(unsigned row){return (row%palettes_per_bank)/2;}
    static unsigned palette_x_attribute(unsigned row){return (row&1u)*256+palette_bank(row)*512;}
    struct palette_index {
        // A full 48-bit material key and a 1-based row fit in one slot.
        static constexpr unsigned row_bits=13,row_mask=(1u<<row_bits)-1;
        std::vector<uint64_t> slots;
        explicit palette_index(size_t capacity):slots(2*capacity){}
        uint64_t &find_slot(uint64_t key){
            const uint32_t hash=uint32_t(material_hash{}(key))*0x9e3779b9u;
            const auto slot_count=slots.size();
            size_t index=hash>>(32-std::countr_zero(slot_count));
            while(slots[index]&&(slots[index]>>row_bits)!=key)
                index=(index+1)&(slot_count-1);
            return slots[index];
        }
    };
    std::unordered_map<uint64_t,GLuint,material_hash> palettes;
    const std::unique_ptr<palette_index> packed_palettes=std::getenv("VF2_PACKED_PALETTE_INDEX")?
        std::make_unique<palette_index>(palette_capacity):nullptr;
    unsigned palette_count=0;
    const bool profile_palettes=std::getenv("VF2_PALETTE_CHURN")!=nullptr;
    struct palette_frame_stats {
        uint64_t probes=0, built=0, capacity=0, luma=0, xlat=0, gamma=0, initial=0;
        uint64_t upload_bytes=0, upload_calls=0;
        double prepare_ms=0, upload_ms=0;
    } palette_stats;
    std::vector<uint8_t> palette_pixels;
    const bool hash_palette_colours=std::getenv("VF2_PALETTE_COLOR_HASH")!=nullptr;
    std::vector<uint64_t> palette_colour_hashes=std::vector<uint64_t>(hash_palette_colours?palette_capacity:0);
    static void hash_bytes(uint64_t &hash,const void *data,size_t size){
        const auto *bytes=static_cast<const uint8_t *>(data);
        for(size_t i=0;i<size;++i)hash=(hash^bytes[i])*1099511628211ull;
    }
    const std::unique_ptr<vf2_batch::palette_shades> shade_cache=std::getenv("VF2_PALETTE_SHADE_CACHE")?
        std::make_unique<vf2_batch::palette_shades>():nullptr;
    static constexpr unsigned palette_entries=128,palette_width=2*palette_entries;
    struct palette_dirty_rows {int first=palette_height,last=-1;};
    std::array<palette_dirty_rows,2> palette_dirty{},spare_palette_dirty{};
    const bool rotate_palette_atlas=std::getenv("VF2_ROTATE_3D_PALETTE")!=nullptr;
    uint64_t palette_switches=0;
    struct uniform_locations {
#define VF2_DECLARE_UNIFORM(n) GLint n=-1;
        VF2_GLES_UNIFORMS(VF2_DECLARE_UNIFORM)
#undef VF2_DECLARE_UNIFORM
    } loc;
    using clock=std::chrono::steady_clock;
    const bool profile_packet_phases=std::getenv("VF2_PACKET_PHASES")!=nullptr;
    bool timing_packets=false;
    double current_pack_ms=0;
    std::vector<std::array<double,4>> packet_phases;
    static double phase_ms(clock::time_point start,clock::time_point end){
        return std::chrono::duration<double,std::milli>(end-start).count();
    }
    std::chrono::steady_clock::time_point began;
    double cpu_time=0;
    void clear_palettes() {
        if(packed_palettes)std::fill(packed_palettes->slots.begin(),packed_palettes->slots.end(),0);
        else palettes.clear();
        palette_count=0;
        palette_dirty={};
    }
    void clear_tiles(bool defer=false) {
        if(!tiles.empty())tile_clears++;
        for(auto &entry:tiles){if(defer)retired_tiles.push_back(entry.second);else glDeleteTextures(1,&entry.second);}
        tiles.clear();tile_bytes=0;
    }
    void release_retired_tiles(){for(GLuint t:retired_tiles)glDeleteTextures(1,&t);retired_tiles.clear();}
    bool same_rams(const gpu3d_rams &next)const{
        return have_rams&&std::equal(std::begin(rams.tex),std::end(rams.tex),std::begin(next.tex))&&
            std::equal(std::begin(rams.gen),std::end(rams.gen),std::begin(next.gen))&&
            rams.luma==next.luma&&rams.xlat==next.xlat&&rams.gamma==next.gamma&&
            std::equal(std::begin(rams.tex_page_gen),std::end(rams.tex_page_gen),std::begin(next.tex_page_gen))&&
            std::memcmp(gamma.data(),next.gamma,256)==0;
    }
    void apply_rams(const gpu3d_rams &next,bool cpu_only){
        bool changed[2];for(int i=0;i<2;i++)changed[i]=!have_rams||next.gen[i]!=rams.gen[i]||next.tex[i]!=rams.tex[i];
        if(!have_rams||next.tex[0]!=rams.tex[0]||next.tex[1]!=rams.tex[1])batch_atlas.clear();
        const bool luma_changed=next.gen[2]!=rams.gen[2], xlat_changed=next.gen[3]!=rams.gen[3];
        const bool gamma_changed=std::memcmp(gamma.data(),next.gamma,256)!=0;
        if(!have_rams||luma_changed||xlat_changed||gamma_changed){
            if(profile_palettes){
                palette_stats.initial+=!have_rams;
                palette_stats.luma+=have_rams&&luma_changed;
                palette_stats.xlat+=have_rams&&xlat_changed;
                palette_stats.gamma+=have_rams&&gamma_changed;
            }
            clear_palettes();
        }
        if(shade_cache&&(!have_rams||xlat_changed||gamma_changed||next.xlat!=rams.xlat))
            shade_cache->reset();
        rams=next;std::memcpy(gamma.data(),next.gamma,256);
        if(direct_tiles){if(changed[0]||changed[1])clear_tiles(cpu_only);}
        else for(int i=0;i<2;i++)if(changed[i])decode_sheet(i);
        have_rams=true;
    }

    GLuint tile(const gpu3d_poly &p,int level,bool micro) {
        unsigned w=micro?128:p.texw>>level,h=micro?128:p.texh>>level;
        unsigned x=micro?p.utexx:((uint32_t(p.texx)-2048)>>level)&2047;
        unsigned y=micro?p.utexy:((uint32_t(p.texy)-1024)>>level)&1023;
        unsigned sheet=p.sheet^(micro?1:(level&1));
        bool transparent=p.renderer&1;
        uint64_t key=x|(uint64_t(y)<<11)|(uint64_t(w)<<21)|(uint64_t(h)<<33)|
            (uint64_t(sheet)<<45)|(uint64_t(bool(p.wrapx))<<46)|(uint64_t(bool(p.wrapy))<<47)|(uint64_t(transparent)<<48);
        if(auto it=tiles.find(key);it!=tiles.end()){tile_hits++;return it->second;}
        tile_misses++;unsigned channels=transparent?2:1;
        std::vector<uint8_t> data(size_t(w)*h*channels);
        for(unsigned v=0;v<h;v++)for(unsigned u=0;u<w;u++) {
            unsigned px=(x+u)&2047,py=(y+v)&1023,right=px>=1024?1024:0,xo=(px-right)>>1;
            unsigned shift=((xo&1)<<4)+((~u&1)<<2)+((~v&1)<<3);
            unsigned index=(rams.tex[sheet][(((((py>>1)<<9)^(right<<8))|xo)>>1)&0x7ffff]>>shift)&15;
            data[(size_t(v)*w+u)*channels]=index<<4;
            if(transparent)data[(size_t(v)*w+u)*2+1]=index==15?0:255;
        }
        // Model 2 borrows an opaque neighbour's index before filtering an
        // alpha edge. Keep alpha unchanged and extend the nearest colour.
        if(transparent) {
            auto original=data;
            for(unsigned v=0;v<h;v++)for(unsigned u=0;u<w;u++)if(!original[(size_t(v)*w+u)*2+1]) {
                const int offsets[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
                for(auto &offset:offsets) {
                    int a=int(u)+offset[0],b=int(v)+offset[1];
                    a=p.wrapx?(a+int(w))%w:std::clamp(a,0,int(w)-1);
                    b=p.wrapy?(b+int(h))%h:std::clamp(b,0,int(h)-1);
                    size_t at=(size_t(b)*w+a)*2;
                    if(original[at+1]){data[(size_t(v)*w+u)*2]=original[at];break;}
                }
            }
        }
        GLuint texture;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,p.wrapx?GL_REPEAT:GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,p.wrapy?GL_REPEAT:GL_CLAMP_TO_EDGE);
        GLenum format=transparent?GL_LUMINANCE_ALPHA:GL_LUMINANCE;
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glTexImage2D(GL_TEXTURE_2D,0,format,w,h,0,format,GL_UNSIGNED_BYTE,data.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT,4);
        tiles.emplace(key,texture);tile_bytes+=data.size();tile_uploads+=data.size();return texture;
    }
    GLuint palette(const auto &p) {
        uint64_t key=uint64_t(p.lumabase)|(uint64_t(p.pal)<<16)|(uint64_t(p.luma)<<32);
        uint64_t *slot=nullptr;
        if(packed_palettes){
            slot=&packed_palettes->find_slot(key);
            if(*slot)return unsigned(*slot&palette_index::row_mask)-1;
        }else if(auto it=palettes.find(key);it!=palettes.end())return it->second;
        if(palette_count>=palette_capacity){
            if(profile_palettes)++palette_stats.capacity;
            return ~0u;
        }
        if(profile_palettes)++palette_stats.built;
        const unsigned row=palette_count,bank=palette_bank(row),y=palette_y(row),x=(row&1u)*palette_entries;
        uint8_t *rgba=palette_pixels.data()+((size_t(bank)*palette_height+y)*palette_width+x)*4;
        if(shade_cache)
            shade_cache->write(rgba,palette_entries,p.pal,p.lumabase,p.luma,rams.luma,rams.xlat,rams.gamma);
        else for(unsigned i=0;i<palette_entries;i++) {
            unsigned li=(p.lumabase+i)&0x7fff;
            unsigned l=std::min((unsigned(rams.luma[li])*p.luma)/256,63u);
            for(int c=0;c<3;c++) rgba[i*4+c]=rams.gamma[rams.xlat[c*0x2000+(((p.pal>>(5*c))&31)<<8)+l]&255];
            rgba[i*4+3]=255;
        }
        if(hash_palette_colours){
            uint64_t hash=14695981039346656037ull;
            hash_bytes(hash,rgba,palette_entries*4);
            palette_colour_hashes[row]=hash;
        }
        auto &dirty=palette_dirty[bank];
        dirty.first=std::min(dirty.first,int(y));dirty.last=std::max(dirty.last,int(y));
        if(slot)*slot=(key<<palette_index::row_bits)|(row+1);
        else palettes.emplace(key,row);
        ++palette_count;
        return row;
    }
    bool prepare_palettes(const auto *polys,size_t count,std::vector<unsigned> &rows,bool upload=true){
        const auto start=profile_palettes?clock::now():clock::time_point{};
        rows.resize(count);bool complete=false;
        for(int attempt=0;attempt<2;attempt++){
            complete=true;
            for(size_t i=0;i<count;i++)if((polys[i].renderer&2)&&polys[i].n>=3&&polys[i].n<=8){
                if(profile_palettes)++palette_stats.probes;
                rows[i]=palette(polys[i]);if(rows[i]==~0u){complete=false;break;}
            }
            if(complete)break;
            clear_palettes();
        }
        if(profile_palettes)palette_stats.prepare_ms+=phase_ms(start,clock::now());
        if(!complete)return false;
        if(upload)upload_palettes();return true;
    }
    void upload_palettes(){
        // Each original pair of indices addresses one luma entry, so one
        // texel represents both at the existing normalized nearest coordinates.
        const bool rotate=rotate_palette_atlas&&
            std::any_of(palette_dirty.begin(),palette_dirty.end(),[](auto r){return r.first<=r.last;});
        glActiveTexture(direct_tiles?GL_TEXTURE2:GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D,rotate?palette_spare:palette_atlas);
        for(unsigned bank=0;bank<palette_banks;++bank){
            auto upload=[&](palette_dirty_rows dirty){
                if(dirty.last<dirty.first)return;
                const auto start=profile_palettes?clock::now():clock::time_point{};
                const int rows=dirty.last-dirty.first+1;
                const size_t offset=(size_t(bank)*palette_height+dirty.first)*palette_width*4;
                glTexSubImage2D(GL_TEXTURE_2D,0,bank*palette_width,dirty.first,palette_width,rows,
                                GL_RGBA,GL_UNSIGNED_BYTE,palette_pixels.data()+offset);
                if(profile_palettes){
                    ++palette_stats.upload_calls;
                    palette_stats.upload_bytes+=uint64_t(rows)*palette_width*4;
                    palette_stats.upload_ms+=phase_ms(start,clock::now());
                }
            };
            auto dirty=palette_dirty[bank];
            if(rotate){
                auto lag=spare_palette_dirty[bank];
                const int used=std::clamp(int(palette_count)-int(bank*palettes_per_bank),0,int(palettes_per_bank));
                lag.last=std::min(lag.last,(used+1)/2-1);
                if(lag.first<=lag.last){
                    // A spare missed one upload. Merge touching ranges and
                    // avoid copying the unchanged space between disjoint ones.
                    if(dirty.first<=dirty.last&&dirty.first<=lag.last+1&&lag.first<=dirty.last+1){
                        dirty.first=std::min(dirty.first,lag.first);
                        dirty.last=std::max(dirty.last,lag.last);
                    }else upload(lag);
                }
            }
            upload(dirty);
        }
        if(rotate){
            spare_palette_dirty=palette_dirty;
            std::swap(palette_atlas,palette_spare);
            ++palette_switches;
        }
        palette_dirty={};
    }
    static void texture_parameters() {
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    void decode_sheet(int sheet) {
        std::vector<uint8_t> decoded(2048*1024);
        for(unsigned y=0;y<1024;y++)for(unsigned x=0;x<2048;x++) {
            unsigned right=x>=1024?1024:0,xo=(x-right)>>1;
            unsigned row=(y>>1)<<9,flip=right<<8;
            unsigned shift=((xo&1)<<4)+((~x&1)<<2)+((~y&1)<<3);
            decoded[(x>=1024?1024*1024:0)+y*1024+(x&1023)]=uint8_t(((rams.tex[sheet][(((row^flip)|xo)>>1)&0x7ffff]>>shift)&15)<<4);
        }
        for(int half=0;half<2;half++) {
            glActiveTexture(GL_TEXTURE0+sheet*2+half);glBindTexture(GL_TEXTURE_2D,sheets[sheet*2+half]);texture_parameters();
            glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE,1024,1024,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,decoded.data()+half*1024*1024);
        }
    }
    void prepare_flat_packet(auto &vertices,auto &indices,
                             const auto &p,int x0,int y0,int x1,int y1,int scale)
    {
        vertices.clear();indices.clear();
        const double left = double(x0) / scale, right = double(x1) / scale;
        const double top = double(y0) / scale, bottom = double(y1) / scale;
        const auto point_at = [&](int i) -> vf2_tiles::point {
            if constexpr (std::is_same_v<std::decay_t<decltype(p)>, gpu3d_poly>)
                return {p.x[i], p.y[i], 1, 0, 0};
            else
                return {p.points[i].x, p.points[i].y, 1, 0, 0};
        };
        const auto emit = [&](const auto &polygon) {
            if (frame_indexed && polygon.size() >= 3) {
                for (const auto &point : polygon) {
                    vf2_tiles::vertex v{};
                    v.p = point;
                    vertices.push_back(v);
                }
                for (size_t i = 1; i + 1 < polygon.size(); ++i)
                    indices.insert(indices.end(), {0, uint32_t(i), uint32_t(i+1)});
            } else if (!frame_indexed) {
                for (size_t i = 1; i + 1 < polygon.size(); ++i)
                    for (const auto &point : {polygon[0], polygon[i], polygon[i+1]}) {
                        vf2_tiles::vertex v{};
                        v.p = point;
                        vertices.push_back(v);
                    }
            }
        };
#ifdef MODEL2_ORIGINAL
        static const bool stack_clipping = std::getenv("DAYTONA_STACK_FLAT_CLIP") ||
            std::getenv("VF2_STACK_FLAT_CLIP");
#else
        static const bool stack_clipping = std::getenv("VF2_STACK_FLAT_CLIP") != nullptr;
#endif
        if (stack_clipping && p.n >= 3 && p.n <= 8) {
            std::array<vf2_tiles::point, 8> points;
            bool inside = true;
            for (int i = 0; i < p.n; ++i) {
                const auto pt = point_at(i);
                points[i] = pt;
                inside &= pt.x >= left && pt.x <= right && pt.y >= top && pt.y <= bottom;
            }
            if (inside) {
                emit(std::span<const vf2_tiles::point>(points.data(), p.n));
                return;
            }
            vf2_tiles::stack_polygon polygon;
            polygon.borrow(points.data(), p.n);
            polygon.clip_by([&](const auto &v) { return double(v.x) - left; });
            polygon.clip_by([&](const auto &v) { return right - v.x; });
            polygon.clip_by([&](const auto &v) { return double(v.y) - top; });
            polygon.clip_by([&](const auto &v) { return bottom - v.y; });
            emit(polygon);
            return;
        }
        std::vector<vf2_tiles::point> polygon;
        for (int i = 0; i < p.n; ++i)
            polygon.push_back(point_at(i));
        polygon = vf2_tiles::clip(std::move(polygon), [&](const auto &v) { return double(v.x) - left; });
        polygon = vf2_tiles::clip(std::move(polygon), [&](const auto &v) { return right - v.x; });
        polygon = vf2_tiles::clip(std::move(polygon), [&](const auto &v) { return double(v.y) - top; });
        polygon = vf2_tiles::clip(std::move(polygon), [&](const auto &v) { return bottom - v.y; });
        emit(polygon);
    }

    static void bind_shared_packet(const prepared_draw_range &range,size_t at,draw_work &work){
        const auto &location=range.locations[at];
        if(location.source==packet_location::fan){
            work.set_packet(range.fans[location.vertex]);
        }else if(work.n){
            if(location.source==packet_location::ordinary)work.ordinary=range.storage.ordinary.data()+location.vertex;
            else work.vertices=range.storage.vertices.data()+location.vertex;
            work.indices=range.storage.indices.data()+location.index;
        }
    }

    void hash_shared_packets()const{
        uint64_t hash=14695981039346656037ull,indices=0;
        const auto add=[&](const auto &value){
            const auto *bytes=reinterpret_cast<const unsigned char *>(&value);
            for(size_t i=0;i<sizeof(value);++i)hash=(hash^bytes[i])*1099511628211ull;
        };
        for(const auto &range:prepared_draws)for(size_t at=0;at<range.draws.size();++at){
            const auto &draw=range.draws[at];
            if(!draw.texture.texw)continue;
            auto work=draw.work;
            bind_shared_packet(range,at,work);
            add(uint32_t(draw.polygon));add(int(draw.band));add(uint32_t(work.ni));
            indices+=work.ni;
            for(size_t i=0;i<work.ni;++i){
                const auto index=work.indices[i];
                if(const auto *fan=work.fan){
                    vf2_tiles::vertex v{};
                    v.p=fan->position(index);
                    std::copy_n(fan->a,4,v.a);std::copy_n(fan->b,4,v.b);
                    add(v);
                }else if(work.ordinary)add(work.ordinary[index].expand());
                else add(work.vertices[index]);
            }
        }
        std::fprintf(stderr,"Expanded texture triangles: %016llx %llu\n",
                     (unsigned long long)hash,(unsigned long long)(indices/3));
    }

    unsigned uniform_mip_axes(const gpu3d_projected_poly &p,int level)const{
        const unsigned w=p.texw>>level,h=p.texh>>level;
        if(!w||!h||w>4||h>4)return 0;
        const unsigned x=((unsigned(p.texx)-2048)>>level)&2047;
        const unsigned y=((unsigned(p.texy)-1024)>>level)&1023;
        const auto *sheet=rams.tex[p.sheet^(level&1)];
        unsigned axes=3, texels[4][4]{};
        for(unsigned v=0;v<h;++v)for(unsigned u=0;u<w;++u){
            const unsigned px=(x+u)&2047,py=(y+v)&1023;
            const unsigned half=px>=1024?1024:0;
            const unsigned xo=(px-half)>>1;
            const unsigned address=((((py>>1)<<9)^(half<<8))|xo)>>1;
            const unsigned shift=((xo&1)<<4)+((~u&1)<<2)+((~v&1)<<3);
            const unsigned index=(sheet[address&0x7ffff]>>shift)&15;
            texels[v][u]=index;
            if(index!=texels[v][0])axes&=~1u;
            if(index!=texels[0][u])axes&=~2u;
        }
        return axes;
    }

    template<bool Narrow=false>
    static bool lacks_sample_centers(const gpu3d_projected_poly &p,int left,int top,int right,int bottom){
        float min_x=INFINITY,min_y=INFINITY,max_x=-INFINITY,max_y=-INFINITY;
        float min_q=INFINITY,max_q=0.f;
        for(unsigned i=0;i<p.n;++i){
            const auto &v=p.points[i];
            if(!(v.x>=left&&v.x<=right&&v.y>=top&&v.y<=bottom&&v.q>0&&v.q<INFINITY))return false;
            min_x=std::min(min_x,v.x);min_y=std::min(min_y,v.y);
            max_x=std::max(max_x,v.x);max_y=std::max(max_y,v.y);
            if constexpr(Narrow){
                min_q=std::min(min_q,v.q);
                max_q=std::max(max_q,v.q);
            }
        }
        // Keep a margin for screen-coordinate and raster subpixel rounding.
        const auto no_center=[](float lo,float hi,double margin){
            return hi-lo<1-2*margin&&
                std::ceil(double(lo)-0.5-margin)>std::floor(double(hi)-0.5+margin);
        };
        if constexpr(Narrow){
            // The tighter margin is for in-viewport polygons with bounded
            // reciprocal depth and limited perspective variation.
            if(min_q>=0x1p-24f&&max_q<=1.f&&max_q<=4.f*min_q)
                return no_center(min_x,max_x,0.125)||no_center(min_y,max_y,0.125);
        }
        return no_center(min_x,max_x,0.25)||no_center(min_y,max_y,0.25);
    }
    bool sampleless_pairs_current(const gpu3d_projected_poly &p,int max_level)const{
        // Workers only read atlas slots here. First touches or generation
        // changes must still run normal planning to preserve later placement.
        bool ready=true;
        vf2_tiles::visit_indexed_bands(p,max_level,limited_mip_scan,[&](int band,double,double){
            if(ready)ready=batch_atlas.pair_current(rams,p,band,max_level,
                vf2_tiles::native_repeat_material(p,band));
        });
        return ready;
    }

    void prepare_shared_projected(const gpu3d_projected_poly *polys,size_t count,
                                  const gpu3d_frame &f,bool *palettes_ready){
        batch_packets.clear();
        shared_packets_active=true;
        const int chunks=std::max(1,std::min(int(count),row_pool::get().threads()*16));
        const int palette_task=palettes_ready?1:0;
        prepared_draws.resize(chunks);
        static const bool test_viewport_bounds=std::getenv("VF2_FAST_VIEWPORT_BOUNDS")!=nullptr;
        static const bool collapse_uniform=std::getenv("VF2_CONSTANT_MIP_MERGE")!=nullptr;
        static const bool share_envelope=std::getenv("DAYTONA_REUSE_FAN_ENVELOPE")&&
            !std::getenv("VF2_CLIP_FLOAT_ALL");
        const bool atlas_uniform=batch_atlas.records_uniform_axes();
        // Pixel-center rejection assumes the single-sample targets from make_targets.
        const bool screen_cull=cull_sampleless&&raster_subpixel_bits>=4&&f.scale==1&&width==496&&height==384;
        static const bool narrow_sampleless=std::getenv("DAYTONA_NARROW_SAMPLELESS")!=nullptr;
        parallel_tasks(chunks+palette_task,[&](int task){
            if(palettes_ready&&task==0){
                *palettes_ready=prepare_palettes(polys,count,staged_rows,false);
                return;
            }
            const int chunk=task-palette_task;
            auto &r=prepared_draws[chunk];
            r.draws.clear();r.locations.clear();r.fans.clear();r.storage.clear();r.retained=0;r.sampleless=0;r.supported=true;
            for(size_t at=count*chunk/chunks;at<count*(chunk+1)/chunks;++at){
                const auto &p=polys[at];
                const bool textured=p.renderer&2;
                if(p.n<3||p.n>8||p.renderer==1||(textured&&(!p.texw||!p.texh)))continue;
                if(p.texlod<-32768||p.texlod>32767){r.supported=false;return;}
                const int x0=std::max(0,int(p.vp[0])*f.scale),x1=std::min(width,(int(p.vp[1])+1)*f.scale);
                const int y0=std::max(0,int(p.vp[2])*f.scale),y1=std::min(height,(int(p.vp[3])+1)*f.scale);
                if(x1<=x0||y1<=y0)continue;
                const int max=textured?max_texture_mip(p):0;
                if(screen_cull&&textured&&
                    (narrow_sampleless?lacks_sample_centers<true>(p,x0,y0,x1,y1):lacks_sample_centers(p,x0,y0,x1,y1))&&
                    sampleless_pairs_current(p,max)){
                    ++r.sampleless;
                    continue;
                }
                const auto append=[&](const vf2_tiles::packet_writer &packet,texture_material material,int band,int max){
                    auto &draw=r.draws.emplace_back(at,count,material,p,band,max);
                    const bool ordinary=packet.ordinary.size()!=0;
                    r.locations.push_back({ordinary?packet.ordinary.first:packet.vertices.first,
                        packet.indices.first,ordinary?packet_location::ordinary:packet_location::full});
                    draw.native_repeat=packet.native_repeat;
                    draw.work.n=ordinary?packet.ordinary.size():packet.vertices.size();
                    draw.work.ni=packet.indices.size();
                };
                if(!textured){
                    vf2_tiles::packet_writer packet(r.storage);
                    prepare_flat_packet(packet.vertices,packet.indices,p,x0,y0,x1,y1,f.scale);
                    texture_material material{};material.renderer=p.renderer;
                    append(packet,material,-1,0);
                    continue;
                }
                const texture_material material{p.texw,p.texh,p.texx,p.texy,p.utexx,p.utexy,
                    p.sheet,p.renderer,p.wrapx,p.wrapy,p.utex};
                const float left=float(x0)/f.scale,top=float(y0)/f.scale,right=float(x1)/f.scale,bottom=float(y1)/f.scale;
                int uncut_band=-2;
                vf2_tiles::retained_fan fan;
                if(vf2_tiles::retain_fan(fan,p,max,left,top,right,bottom,indexed_texture_cells,
                                         retained_cell_bounds,fused_uv_bounds,share_envelope?&uncut_band:nullptr)){
                    auto &draw=r.draws.emplace_back(at,count,material,p,fan.band,max);
                    draw.work.n=fan.n;
                    draw.work.ni=fan.index_count();
                    r.locations.push_back({r.fans.size(),0,packet_location::fan});
                    r.fans.push_back(fan);
                    ++r.retained;
                    continue;
                }
                const auto emit=[&](int band,double lower,double upper,bool outer_kept=false){
                    vf2_tiles::packet_writer packet(r.storage);
                    unsigned uniform_axes=0;
                    if((collapse_uniform||atlas_uniform)&&band>=0&&p.renderer==2){
                        const auto uniform=[&](int level){
                            return atlas_uniform?batch_atlas.current_uniform_axes(rams,p,level):uniform_mip_axes(p,level);
                        };
                        uniform_axes=uniform(band);
                        if(uniform_axes&&band!=max)uniform_axes&=uniform(band+1);
                    }
                    vf2_tiles::clip_indexed_band<true>(packet,p,band,max,left,top,right,bottom,lower,upper,
                        origin_packet_fan,indexed_texture_cells,compact_cell_vertices,test_viewport_bounds,uniform_axes,outer_kept);
                    if(packet.indices.size())append(packet,material,band,max);
                };
                if(uncut_band>=-1)emit(uncut_band,0,INFINITY,true);
                else vf2_tiles::visit_indexed_bands(p,max,limited_mip_scan,emit);
            }
        });
        draw_lists_ready=std::all_of(prepared_draws.begin(),prepared_draws.end(),
            [](const auto &range){return range.supported;});
        if(!draw_lists_ready)return;
        size_t spare=0;
        for(const auto &r:prepared_draws){
            retained_fans+=r.retained;
            sampleless_polygons+=r.sampleless;
            spare+=r.storage.spare_bytes()+(r.fans.capacity()-r.fans.size())*sizeof(vf2_tiles::retained_fan);
        }
        if(packet_trim_test)packet_trim_checksum=packet_trim_checksum*31+spare;
        if(spare>(packet_trim_test?16*1024:4*1024*1024)){
            if(packet_trim_test)++packet_trims;
            const auto trim=[](auto &v){if(v.capacity()>v.size()){using V=std::decay_t<decltype(v)>;V(v.begin(),v.end()).swap(v);}};
            for(auto &r:prepared_draws){
                trim(r.storage.vertices);trim(r.storage.ordinary);trim(r.storage.indices);trim(r.fans);
            }
        }
        static const bool hash_packets=std::getenv("VF2_CLIP_TRIANGLE_HASH")!=nullptr;
        if(hash_packets)hash_shared_packets();
        ++shared_packet_frames;
        packets_ready=true;
    }

    template<class Polygon>
    void prepare_packets(const Polygon *polys,size_t count,const gpu3d_frame &f,bool *palettes_ready=nullptr,bool allow_fans=false){
        if constexpr(std::is_same_v<Polygon,gpu3d_projected_poly>){
            if(shared_packet_storage&&allow_fans&&use_prepared_draws&&frame_indexed&&
               compact_packets&&stack_packets&&!mesh_render){
                prepare_shared_projected(polys,count,f,palettes_ready);
                return;
            }
        }
        if(shared_packets_active){
            prepared_draws.clear();
            shared_packets_active=false;
        }
        batch_packets.resize(count);
        draw_lists_ready=use_prepared_draws&&frame_indexed&&compact_packets&&!mesh_render;
        const bool keep_inactive=keep_inactive_slots&&draw_lists_ready&&stack_packets;
        const bool count_by_chunk=packet_chunk_capacity&&draw_lists_ready;
        // Polygon clipping and wrap splitting are independent. Keep all GL
        // resource access on the submitting thread, and prepare packets across
        // the existing row pool before filling the shared page-pair buckets.
        const auto packets_range=[&](int begin,int end,prepared_draw_range *draws){
            size_t chunk_spare=0;
            for(int at=begin;at<end;at++){
                auto &prepared=batch_packets[at];const auto &p=polys[at];
                if(!keep_inactive&&prepared.indexed_count==0&&!prepared.indexed.empty())
                    vf2_tiles::clear_packet<true>(prepared.indexed.front());
                prepared.active_bands=0;prepared.fan.n=0;prepared.flat_packet=false;
                if(!count_by_chunk)prepared.spare_bytes=0;
                size_t &spare=count_by_chunk?chunk_spare:prepared.spare_bytes;
                const bool textured=p.renderer&2;
                if(p.n<3||p.n>8||p.renderer==1||(textured&&(!p.texw||!p.texh))||(!textured&&!draws)){
                    prepared.bands.clear();prepared.indexed.clear();prepared.indexed_count=0;continue;
                }
                if(draws&&(p.texlod<-32768||p.texlod>32767))draws->supported=false;
                int x0=std::max(0,int(p.vp[0])*f.scale),x1=std::min(width,(int(p.vp[1])+1)*f.scale);
                int y0=std::max(0,int(p.vp[2])*f.scale),y1=std::min(height,(int(p.vp[3])+1)*f.scale);
                if(x1<=x0||y1<=y0){prepared.bands.clear();prepared.indexed.clear();prepared.indexed_count=0;continue;}
                if(!textured){
                    prepared.bands.clear();
                    prepared.indexed.resize(1);
                    prepared.indexed_count=0;
                    prepared.flat_packet=true;
                    auto &packet=prepared.indexed.front();
                    vf2_tiles::clear_packet<true>(packet);
                    prepare_flat_packet(packet.vertices,packet.indices,p,x0,y0,x1,y1,f.scale);
                    spare+=(packet.vertices.capacity()-packet.vertices.size())*sizeof(vf2_tiles::vertex)+
                        (packet.indices.capacity()-packet.indices.size())*sizeof(uint32_t)+
                        packet.ordinary.capacity()*sizeof(vf2_tiles::ordinary_vertex);
                    texture_material flat_material{};
                    flat_material.renderer=p.renderer;
                    draws->draws.emplace_back(packet,size_t(at),count,flat_material,p,-1,0);
                    continue;
                }
                prepared.max_level=max_texture_mip(p);
                if(frame_indexed){
                    prepared.bands.clear();
                    const float left=float(x0)/f.scale,top=float(y0)/f.scale,right=float(x1)/f.scale,bottom=float(y1)/f.scale;
                    if(allow_fans&&draws&&vf2_tiles::retain_fan(prepared.fan,p,prepared.max_level,
                            left,top,right,bottom,indexed_texture_cells,retained_cell_bounds,fused_uv_bounds)){
                        for(auto &slot:prepared.indexed){
                            if(!keep_inactive)vf2_tiles::clear_packet<true>(slot);
                            spare+=slot.vertices.capacity()*sizeof(vf2_tiles::vertex)
                                +slot.ordinary.capacity()*sizeof(vf2_tiles::ordinary_vertex)
                                +slot.indices.capacity()*sizeof(uint32_t);
                        }
                        prepared.indexed_count=0;
                        const texture_material texture{p.texw,p.texh,p.texx,p.texy,p.utexx,p.utexy,
                            p.sheet,p.renderer,p.wrapx,p.wrapy,p.utex};
                        draws->draws.emplace_back(prepared.fan,size_t(at),count,texture,p,
                            prepared.fan.band,prepared.max_level);
                        ++draws->retained;
                        continue;
                    }
                    if(stack_packets)prepared.indexed_count=vf2_tiles::sparse_indexed_packets<true>(
                        prepared.indexed,prepared.first_indexed,prepared.indexed_count,p,prepared.max_level,left,top,right,bottom,limited_mip_scan,origin_packet_fan,indexed_texture_cells,compact_cell_vertices&&allow_fans&&draws,keep_inactive);
                    else prepared.indexed_count=vf2_tiles::sparse_indexed_packets<false>(
                        prepared.indexed,prepared.first_indexed,prepared.indexed_count,p,prepared.max_level,left,top,right,bottom,limited_mip_scan);
                }else{
                    prepared.indexed.clear();prepared.indexed_count=0;
                    prepared.bands.resize(prepared.max_level+2);
                    for(int band=-1;band<=prepared.max_level;band++){
                        if(stack_packets)vf2_tiles::packet_into<false,true>(prepared.bands[band+1],p,band,prepared.max_level,float(x0)/f.scale,float(y0)/f.scale,float(x1)/f.scale,float(y1)/f.scale,true);
                        else vf2_tiles::packet_into<false,false>(prepared.bands[band+1],p,band,prepared.max_level,float(x0)/f.scale,float(y0)/f.scale,float(x1)/f.scale,float(y1)/f.scale,true);
                    }
                }
                if(compact_packets){
                    // These vectors are hot on this worker. Avoid walking
                    // their allocation descriptors again on the GL thread.
                    for(size_t i=0;i<prepared.indexed.size();++i){
                        const auto &band=prepared.indexed[i];
                        // Reusable slots beyond indexed_count may still hold a
                        // prior frame's payload if the fast path was used.
                        const bool active=i<size_t(prepared.indexed_count);
                        if(active&&!band.indices.empty())prepared.active_bands|=uint32_t(1)<<(i+prepared.first_indexed+1);
                        spare+=(band.vertices.capacity()-(active?band.vertices.size():0))*sizeof(vf2_tiles::vertex)
                            +(band.indices.capacity()-(active?band.indices.size():0))*sizeof(uint32_t);
                        spare+=(band.ordinary.capacity()-(active?band.ordinary.size():0))*sizeof(vf2_tiles::ordinary_vertex);
                    }
                    for(size_t i=0;i<prepared.bands.size();++i){
                        const auto &band=prepared.bands[i];
                        if(!band.empty())prepared.active_bands|=uint32_t(1)<<i;
                        spare+=(band.capacity()-band.size())*sizeof(vf2_tiles::vertex);
                    }
                }
                if(draws&&prepared.active_bands){
                    const texture_material texture{p.texw,p.texh,p.texx,p.texy,p.utexx,p.utexy,
                        p.sheet,p.renderer,p.wrapx,p.wrapy,p.utex};
                    for(uint32_t active=prepared.active_bands;active;active&=active-1){
                        const int band=int(std::countr_zero(active))-1;
                        const auto *packet=prepared.indexed_at(band);
                        draws->draws.emplace_back(*packet,size_t(at),count,texture,p,band,prepared.max_level);
                    }
                }
            }
            if(count_by_chunk)draws->spare_bytes=chunk_spare;
        };
        if(palettes_ready||draw_lists_ready){
            // Both job types read the same immutable snapshot. Polygon
            // chunks stay in source order even when workers finish out of order.
            const int chunks=std::max(1,std::min(int(count),row_pool::get().threads()*16));
            if(draw_lists_ready)prepared_draws.resize(chunks);
            const int palette_task=palettes_ready?1:0;
            parallel_tasks(chunks+palette_task,[&](int task){
                if(palettes_ready&&task==0){
                    *palettes_ready=prepare_palettes(polys,count,staged_rows,false);
                    return;
                }
                const int chunk=task-palette_task;
                auto *draws=draw_lists_ready?&prepared_draws[chunk]:nullptr;
                if(draws){draws->draws.clear();draws->supported=true;draws->retained=0;}
                packets_range(int(count)*chunk/chunks,int(count)*(chunk+1)/chunks,draws);
            });
            if(draw_lists_ready)draw_lists_ready=std::all_of(prepared_draws.begin(),prepared_draws.end(),
                [](const auto &range){return range.supported;});
            if(allow_fans&&use_prepared_draws&&frame_indexed&&compact_packets&&!mesh_render){
                if(!draw_lists_ready){
                    if constexpr(std::is_same_v<Polygon,gpu3d_poly>)prepare_packets(polys,count,f);
                    return;
                }
                for(const auto &range:prepared_draws)retained_fans+=range.retained;
            }
        }else parallel_rows(int(count),[&](int begin,int end){packets_range(begin,end,nullptr);},4);
        // Bound retained spare storage as changing scenes move large polygons
        // between list slots. Current frame data is never discarded.
        size_t spare=0;
        if(count_by_chunk)for(const auto &range:prepared_draws)spare+=range.spare_bytes;
        else for(const auto &p:batch_packets){
            if(compact_packets){spare+=p.spare_bytes;continue;}
            for(const auto &band:p.bands)spare+=(band.capacity()-band.size())*sizeof(vf2_tiles::vertex);
            for(const auto &band:p.indexed)spare+=(band.vertices.capacity()-band.vertices.size())*sizeof(vf2_tiles::vertex)+(band.indices.capacity()-band.indices.size())*sizeof(uint32_t)+(band.ordinary.capacity()-band.ordinary.size())*sizeof(vf2_tiles::ordinary_vertex);
        }
        if(packet_trim_test)packet_trim_checksum=packet_trim_checksum*31+spare;
        if(spare>(packet_trim_test?16*1024:4*1024*1024)){
            if(packet_trim_test)++packet_trims;
            auto trim=[](auto &v){if(v.capacity()>v.size()){using V=std::decay_t<decltype(v)>;V(v.begin(),v.end()).swap(v);}};
            for(auto &p:batch_packets){
                for(auto &band:p.bands)trim(band);
                for(size_t i=0;i<p.indexed.size();++i){
                    auto &band=p.indexed[i];
                    if(i>=size_t(p.flat_packet?1:p.indexed_count))vf2_tiles::clear_packet<true>(band);
                    trim(band.vertices);trim(band.indices);trim(band.ordinary);
                }
            }
            if(draw_lists_ready){
                // The active buffers may have moved when spare capacity was removed.
                for(auto &range:prepared_draws)for(auto &draw:range.draws){
                    if(draw.work.fan)continue;
                    const auto &p=batch_packets[draw.polygon];
                    draw.work.set_packet(draw.texture.texw?*p.indexed_at(draw.band):p.indexed.front());
                }
            }
        }
        static const bool hash_packets=std::getenv("VF2_CLIP_TRIANGLE_HASH")!=nullptr;
        if(hash_packets){
            uint64_t hash=14695981039346656037ull,indices=0;
            const auto add=[&](const void *p,size_t n){
                const auto *bytes=static_cast<const unsigned char*>(p);
                for(size_t i=0;i<n;++i)hash=(hash^bytes[i])*1099511628211ull;
            };
            if(frame_indexed)for(uint32_t at=0;at<batch_packets.size();++at){
                const auto &record=batch_packets[at];
                const auto &fan=record.fan;
                if(fan.n){
                    const uint32_t size=fan.index_count();
                    add(&at,sizeof at);add(&fan.band,sizeof fan.band);add(&size,sizeof size);
                    indices+=size;
                    for(uint32_t i=0;i<size;++i){
                        vf2_tiles::vertex v{};
                        v.p=fan.position(vf2_tiles::fan_indices[i]);
                        std::copy_n(fan.a,4,v.a);std::copy_n(fan.b,4,v.b);
                        add(&v,sizeof v);
                    }
                    continue;
                }
                for(int i=0;i<record.indexed_count;++i){
                    const auto &packet=record.indexed[i];
                    if(packet.indices.empty())continue;
                    const int band=record.first_indexed+i;
                    const auto size=uint32_t(packet.indices.size());
                    add(&at,sizeof at);add(&band,sizeof band);add(&size,sizeof size);
                    indices+=size;
                    for(uint32_t index:packet.indices){
                        const auto vertex=packet.vertex_at(index);
                        add(&vertex,sizeof vertex);
                    }
                }
            }
            std::fprintf(stderr,"Expanded texture triangles: %016llx %llu\n",
                         (unsigned long long)hash,(unsigned long long)(indices/3));
        }
        packets_ready=true;
    }
    bool prepare_batch_reference(const gpu3d_poly *polys,size_t count,const gpu3d_frame &f,const std::vector<unsigned> &rows,bool owners){
        index_overflow=false;batch_modes.clear();
        batch_vertices.clear();batch_vertices.reserve(count*12);
        batch_indices.clear();
        for(auto &bucket:batch_buckets){bucket.second.vertices.clear();bucket.second.indices.clear();}
        batch_draws.clear();
        if(!packets_ready)prepare_packets(polys,count,f);
        packets_ready=false;
        for(size_t at=0;at<count;at++){
            const auto &p=polys[at];if(p.n<3||p.n>8||p.renderer==1)continue;
            bool textured=p.renderer&2;if(textured&&(!p.texw||!p.texh))continue;
            if(p.texlod<-32768||p.texlod>32767)return false;
            int x0=std::max(0,int(p.vp[0])*f.scale),x1=std::min(width,(int(p.vp[1])+1)*f.scale);
            int y0=std::max(0,int(p.vp[2])*f.scale),y1=std::min(height,(int(p.vp[3])+1)*f.scale);
            if(x1<=x0||y1<=y0)continue;
            int max_level=textured?batch_packets[at].max_level:0;
            batch_material material{};
            material.info0[0]=textured;material.info0[1]=p.renderer&1;material.info0[2]=p.checker;
            material.depth=float((double(at)+1)/(count+1)*2-1);
            material.info1[0]=p.texlod;material.info1[3]=p.utexminlod;
            if(textured){unsigned row=rows[at];material.palette[0]=palette_x_attribute(row);material.palette[1]=palette_y(row);}
            if(owners){for(int i=0;i<4;i++)material.colour[i]=(at>>(8*i))&255;}
            else {material.colour[0]=(p.colour>>16)&255;material.colour[1]=(p.colour>>8)&255;material.colour[2]=p.colour&255;material.colour[3]=255;}
            const bool power_of_two=textured&&!(p.texw&(p.texw-1))&&!(p.texh&(p.texh-1));
            const float inverse_w=textured?1.f/p.texw:0,inverse_h=textured?1.f/p.texh:0;
            auto append=[&](const vf2_tiles::vertex &source,const std::array<float,8> &map,std::vector<batch_vertex> &bucket){
                batch_vertex v{};v.material=material;v.position_q[0]=source.p.x;v.position_q[1]=source.p.y;v.position_q[2]=source.p.q;
                if(textured){
                    float uv[2]={(power_of_two?source.p.u*inverse_w:source.p.u/p.texw)/source.p.q,(power_of_two?source.p.v*inverse_h:source.p.v/p.texh)/source.p.q};
                    for(int i=0;i<2;i++){
                        float sa=source.a[i]*map[i],ba=source.a[i+2]*map[i]+map[i+2];
                        float sb=source.b[i]*map[i+4],bb=source.b[i+2]*map[i+4]+map[i+6];
                        v.uv0[i]=uv[i]*sa+ba;v.uv1[i]=uv[i]*sb+bb;
                    }
                }
                bucket.push_back(v);
            };
            if(textured){
                for(int band=-1;band<=max_level;band++){
                    const auto &prepared=batch_packets[at];
                    const auto *indexed=frame_indexed?prepared.indexed_at(band):nullptr;
                    if(frame_indexed&&(!indexed||indexed->indices.empty()))continue;
                    const auto &vertices=indexed?indexed->vertices:prepared.bands[band+1];
                    if(indexed?!indexed->vertex_count():vertices.empty())continue;
                    const bool native_repeat=indexed&&indexed->native_repeat;
                    vf2_batch::tile a,b;
                    if(!batch_atlas.get_pair(rams,p,band,max_level,native_repeat,a,b))return false;
                    material.info1[1]=band;material.info1[2]=band<0?(p.utex?2:0):(band<max_level?1:0);
                    std::array<float,8> map={a.transform[0],a.transform[1],a.transform[2],a.transform[3],
                        b.transform[0],b.transform[1],b.transform[2],b.transform[3]};
                    const uint64_t key=(uint64_t(a.page)<<32)|b.page;
                    auto &group=batch_buckets[key];auto &bucket=group.vertices;
                    const size_t first=bucket.size();
                    if(indexed&&!indexed->ordinary.empty()){
                        for(const auto &v:indexed->ordinary)append(v.expand(),map,bucket);
                    }else for(const auto &v:vertices)append(v,map,bucket);
                    if(frame_indexed){
                        auto &indices=group.indices;
                        for(uint32_t index:indexed->indices)indices.push_back(uint32_t(first)+index);
                    }
                }
            }else{
                std::vector<vf2_tiles::point> polygon;
                for(int i=0;i<p.n;i++)polygon.push_back({p.x[i],p.y[i],1,0,0});
                polygon=vf2_tiles::clip(std::move(polygon),[&](const auto &v){return double(v.x)-double(x0)/f.scale;});
                polygon=vf2_tiles::clip(std::move(polygon),[&](const auto &v){return double(x1)/f.scale-v.x;});
                polygon=vf2_tiles::clip(std::move(polygon),[&](const auto &v){return double(v.y)-double(y0)/f.scale;});
                polygon=vf2_tiles::clip(std::move(polygon),[&](const auto &v){return double(y1)/f.scale-v.y;});
                auto &group=batch_buckets[0];auto &bucket=group.vertices;
                if(frame_indexed&&polygon.size()>=3){
                    const uint32_t first=uint32_t(bucket.size());
                    for(const auto &point:polygon){vf2_tiles::vertex v{};v.p=point;append(v,{},bucket);}
                    auto &indices=group.indices;
                    for(size_t i=1;i+1<polygon.size();i++)indices.insert(indices.end(),{first,first+uint32_t(i),first+uint32_t(i+1)});
                }else if(!frame_indexed)for(size_t i=1;i+1<polygon.size();i++)for(const auto &point:{polygon[0],polygon[i],polygon[i+1]}){vf2_tiles::vertex v{};v.p=point;append(v,{},bucket);}
            }
        }
        for(const auto &bucket:batch_buckets){
            const auto &vertices=bucket.second.vertices;
            if(vertices.empty())continue;
            if(frame_indexed){
                const auto &indices=bucket.second.indices;
                if(indices.empty())continue;
                if(batch_vertices.size()+vertices.size()>65536){
                    index_overflow=true;return false;
                }
                batch_draws.push_back({unsigned(bucket.first>>32),unsigned(bucket.first),unsigned(batch_indices.size()),unsigned(indices.size())});
                for(uint32_t index:indices)batch_indices.push_back(uint16_t(batch_vertices.size()+index));
            }else batch_draws.push_back({unsigned(bucket.first>>32),unsigned(bucket.first),unsigned(batch_vertices.size()),unsigned(vertices.size())});
            batch_vertices.insert(batch_vertices.end(),vertices.begin(),vertices.end());
        }
        batch_vertex_count=batch_vertices.size();batch_index_count=batch_indices.size();
        return true;
    }
    const gpu3d_mesh_polygon *mesh_for(const gpu3d_geometry_frame &scene,size_t at,
            const gpu3d_poly &projected,const direct_work &work,int scale){
        const uint32_t source=scene.mesh_projected_sources[at];
        if(source==gpu3d_mesh_polygon::invalid||work.n!=projected.n||work.ni!=size_t(3*(projected.n-2)))
            return nullptr;
        const auto &mesh=scene.mesh_polygons[source];
        for(unsigned v=0;v<projected.n;++v){
            const auto &point=work.vertices[v].p;
            if(point.x!=projected.x[v]||point.y!=projected.y[v]||point.q!=projected.z[v]||
                point.u!=projected.u[v]||point.v!=projected.v[v])
                return nullptr;
        }
        const auto &r=scene.compact_polygons[source];
        const auto &obj=scene.mesh_objects[mesh.object];
        const auto center=std::array<float,2>{r.center[0],r.center[1]};
        if(mesh_ready[mesh.object]&&mesh_centers[mesh.object]!=center)return nullptr;
        if(!mesh_ready[mesh.object]){
            const auto &m=obj.matrix;
            const float sx=2.f*scale/width,sy=2.f*scale/height;
            float *rows=mesh_rows[mesh.object/64].data()+12*(mesh.object%64);
            for(unsigned column=0;column<4;++column){
                const auto i=column*3;
                rows[column]=m[i]*obj.focus[0]*sx+m[i+2]*(center[0]*sx-1);
                rows[4+column]=m[i+1]*obj.focus[1]*sy+m[i+2]*(1-center[1]*sy);
                rows[8+column]=m[i+2];
            }
            mesh_ready[mesh.object]=1;
            mesh_centers[mesh.object]=center;
        }
        return &mesh;
    }

    bool prepare_batch_direct(const gpu3d_poly *polys,size_t count,const gpu3d_frame &f,const std::vector<unsigned> &rows,bool owners){
        // Resolve atlas uploads on the GL thread and assign disjoint output
        // spans. Workers can then pack directly into the final VBO/IBO arrays.
        // Reuse descriptors and their flat-polygon storage between frames.
        index_overflow=false;size_t job_count=0;batch_draws.clear();batch_modes.clear();
        for(auto &entry:direct_buckets)entry.second={};
        draw_maps.clear();
        draw_maps.emplace_back();
        const auto add_map=[&](const vf2_batch::tile &a,const vf2_batch::tile &b){
            const size_t slot=draw_maps.size();
            draw_maps.push_back({a.transform[0],a.transform[1],a.transform[2],a.transform[3],
                                 b.transform[0],b.transform[1],b.transform[2],b.transform[3]});
            return slot;
        };
        static const bool plan_fail_stats=std::getenv("VF2_PLAN_FAIL_STATS")!=nullptr;
        if(count&&!polys&&(!packets_ready||!draw_lists_ready)){
            if(plan_fail_stats)std::fprintf(stderr,"Plan fail 1 (lists not ready): frame %llu packets_ready=%d draw_lists_ready=%d count=%zu\n",
                (unsigned long long)painted_frames+1,packets_ready,draw_lists_ready,count);
            return false;
        }
        if(!packets_ready)prepare_packets(polys,count,f);
        packets_ready=false;
        const auto *scene=mesh_render&&frame_indexed&&compact_packets?f.mesh_scene:nullptr;
        if(scene&&scene->mesh_projected_sources.size()!=count)scene=nullptr;
        std::unordered_map<uint64_t,uint64_t,vf2_batch::tile_hash> mixed_keys;
        const bool choose_modes=specialize_batches&&!owners&&!scene;
        static const bool textured_modes=std::getenv("VF2_TEXTURED_TRANSPARENT_SHADER")!=nullptr;
        bool use_compact=compact_meta;
        const bool force_wide=test_compact_fallback&&painted_frames%2!=0;
        const auto append_to_bucket=[&](draw_work &work,uint64_t key,direct_bucket *cached=nullptr){
            if(use_compact&&work.n&&(force_wide||!can_compact(work.material)))use_compact=false;
            auto &bucket=cached?*cached:direct_buckets[key];
            work.bucket=&bucket;
            work.first_vertex=bucket.vertices;work.first_index=bucket.indices;
            bucket.vertices+=work.n;bucket.indices+=work.ni;
            if(choose_modes&&work.n){
                const auto &m=work.material;
                const unsigned general=(!m.info0[0]||m.info0[1]||m.info0[2])?1:0;
                bucket.features|=general|(m.info1[2]?2:0)|(m.info0[1]?4:0)|((!m.info0[0]||m.info0[2])?8:0);
            }
        };
        if(scene){
            mesh_rows.resize((scene->mesh_objects.size()+63)/64);
            mesh_ready.assign(scene->mesh_objects.size(),0);
            mesh_centers.resize(scene->mesh_objects.size());
        }
        const bool use_worker_jobs=draw_lists_ready&&!owners;
        if(use_worker_jobs){
            constexpr size_t pair_cache_size=512;
            static const bool cache_pairs=std::getenv("VF2_ATLAS_PAIR_CACHE")!=nullptr;
            static const bool cache_stats=std::getenv("VF2_ATLAS_PAIR_CACHE_STATS")!=nullptr;
            static const bool cache_buckets=std::getenv("DAYTONA_ATLAS_PAIR_BUCKET")!=nullptr;
            // Atlas placements stay stable during this plan. Start with no
            // valid entries on every call, including a retry after a clear.
            std::bitset<pair_cache_size> valid_pairs;
            if(cache_pairs&&atlas_pair_cache.empty())atlas_pair_cache.resize(pair_cache_size);
            size_t pair_requests=0,pair_hits=0;
            ++prepared_draw_frames;
            for(auto &range:prepared_draws){
                range.first_job=job_count;
                job_count+=range.draws.size();
                for(size_t at=0;at<range.draws.size();++at){
                    auto &draw=range.draws[at];
                    auto &work=draw.work;
                    const auto &texture=draw.texture;
                    uint64_t key=0;
                    atlas_pair_entry *entry=nullptr;
                    bool hit=false;
                    if(texture.texw){
                        const unsigned row=rows[draw.polygon];
                        work.material.palette[0]=palette_x_attribute(row);work.material.palette[1]=palette_y(row);
                        unsigned slot=0;
                        if(cache_pairs){
                            const uint32_t hash=(uint32_t(texture.texx)*0x9e3779b1u)^(uint32_t(texture.texy)*0x85ebca6bu)^
                                (uint32_t(draw.band+1)*0xc2b2ae35u)^(uint32_t(texture.texw)*0x27d4eb2du+texture.texh);
                            slot=hash>>23;
                            entry=&atlas_pair_cache[slot];
                            hit=valid_pairs[slot]&&entry->band==draw.band&&entry->max_level==draw.max_level&&
                                entry->native_repeat==draw.native_repeat&&entry->texture==texture;
                        }
                        if(hit){
                            work.map_slot=entry->map_slot;
                            key=entry->page_key;
                        }else{
                            vf2_batch::tile a,b;
                            if(!batch_atlas.get_pair(rams,texture,draw.band,draw.max_level,draw.native_repeat,a,b)){
                                if(plan_fail_stats)std::fprintf(stderr,"Plan fail 2 (get_pair worker): frame %llu at=%zu/%zu band=%d level=%d tex=%ux%u page_count=%zu resident=%zu allocated=%zu/%zu\n",
                                    (unsigned long long)painted_frames+1,at,count,draw.band,draw.max_level,texture.texw,texture.texh,
                                    batch_atlas.page_count(),batch_atlas.resident(),batch_atlas.allocated_bytes(),vf2_batch::tile_atlas::capacity_bytes());
                                return false;
                            }
                            work.map_slot=add_map(a,b);
                            key=(uint64_t(a.page)<<32)|b.page;
                            if(entry){
                                *entry={texture,draw.band,draw.max_level,draw.native_repeat,work.map_slot,key};
                                valid_pairs[slot]=true;
                            }
                        }
                        if(cache_stats){++pair_requests;pair_hits+=hit;}
                        if(cell_vertex_stats){
                            if(shared_packets_active){
                                const auto source=range.locations[at].source;
                                if(source==packet_location::ordinary)ordinary_cell_vertices+=work.n;
                                if(source==packet_location::full)full_cell_vertices+=work.n;
                            }else if(!work.fan){
                                if(work.ordinary)ordinary_cell_vertices+=work.n;
                                else full_cell_vertices+=work.n;
                            }
                        }
                    }
                    // Hash nodes keep their addresses across insertions.
                    // The per-plan valid bits prevent reuse after a new plan.
                    append_to_bucket(work,key,cache_buckets&&hit?entry->cached_bucket:nullptr);
                    if(cache_buckets&&entry&&!hit)entry->cached_bucket=work.bucket;
                }
            }
            prepared_draw_jobs+=job_count;
            if(cache_stats)std::fprintf(stderr,"Atlas pair cache: %zu %zu\n",pair_requests,pair_hits);
        }else for(size_t at=0;at<count;++at){
            const auto &p=polys[at];if(p.n<3||p.n>8||p.renderer==1)continue;
            const bool textured=p.renderer&2;if(textured&&(!p.texw||!p.texh))continue;
            if(p.texlod<-32768||p.texlod>32767){
                if(plan_fail_stats)std::fprintf(stderr,"Plan fail 3 (texlod): frame %llu at=%zu texlod=%d\n",
                    (unsigned long long)painted_frames+1,at,int(p.texlod));
                return false;
            }
            const int x0=std::max(0,int(p.vp[0])*f.scale),x1=std::min(width,(int(p.vp[1])+1)*f.scale);
            const int y0=std::max(0,int(p.vp[2])*f.scale),y1=std::min(height,(int(p.vp[3])+1)*f.scale);
            if(x1<=x0||y1<=y0)continue;
            batch_material material{};
            material.info0[0]=textured;material.info0[1]=p.renderer&1;material.info0[2]=p.checker;
            material.depth=float((double(at)+1)/(count+1)*2-1);
            material.info1[0]=p.texlod;material.info1[3]=p.utexminlod;
            if(textured){const unsigned row=rows[at];material.palette[0]=palette_x_attribute(row);material.palette[1]=palette_y(row);}
            if(owners){for(int i=0;i<4;++i)material.colour[i]=(at>>(8*i))&255;}
            else{material.colour[0]=(p.colour>>16)&255;material.colour[1]=(p.colour>>8)&255;material.colour[2]=p.colour&255;material.colour[3]=255;}
            const int max_level=textured?batch_packets[at].max_level:0;
            uint32_t active=textured&&compact_packets?batch_packets[at].active_bands:0;
            for(int band=-1;band<=(textured?max_level:-1);++band){
                if(textured&&compact_packets){
                    if(!active)break;
                    band=int(std::countr_zero(active))-1;active&=active-1;
                }
                const auto *indexed=textured&&frame_indexed?batch_packets[at].indexed_at(band):nullptr;
                if(textured&&(frame_indexed?(!indexed||indexed->indices.empty()):batch_packets[at].bands[band+1].empty()))continue;
                if(job_count==direct_jobs.size())direct_jobs.emplace_back();
                direct_work &work=direct_jobs[job_count++];work.material=material;work.map_slot=0;
                work.texw=work.texh=0;work.n=work.ni=0;work.mesh=nullptr;work.fan=nullptr;work.ordinary=nullptr;
                uint64_t key=0;
                if(textured){
                    const auto &prepared=batch_packets[at];
                    const auto &vertices=indexed?indexed->vertices:prepared.bands[band+1];
                    work.vertices=vertices.data();work.n=vertices.size();
                    if(indexed)work.set_packet(*indexed);
                    const bool native_repeat=indexed&&indexed->native_repeat;
                    vf2_batch::tile a,b;
                    if(!batch_atlas.get_pair(rams,p,band,max_level,native_repeat,a,b)){
                        if(plan_fail_stats)std::fprintf(stderr,"Plan fail 4 (get_pair serial): frame %llu at=%zu/%zu band=%d level=%d tex=%ux%u page_count=%zu resident=%zu allocated=%zu/%zu\n",
                            (unsigned long long)painted_frames+1,at,count,band,max_level,p.texw,p.texh,
                            batch_atlas.page_count(),batch_atlas.resident(),batch_atlas.allocated_bytes(),vf2_batch::tile_atlas::capacity_bytes());
                        return false;
                    }
                    work.material.info1[1]=band;work.material.info1[2]=band<0?(p.utex?2:0):(band<max_level?1:0);
                    work.map_slot=add_map(a,b);
                    work.texw=p.texw;work.texh=p.texh;
                    work.power_of_two=!(p.texw&(p.texw-1))&&!(p.texh&(p.texh-1));
                    key=(uint64_t(a.page)<<32)|b.page;
                    if(scene&&std::has_single_bit(prepared.active_bands)){
                        work.mesh=mesh_for(*scene,at,p,work,f.scale);
                        if(work.mesh){
                            work.material.info0[3]=uint8_t(work.mesh->object%64+1);
                            const uint64_t group_key=key|(uint64_t(work.mesh->object/64+1)<<48);
                            mixed_keys.try_emplace(key,group_key);
                            key=group_key;
                        }
                    }
                }else{
                    prepare_flat_packet(work.flat,work.flat_indices,p,x0,y0,x1,y1,f.scale);
                    work.vertices=work.flat.data();work.n=work.flat.size();
                    work.indices=work.flat_indices.data();work.ni=work.flat_indices.size();
                }
                if(scene)work.bucket_key=key;
                else append_to_bucket(work,key);
            }
        }
        if(scene){
            for(size_t j=0;j<job_count;++j){
                auto &work=direct_jobs[j];
                uint64_t key=work.bucket_key;
                if(!work.mesh){
                    const auto mixed=mixed_keys.find(key);
                    if(mixed!=mixed_keys.end())key=mixed->second;
                }else ++mesh_drawn_polygons;
                auto &bucket=direct_buckets[key];
                work.bucket=&bucket;
                work.first_vertex=bucket.vertices;work.first_index=bucket.indices;
                bucket.vertices+=work.n;bucket.indices+=work.ni;
            }
        }
        size_t nv=0,ni=0;
        for(auto &entry:direct_buckets){
            auto &bucket=entry.second;if(!bucket.vertices)continue;
            if(frame_indexed){
                if(!bucket.indices)continue;
                if(nv+bucket.vertices>65536){
                    index_overflow=true;
                    if(plan_fail_stats)std::fprintf(stderr,"Plan fail 5 (index overflow): frame %llu nv=%zu bucket_vertices=%zu count=%zu draws=%zu\n",
                        (unsigned long long)painted_frames+1,nv,bucket.vertices,count,batch_draws.size());
                    return false;
                }
                batch_draws.push_back({unsigned((entry.first>>32)&0xffff),unsigned(entry.first),unsigned(ni),unsigned(bucket.indices),unsigned(entry.first>>48)});
            }else batch_draws.push_back({unsigned(entry.first>>32),unsigned(entry.first),unsigned(nv),unsigned(bucket.vertices)});
            if(choose_modes){
                const unsigned f=entry.second.features;
                // Mode 3 is the textured branch without the untextured and
                // checker tests, so buckets that are wholly textured and
                // checker-free use it whether or not they are transparent.
                batch_modes.push_back(textured_modes?(f&8?0:(f&4?3:(f&2?1:2))):(f&1?0:(f&2?1:2)));
            }
            bucket.first_vertex=nv;bucket.first_index=ni;nv+=bucket.vertices;ni+=bucket.indices;
        }
        compact_ready=use_compact;
        if(compact_ready){
            if(compact_vertices.size()<nv)compact_vertices.resize(nv);
        }else if(batch_vertices.size()<nv)batch_vertices.resize(nv);
        if(batch_indices.size()<ni)batch_indices.resize(ni);
        batch_vertex_count=nv;batch_index_count=ni;
        // No GL calls or container mutations below: each job owns its spans.
        const auto pack_job=[&](auto &vertices,const draw_work &work,const gpu3d_mesh_polygon *mesh){
            if(!work.n)return;
            const auto &map=draw_maps[work.map_slot];
            const bool textured=work.texw!=0;
            const float inverse_w=textured?1.f/work.texw:0,inverse_h=textured?1.f/work.texh:0;
            const auto *source=work.vertices;
            auto *destination=vertices.data()+work.bucket->first_vertex+work.first_vertex;
            const auto &material=[&](){
                if constexpr(sizeof(*destination)==sizeof(compact_vertex))return compact(work.material);
                else return work.material;
            }();
            const auto copy_vertex=[&](size_t i,const vf2_tiles::point &p,const float *a,const float *b){
                auto &v=destination[i];
                v.material=material;
                if(mesh)std::copy_n(mesh->position[i].data(),3,v.position_q);
                else{v.position_q[0]=p.x;v.position_q[1]=p.y;v.position_q[2]=p.q;}
                if(textured){
                    float uv[2]={(work.power_of_two?p.u*inverse_w:p.u/work.texw)/p.q,(work.power_of_two?p.v*inverse_h:p.v/work.texh)/p.q};
                    for(int axis=0;axis<2;++axis){
                        float sa=a[axis]*map[axis],ba=a[axis+2]*map[axis]+map[axis+2];
                        float sb=b[axis]*map[axis+4],bb=b[axis+2]*map[axis+4]+map[axis+6];
                        v.uv0[axis]=uv[axis]*sa+ba;v.uv1[axis]=uv[axis]*sb+bb;
                    }
                }else{
                    v.uv0[0]=v.uv0[1]=v.uv1[0]=v.uv1[1]=0;
                }
            };
            if(const auto *fan=work.fan){
                if(fan->points){
                    for(size_t i=0;i<work.n;++i)copy_vertex(i,fan->points[i],fan->a,fan->b);
                }else for(size_t i=0;i<work.n;++i)copy_vertex(i,projected_point(*fan->polygon,i),fan->a,fan->b);
            }else if(const auto *ordinary=work.ordinary)
                for(size_t i=0;i<work.n;++i){
                    const auto &v=ordinary[i];
                    const float transform[]={1,1,-float(v.cell_x),-float(v.cell_y)};
                    copy_vertex(i,v.p,transform,transform);
                }
            else
                for(size_t i=0;i<work.n;++i)copy_vertex(i,source[i].p,source[i].a,source[i].b);
            if(frame_indexed){
                const auto *indices=work.indices;
                auto *out=batch_indices.data()+work.bucket->first_index+work.first_index;
                const auto first_vertex=work.bucket->first_vertex+work.first_vertex;
                for(size_t i=0;i<work.ni;++i)out[i]=uint16_t(first_vertex+indices[i]);
            }
        };
        const auto pack_into=[&](auto &vertices,int begin,int end){
            if(use_worker_jobs){
                for(auto &range:prepared_draws){
                    const int base=int(range.first_job);
                    const int first=std::max(0,begin-base);
                    const int last=std::min(end-base,int(range.draws.size()));
                    for(int j=first;j<last;++j){
                        auto &work=range.draws[j].work;
                        if(shared_packets_active)bind_shared_packet(range,j,work);
                        pack_job(vertices,work,nullptr);
                    }
                }
            }else for(int j=begin;j<end;++j)pack_job(vertices,direct_jobs[j],direct_jobs[j].mesh);
        };
        const auto pack_range=[&](int begin,int end){
            if(compact_ready)pack_into(compact_vertices,begin,end);
            else pack_into(batch_vertices,begin,end);
        };
        const auto pack_start=timing_packets?clock::now():clock::time_point{};
        static const bool dual_pack=std::getenv("VF2_DUAL_PACK")!=nullptr;
        if(dual_pack&&job_count>=8&&row_pool::get().threads()>1){
            constexpr int ranges=32;
            const int count=int(job_count);
            // The pool workers are idle between packet preparation and this
            // phase, so pack on the caller and every one of them instead of
            // starting one helper thread.
            parallel_tasks(ranges,[&](int part){
                pack_range(count*part/ranges,count*(part+1)/ranges);
            });
        }else parallel_rows(int(job_count),pack_range,4);
        if(timing_packets)current_pack_ms+=phase_ms(pack_start,clock::now());
        return true;
    }
    bool prepare_batch(const gpu3d_poly *polys,size_t count,const gpu3d_frame &f,const std::vector<unsigned> &rows,bool owners){
        static const bool direct=std::getenv("VF2_GLES_DIRECT_PACK")!=nullptr;
        static const bool check=std::getenv("VF2_CHECK_DIRECT_PACK")!=nullptr;
        compact_ready=false;
        if(!direct&&!check)return prepare_batch_reference(polys,count,f,rows,owners);
        if(!prepare_batch_direct(polys,count,f,rows,owners))return false;
        static const bool hash_draw_stream=std::getenv("VF2_DRAW_STREAM_HASH")!=nullptr;
        if(hash_draw_stream){
            uint64_t hash=14695981039346656037ull;
            const auto add=[&](const auto &items){
                const auto *bytes=reinterpret_cast<const unsigned char *>(items.data());
                const auto size=items.size()*sizeof(items.front());
                for(size_t i=0;i<size;++i)hash=(hash^bytes[i])*1099511628211ull;
            };
            const auto add_n=[&](const auto *items,size_t count){
                const auto *bytes=reinterpret_cast<const unsigned char *>(items);
                const auto size=count*sizeof(*items);
                for(size_t i=0;i<size;++i)hash=(hash^bytes[i])*1099511628211ull;
            };
            if(hash_palette_colours){
                for(size_t vi=0;vi<batch_vertex_count;++vi){
                    const auto &vertex=batch_vertices[vi];
                    auto normalized=vertex;
                    if(vertex.material.info0[0]){
                        const unsigned x=vertex.material.palette[0], y=vertex.material.palette[1];
                        const unsigned row=(x/512)*palettes_per_bank+y*2+((x/256)&1u);
                        if(row>=palette_count)return false;
                        const uint64_t shade=palette_colour_hashes[row];
                        hash_bytes(hash,&shade,sizeof(shade));
                    }
                    normalized.material.palette[0]=normalized.material.palette[1]=0;
                    hash_bytes(hash,&normalized,sizeof(normalized));
                }
            }else add_n(batch_vertices.data(),batch_vertex_count);
            add_n(batch_indices.data(),batch_index_count);add(batch_draws);
            std::fprintf(stderr,"GPU draw stream: %016llx %zu %zu %zu\n",
                         (unsigned long long)hash,batch_vertex_count,batch_index_count,batch_draws.size());
        }
        if(check){
            std::vector<batch_vertex> vertices(batch_vertices.begin(),batch_vertices.begin()+batch_vertex_count);
            std::vector<uint16_t> indices(batch_indices.begin(),batch_indices.begin()+batch_index_count);
            auto draws=batch_draws;
            if(!prepare_batch_reference(polys,count,f,rows,owners))return false;
            const bool same=vertices.size()==batch_vertex_count&&indices.size()==batch_index_count&&draws.size()==batch_draws.size()&&
                (vertices.empty()||std::memcmp(vertices.data(),batch_vertices.data(),vertices.size()*sizeof(batch_vertex))==0)&&
                (draws.empty()||std::memcmp(draws.data(),batch_draws.data(),draws.size()*sizeof(batch_draw))==0);
            if(!same){std::fprintf(stderr,"Direct vertex packing differs from reference\n");std::abort();}
        }
        return true;
    }
    mutable double submit_phase_ms[8]{};
    mutable unsigned long long submit_phase_calls[8]{};
    mutable double draw_call_ms=0,draw_call_max_ms=0;
    mutable unsigned long long draw_call_count=0,draw_call_indices=0;
    const bool trace_slow_submits=std::getenv("VF2_TRACE_SLOW_SUBMITS")!=nullptr;
    void trace_submit_phase(const char *phase,std::chrono::steady_clock::time_point start)const{
        if(!trace_slow_submits)return;
        const double elapsed=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        // Accumulate a per-phase average: the 2 ms totals hide behind 8 ms
        // outliers when only the slow frames are printed.
        static const char *names[8]={"target-underlay","state-vertices","draw-calls","overlay-finish","texture-atlas","palette","",""};
        for(unsigned i=0;i<8;++i)if(std::strcmp(names[i],phase)==0){submit_phase_ms[i]+=elapsed;++submit_phase_calls[i];break;}
        if(elapsed>=8)
            std::fprintf(stderr,"Slow 3D submit: draw %llu phase %s %.3f ms\n",
                         (unsigned long long)painted_frames+1,phase,elapsed);
    }

    bool paint_underlay(const gpu3d_frame &f){
#ifdef MODEL2_ORIGINAL
        static const bool combine_clear=std::getenv("DAYTONA_COMBINE_UNDERLAY_CLEAR")!=nullptr;
#else
        constexpr bool combine_clear=false;
#endif
        // Clear both attachments before the first draw to the scene target.
        glDepthMask(GL_TRUE);
        glClearDepthf(1);
        glDisable(GL_SCISSOR_TEST);
        if(!combine_clear)glClear(GL_DEPTH_BUFFER_BIT);
        return f.underlay(f.layers,combine_clear);
    }

    bool paint_batch(const gpu3d_frame &f,int target,bool owners){
        auto phase=trace_slow_submits?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
        GLint prior;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&prior);
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);attach_color(target);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth_buffer);
        if (!verify_edge_target()) {
            glBindFramebuffer(GL_FRAMEBUFFER, prior);
            return false;
        }
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){glBindFramebuffer(GL_FRAMEBUFFER,prior);return false;}
        if(!owners&&f.underlay&&!paint_underlay(f)){glBindFramebuffer(GL_FRAMEBUFFER,prior);return false;}
        trace_submit_phase("target-underlay",phase);
        if(trace_slow_submits)phase=std::chrono::steady_clock::now();
        glViewport(0,0,width,height);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);glClearDepthf(1);
        glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glDisable(GL_DITHER);
        glClearColor(owners?1.f:0.f,owners?1.f:0.f,owners?1.f:0.f,owners?1.f:0.f);if(owners||!f.underlay)glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        const auto activate=[&](unsigned mode){
            auto &p=compact_ready?compact_programs[mode]:batch_programs[mode];
            glUseProgram(p.name);
            if(!specialize_batches){
                glUniform2f(p.size,width,height);glUniform2f(p.scale,float(width)/496.f,float(height)/384.f);
                glUniform1f(p.height,height);glUniform1f(p.owners,owners);
                glUniform1i(p.atlas_a,0);glUniform1i(p.atlas_b,1);glUniform1i(p.palette,2);
                return;
            }
            // Uniform storage belongs to the linked program and survives
            // switches. Only the renderer owns these handles.
            if(p.configured_width!=width||p.configured_height!=height){
                glUniform2f(p.size,width,height);glUniform1f(p.height,height);
                // The shader normalises arcade coordinates: the picture fills
                // the target whatever its size (integer scaling or not).
                glUniform2f(p.scale,float(width)/496.f,float(height)/384.f);
                p.configured_width=width;p.configured_height=height;
            }
            if(p.owners>=0&&p.configured_owner!=int(owners)){
                glUniform1f(p.owners,owners);p.configured_owner=int(owners);
            }
        };
        const bool choose_modes=!owners&&!batch_modes.empty();
        unsigned active_mode=choose_modes?batch_modes.front():0;
        activate(active_mode);
        glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_2D,palette_atlas);
        if(compact_meta){
            if(compact_ready)++compact_frames;
            else ++wide_fallback_frames;
        }
        const auto *vertices=compact_ready?static_cast<const void*>(compact_vertices.data()):batch_vertices.data();
        const size_t stride=compact_ready?sizeof(compact_vertex):sizeof(batch_vertex);
        if(client_arrays){
            glBindBuffer(GL_ARRAY_BUFFER,0);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
        }else{
            glBindBuffer(GL_ARRAY_BUFFER,tile_vbo);
            glBufferData(GL_ARRAY_BUFFER,batch_vertex_count*stride,vertices,GL_STREAM_DRAW);
            if(frame_indexed){glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,tile_ibo);glBufferData(GL_ELEMENT_ARRAY_BUFFER,batch_index_count*sizeof(uint16_t),batch_indices.data(),GL_STREAM_DRAW);}
        }
        const unsigned sizes[8]={3,2,2,1,4,4,2,4},offsets[8]={0,12,20,28,32,36,44,48};
        const GLenum types[8]={GL_FLOAT,GL_FLOAT,GL_FLOAT,GL_FLOAT,GL_UNSIGNED_BYTE,GL_SHORT,GL_UNSIGNED_SHORT,GL_UNSIGNED_BYTE};
        const unsigned compact_sizes[8]={3,2,2,1,1,1,1,3},compact_offsets[8]={0,12,20,28,36,32,34,37};
        for(unsigned i=0;i<8;++i){
            glEnableVertexAttribArray(i);
            const auto offset=compact_ready?compact_offsets[i]:offsets[i];
            glVertexAttribPointer(i,compact_ready?compact_sizes[i]:sizes[i],types[i],i==7?GL_TRUE:GL_FALSE,
                stride,reinterpret_cast<void*>((client_arrays?reinterpret_cast<uintptr_t>(vertices):0)+offset));
        }
        // Ceiling probe: skipping aUV1 (8 bytes) shows how submit scales with
        // the per-vertex byte count on the client-array path.
        static const bool skip_uv1=std::getenv("VF2_SKIP_UV1")!=nullptr;
        if(skip_uv1)glDisableVertexAttribArray(2);
        trace_submit_phase("state-vertices",phase);
        if(trace_slow_submits)phase=std::chrono::steady_clock::now();
        unsigned uploaded_mesh_group=0;
        static const bool cache_texture_bindings=std::getenv("VF2_CACHE_BATCH_TEXTURE_BINDS")!=nullptr;
        const bool has_atlas=batch_atlas.page_count()!=0;
        const unsigned unbound=std::numeric_limits<unsigned>::max();
        unsigned page_a=unbound,page_b=unbound;
        GLenum active_unit=GL_TEXTURE2;
        const auto bind_page=[&](GLenum unit,unsigned &current,unsigned page){
            if(cache_texture_bindings&&current==page)return;
            if(!cache_texture_bindings||active_unit!=unit)glActiveTexture(unit);
            glBindTexture(GL_TEXTURE_2D,batch_atlas.handle(page));
            current=page;
            active_unit=unit;
        };
        for(size_t i=0;i<batch_draws.size();++i){
            const auto &draw=batch_draws[i];
            if(choose_modes){
                const unsigned mode=batch_modes[i];
                if(mode!=active_mode){activate(mode);active_mode=mode;}
                ++mode_draws[mode];mode_indices[mode]+=draw.count;
            }
            if(draw.mesh_group&&draw.mesh_group!=uploaded_mesh_group){
                glUniform4fv(mesh_rows_location,192,mesh_rows[draw.mesh_group-1].data());
                uploaded_mesh_group=draw.mesh_group;
            }
            if(has_atlas){
                bind_page(GL_TEXTURE0,page_a,draw.page_a);
                bind_page(GL_TEXTURE1,page_b,draw.page_b);
            }
            const auto draw_begin=trace_slow_submits?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
            if(frame_indexed)glDrawElements(GL_TRIANGLES,draw.count,GL_UNSIGNED_SHORT,reinterpret_cast<void*>((client_arrays?reinterpret_cast<uintptr_t>(batch_indices.data()):0)+draw.first*sizeof(uint16_t)));
            else glDrawArrays(GL_TRIANGLES,draw.first,draw.count);
            if(trace_slow_submits){
                const double d=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-draw_begin).count();
                draw_call_ms+=d;if(d>draw_call_max_ms)draw_call_max_ms=d;
                draw_call_indices+=draw.count;++draw_call_count;
            }
        }
        if(cache_texture_bindings&&has_atlas&&!batch_draws.empty()&&active_unit!=GL_TEXTURE1)
            glActiveTexture(GL_TEXTURE1);
        trace_submit_phase("draw-calls",phase);
        if(trace_slow_submits)phase=std::chrono::steady_clock::now();
        for(unsigned i=0;i<8;i++)glDisableVertexAttribArray(i);
        glDisable(GL_DEPTH_TEST);glBindBuffer(GL_ARRAY_BUFFER,0);
        if(frame_indexed)glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);
        if(!owners&&f.overlay&&!f.overlay(f.layers)){glBindFramebuffer(GL_FRAMEBUFFER,prior);return false;}
        if(discard_framebuffer){const GLenum attachment=GL_DEPTH_ATTACHMENT;discard_framebuffer(GL_FRAMEBUFFER,1,&attachment);}
        glBindFramebuffer(GL_FRAMEBUFFER,prior);pending=true;
        trace_submit_phase("overlay-finish",phase);
        cpu_time=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
        if(profile_palettes){
            const auto &p=palette_stats;
            std::fprintf(stderr,"Palette frame %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %.6f %.6f %u\n",
                         (unsigned long long)painted_frames+1,(unsigned long long)p.probes,
                         (unsigned long long)p.built,(unsigned long long)p.capacity,
                         (unsigned long long)p.luma,(unsigned long long)p.xlat,(unsigned long long)p.gamma,
                         (unsigned long long)p.initial,(unsigned long long)p.upload_calls,
                         (unsigned long long)p.upload_bytes,p.prepare_ms,p.upload_ms,palette_count);
            palette_stats={};
        }
        if(++painted_frames%30==0&&std::getenv("VF2_GLES_STATS"))std::fprintf(stderr,"GLES batch: %zu draws, %zu vertices, %zu indices, %zu tiles, %zu pages, %zu palettes; atlas uploaded %llu bytes, %llu clears\n",batch_draws.size(),batch_vertex_count,batch_index_count,batch_atlas.resident(),batch_atlas.page_count(),size_t(palette_count),(unsigned long long)batch_atlas.uploaded,(unsigned long long)batch_atlas.clears);
        // Per-phase submit average: separates the vertex upload from the draw
        // submission so the 4 ms submit can be attributed.
        if(painted_frames%30==0&&trace_slow_submits){
            static const char *names_print[8]={"target-underlay","state-vertices","draw-calls","overlay-finish","texture-atlas","palette","",""};
            std::fputs("Submit phases:",stderr);
            for(unsigned i=0;i<8;++i)if(submit_phase_calls[i])
                std::fprintf(stderr," %s %.3f",names_print[i],
                    submit_phase_ms[i]/submit_phase_calls[i]);
            std::fputc('\n',stderr);
            if(draw_call_count)
                std::fprintf(stderr,"Draw calls: %llu calls %.3f ms total %.1f us/call max %.1f us, %llu indices %.1f ns/index\n",
                    (unsigned long long)draw_call_count,draw_call_ms,
                    draw_call_ms/draw_call_count*1000.0,draw_call_max_ms*1000.0,
                    (unsigned long long)draw_call_indices,
                    draw_call_indices?draw_call_ms/draw_call_indices*1e6:0.0);
        }
        if(choose_modes&&painted_frames%30==0&&std::getenv("VF2_GLES_STATS"))
            std::fprintf(stderr,"Atlas shader draws/indices: %llu/%llu %llu/%llu %llu/%llu %llu/%llu\n",
                (unsigned long long)mode_draws[0],(unsigned long long)mode_indices[0],
                (unsigned long long)mode_draws[1],(unsigned long long)mode_indices[1],
                (unsigned long long)mode_draws[2],(unsigned long long)mode_indices[2],
                (unsigned long long)mode_draws[3],(unsigned long long)mode_indices[3]);
        if(retained_fans&&painted_frames%30==0&&std::getenv("VF2_GLES_STATS"))
            std::fprintf(stderr,"Retained indexed fans: %llu\n",(unsigned long long)retained_fans);
        if(mesh_render&&painted_frames%30==0)
            std::fprintf(stderr,"Mixed mesh polygons submitted: %llu\n",(unsigned long long)mesh_drawn_polygons);
        if(painted_frames%30==0&&staged_frames&&std::getenv("VF2_GLES_STATS"))std::fprintf(stderr,"Staged renderer CPU planning: %.2f ms/frame over %llu frames\n",staged_cpu_total/staged_frames,(unsigned long long)staged_frames);
        if(painted_frames%30==0&&std::getenv("VF2_GLES_STATS"))std::fprintf(stderr,"GLES atlas validation: %llu page hits, %llu word snapshots, %llu source words read\n",(unsigned long long)batch_atlas.page_hits,(unsigned long long)batch_atlas.source_checks,(unsigned long long)batch_atlas.source_words);
        return glGetError()==GL_NO_ERROR;
    }
    bool paint_staged(const void *identity,bool projected,size_t count,const gpu3d_frame &f,int target,bool owners,bool &painted){
        const bool was_staged=staged_uploads;
        bool planned=staged_plan&&!owners&&projected==staged_projected&&identity==staged_polys&&count==staged_count&&
            f.w==staged_frame.w&&f.h==staged_frame.h&&f.scale==staged_frame.scale&&f.ss==staged_frame.ss&&f.edge==staged_frame.edge&&f.mesh_scene==staged_frame.mesh_scene;
        staged_plan=false;
        if(staged_uploads){
            const auto phase=trace_slow_submits?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
            batch_atlas.defer_uploads(false);release_retired_tiles();
            if(!batch_atlas.flush()){batch_atlas.clear();planned=false;}
            trace_submit_phase("texture-atlas",phase);
            staged_uploads=false;
        }
        painted=planned;
        if(planned){
            const auto phase=trace_slow_submits?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
            upload_palettes();
            trace_submit_phase("palette",phase);
            const bool ok=paint_batch(f,target,owners);cpu_time+=staged_cpu_ms;return ok;
        }
        if(was_staged)packets_ready=false;
        return true;
    }
    bool paint(const gpu3d_poly *polys,size_t count,const gpu3d_frame &f,int target,bool owners) {
        if(target<0||size_t(target)>=targets.size()||!have_rams||f.ss!=1||f.edge||f.w!=width||f.h!=height)return false;
        began=std::chrono::steady_clock::now();
        bool painted=false;
        const bool result=paint_staged(polys,false,count,f,target,owners,painted);
        if(painted)return result;
        if(!packets_ready)frame_indexed=indexed_batches;
        std::vector<unsigned> palette_rows;
        if(!prepare_palettes(polys,count,palette_rows))return false;
        if(use_batches){
            for(int attempt=0;attempt<2;attempt++){
                if(prepare_batch(polys,count,f,palette_rows,owners))return paint_batch(f,target,owners);
                if(index_overflow){
                    // Keep large scenes batched without truncating indices.
                    frame_indexed=false;packets_ready=false;
                    if(prepare_batch(polys,count,f,palette_rows,owners))return paint_batch(f,target,owners);
                }
                batch_atlas.clear();
            }
            if(std::getenv("VF2_GLES_STATS"))std::fprintf(stderr,"GLES batch preparation limit exceeded; using individual tiles\n");
        }
        struct tile_draw {int band,first,count;};
        std::vector<vf2_tiles::vertex> frame_vertices;
        std::vector<tile_draw> frame_draws;
        std::vector<size_t> first_draw;
        if(direct_tiles){
            first_draw.resize(count+1);frame_vertices.reserve(count*12);
            for(size_t at=0;at<count;at++){
                first_draw[at]=frame_draws.size();const auto &p=polys[at];
                if(p.n<3||p.n>8||!(p.renderer&2)||!p.texw||!p.texh)continue;
                int x0=std::max(0,int(p.vp[0])*f.scale),x1=std::min(width,(int(p.vp[1])+1)*f.scale);
                int y0=std::max(0,int(p.vp[2])*f.scale),y1=std::min(height,(int(p.vp[3])+1)*f.scale);
                if(x1<=x0||y1<=y0)continue;
                int max_level=max_texture_mip(p);
                for(int band=-1;band<=max_level;band++){
                    auto vertices=vf2_tiles::packet(p,band,max_level,float(x0)/f.scale,float(y0)/f.scale,float(x1)/f.scale,float(y1)/f.scale);
                    if(vertices.empty())continue;
                    frame_draws.push_back({band,int(frame_vertices.size()),int(vertices.size())});
                    frame_vertices.insert(frame_vertices.end(),vertices.begin(),vertices.end());
                }
            }
            first_draw[count]=frame_draws.size();
            glBindBuffer(GL_ARRAY_BUFFER,tile_vbo);
            glBufferData(GL_ARRAY_BUFFER,frame_vertices.size()*sizeof(vf2_tiles::vertex),frame_vertices.data(),GL_STREAM_DRAW);
        }
        GLint prior;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&prior);
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);attach_color(target);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth_buffer);
        if (!verify_edge_target()) {
            glBindFramebuffer(GL_FRAMEBUFFER, prior);
            return false;
        }
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){glBindFramebuffer(GL_FRAMEBUFFER,prior);return false;}
        if(!owners&&f.underlay&&!paint_underlay(f)){glBindFramebuffer(GL_FRAMEBUFFER,prior);return false;}
        glViewport(0,0,width,height);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LESS);glDepthMask(GL_TRUE);glClearDepthf(1.f);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DITHER);glClearColor(owners?1.f:0.f,owners?1.f:0.f,owners?1.f:0.f,owners?1.f:0.f);if(owners||!f.underlay)glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        glUseProgram(program);glUniform2f(loc.uSize,width,height);glUniform2f(loc.uScale,float(width)/496.f,float(height)/384.f);
        glUniform1f(loc.uHeight,height);glUniform1f(loc.uOwnerMode,owners?1.f:0.f);
        for(int i=0;i<4;i++){glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,sheets[i]);}
        glUniform1i(loc.uSheet00,0);glUniform1i(loc.uSheet01,1);glUniform1i(loc.uSheet10,2);glUniform1i(loc.uSheet11,3);glUniform1i(loc.uPalette,4);
        if(direct_tiles){glUniform1i(loc.uTile0,0);glUniform1i(loc.uTile1,1);glUniform1i(loc.uPalette,2);}
        glActiveTexture(direct_tiles?GL_TEXTURE2:GL_TEXTURE4);glBindTexture(GL_TEXTURE_2D,palette_atlas);
        glBindBuffer(GL_ARRAY_BUFFER,vbo);glEnableVertexAttribArray(0);glEnableVertexAttribArray(1);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(vertex),nullptr);
        glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(vertex),(void*)(2*sizeof(float)));
        glEnable(GL_SCISSOR_TEST);
        // Model 2 sorts entire polygons. Encode that order as a constant depth
        // per polygon, rather than interpolating geometric vertex depth. This
        // preserves its ordering while allowing early rejection of hidden
        // fragments. Discarded checker/transparent pixels do not write depth.
        for(size_t at=0;at<count;at++) {
            const gpu3d_poly &p=polys[at];
            if(p.n<3||p.n>8||p.renderer==1)continue;
            int x0=std::max(0,int(p.vp[0])*f.scale),x1=std::min(width,(int(p.vp[1])+1)*f.scale);
            int y0=std::max(0,int(p.vp[2])*f.scale),y1=std::min(height,(int(p.vp[3])+1)*f.scale);
            if(x1<=x0||y1<=y0)continue;
            glScissor(x0,height-y1,x1-x0,y1-y0);
            glUniform1f(loc.uSortDepth,float((double(at)+1)/(count+1)*2-1));
            bool tex=(p.renderer&2)!=0;
            if(tex&&(!p.texw||!p.texh))continue;
            glUniform1f(loc.uTextured,tex?1.f:0.f);glUniform1f(loc.uTransparent,(p.renderer&1)?1.f:0.f);
            glUniform1f(loc.uChecker,p.checker?1.f:0.f);
            glUniform4f(loc.uColour,((p.colour>>16)&255)/255.f,((p.colour>>8)&255)/255.f,(p.colour&255)/255.f,1.f);
            glUniform4f(loc.uOwner,(at&255)/255.f,((at>>8)&255)/255.f,((at>>16)&255)/255.f,((at>>24)&255)/255.f);
            if(tex){unsigned row=palette_rows[at];glUniform2f(loc.uPaletteOffset,palette_x_attribute(row)/float(512*palette_banks),(palette_y(row)+.5f)/1024.f);}
            if(direct_tiles&&tex) {
                if(tile_bytes>8*1024*1024)clear_tiles();
                int max_level=max_texture_mip(p);
                glUniform4f(loc.uTexture,p.texw,p.texh,0,0);
                glUniform1f(loc.uLOD,p.texlod);glUniform1f(loc.uMicroMinLOD,p.utexminlod);
                glBindBuffer(GL_ARRAY_BUFFER,tile_vbo);
                glEnableVertexAttribArray(2);glEnableVertexAttribArray(3);
                glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(vf2_tiles::vertex),nullptr);
                glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(vf2_tiles::vertex),(void*)(2*sizeof(float)));
                glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,sizeof(vf2_tiles::vertex),(void*)(5*sizeof(float)));
                glVertexAttribPointer(3,4,GL_FLOAT,GL_FALSE,sizeof(vf2_tiles::vertex),(void*)(9*sizeof(float)));
                for(size_t i=first_draw[at];i<first_draw[at+1];i++) {
                    const auto &draw=frame_draws[i];int band=draw.band;
                    glUniform1f(loc.uBand,band);
                    glUniform1f(loc.uBlendMode,band<0?(p.utex?2:0):(band<max_level?1:0));
                    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,tile(p,std::max(0,band),false));
                    glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,tile(p,band<0?0:std::min(band+1,max_level),band<0&&p.utex));
                    glDrawArrays(GL_TRIANGLES,draw.first,draw.count);
                    draw_calls++;submitted_vertices+=draw.count;
                }
                glDisableVertexAttribArray(2);glDisableVertexAttribArray(3);
                glBindBuffer(GL_ARRAY_BUFFER,vbo);
                glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,sizeof(vertex),nullptr);
                glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(vertex),(void*)(2*sizeof(float)));
                continue;
            }
            if(tex) {
                glUniform4f(loc.uTexture,p.texw,p.texh,p.texx,p.texy);
                glUniform4f(loc.uWrap,p.wrapx,p.wrapy,p.mirrorx,p.mirrory);
                glUniform4f(loc.uMicro,p.utex,p.utexminlod,p.utexx,p.utexy);
                glUniform1f(loc.uSheet,p.sheet);glUniform1f(loc.uLOD,p.texlod);
                glUniform1f(loc.uMaxLevel,max_texture_mip(p));
                // Geometry frequently uses many whole texture repeats. Remove
                // a common integer period in the highp vertex stage so the
                // mediump interpolator keeps subtexel precision. The period
                // also preserves microtexture mirroring, when present.
                float lo[2]={INFINITY,INFINITY},hi[2]={-INFINITY,-INFINITY};
                for(int i=0;i<p.n;i++)if(p.z[i]>0) {
                    float uv[2]={p.u[i]/p.z[i]/p.texw,p.v[i]/p.z[i]/p.texh};
                    for(int a=0;a<2;a++){lo[a]=std::min(lo[a],uv[a]);hi[a]=std::max(hi[a],uv[a]);}
                }
                float micro=p.utex?std::exp2(std::exp2(float(p.utexminlod))):1.f;
                float period[2]={p.utex?std::max(2.f,256.f/(p.texw*micro)):2.f,p.utex?std::max(2.f,256.f/(p.texh*micro)):2.f};
                float base[2]={};
                for(int a=0;a<2;a++)if(std::isfinite(lo[a])&&std::isfinite(hi[a]))base[a]=period[a]*std::round((lo[a]+hi[a])*.5f/period[a]);
                glUniform2f(loc.uUVBase,base[0],base[1]);
            }
            vertex vertices[8];
            for(int i=0;i<p.n;i++) vertices[i]={p.x[i],p.y[i],tex?p.z[i]:1.f,tex?p.u[i]:0.f,tex?p.v[i]:0.f};
            glBufferData(GL_ARRAY_BUFFER,p.n*sizeof(vertex),vertices,GL_STREAM_DRAW);glDrawArrays(GL_TRIANGLE_FAN,0,p.n);
        }
        glDisable(GL_SCISSOR_TEST);glDisable(GL_DEPTH_TEST);glDisableVertexAttribArray(0);glDisableVertexAttribArray(1);glBindBuffer(GL_ARRAY_BUFFER,0);
        if(!owners&&f.overlay&&!f.overlay(f.layers)){glBindFramebuffer(GL_FRAMEBUFFER,prior);return false;}
        if(discard_framebuffer){const GLenum attachment=GL_DEPTH_ATTACHMENT;discard_framebuffer(GL_FRAMEBUFFER,1,&attachment);}
        glBindFramebuffer(GL_FRAMEBUFFER,prior);pending=true;
        cpu_time=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
        if(++painted_frames%300==0&&std::getenv("VF2_GLES_STATS")){
            std::fprintf(stderr,"GLES tiles: %zu resident, %zu bytes; hits %llu misses %llu clears %llu uploaded %llu bytes; %llu draws, %llu vertices (last 300 submissions)\n",tiles.size(),tile_bytes,(unsigned long long)tile_hits,(unsigned long long)tile_misses,(unsigned long long)tile_clears,(unsigned long long)tile_uploads,(unsigned long long)draw_calls,(unsigned long long)submitted_vertices);
            tile_hits=tile_misses=tile_clears=tile_uploads=draw_calls=submitted_vertices=0;
        }
        return glGetError()==GL_NO_ERROR;
    }
public:
    ~gles2() override {
        if(cull_sampleless)std::fprintf(stderr,"Sampleless cull: %llu polygons, %d raster subpixel bits\n",
            (unsigned long long)sampleless_polygons,raster_subpixel_bits);
        if(shared_packet_frames)std::fprintf(stderr,"Shared packet storage: %llu frames\n",(unsigned long long)shared_packet_frames);
        if(packet_trim_test)std::fprintf(stderr,"Packet trim test: %llu trims, %016llx capacity trace\n",
            (unsigned long long)packet_trims,(unsigned long long)packet_trim_checksum);
        if(cell_vertex_stats)
            std::fprintf(stderr,"Intermediate cell vertices: %llu full, %llu compact\n",
                (unsigned long long)full_cell_vertices,(unsigned long long)ordinary_cell_vertices);
        if(prepared_draw_frames)
            std::fprintf(stderr,"Worker-prepared draws: %llu batches, %llu jobs\n",
                         (unsigned long long)prepared_draw_frames,(unsigned long long)prepared_draw_jobs);
        for(size_t i=0;i<packet_phases.size();++i){
            const auto &phase=packet_phases[i];
            std::fprintf(stderr,"Packet phases %zu %.6f %.6f %.6f %.6f\n",i+1,phase[0],phase[1],phase[2],phase[3]);
        }
        if(rotate_palette_atlas)std::fprintf(stderr,"3D palette texture switches: %llu\n",(unsigned long long)palette_switches);
        release_retired_tiles();clear_tiles();clear_palettes();glDeleteTextures(1,&palette_atlas);
        glDeleteTextures(1,&palette_spare);
        glDeleteTextures(targets.size(),targets.data());glDeleteTextures(4,sheets);glDeleteBuffers(1,&vbo);glDeleteBuffers(1,&tile_vbo);glDeleteBuffers(1,&tile_ibo);glDeleteRenderbuffers(1,&depth_buffer);glDeleteFramebuffers(1,&fbo);glDeleteProgram(program);for(const auto &p:batch_programs)if(p.name)glDeleteProgram(p.name);
        for(const auto &p:compact_programs)if(p.name)glDeleteProgram(p.name);
        if(compact_meta)std::fprintf(stderr,"Compact vertex frames: %zu packed, %zu wide fallback\n",
                                    compact_frames,wide_fallback_frames);
    }
    bool init() {
        if(!glGetString(GL_VERSION))return false;
        if (edge_aa) {
            const auto *extensions = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
            if (!extensions || !std::strstr(extensions, "GL_EXT_multisampled_render_to_texture")) {
                std::fputs("VF2 GLES edge AA requires multisampled render to texture\n", stderr);
                return false;
            }
            attach_msaa = reinterpret_cast<PFNGLFRAMEBUFFERTEXTURE2DMULTISAMPLEEXTPROC>(
                eglGetProcAddress("glFramebufferTexture2DMultisampleEXT"));
            depth_msaa = reinterpret_cast<PFNGLRENDERBUFFERSTORAGEMULTISAMPLEEXTPROC>(
                eglGetProcAddress("glRenderbufferStorageMultisampleEXT"));
            GLint max_samples = 0;
            glGetIntegerv(GL_MAX_SAMPLES_EXT, &max_samples);
            if (!attach_msaa || !depth_msaa || max_samples < edge_samples) {
                std::fputs("VF2 GLES edge AA cannot allocate four hardware samples\n", stderr);
                return false;
            }
        }
        if(!std::getenv("VF2_KEEP_DEPTH")&&std::strstr(reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)),"GL_EXT_discard_framebuffer"))
            discard_framebuffer=reinterpret_cast<PFNGLDISCARDFRAMEBUFFEREXTPROC>(eglGetProcAddress("glDiscardFramebufferEXT"));
        GLint units=0;glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS,&units);if(units<5)return false;
        GLint range[2],precision=0;glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER,GL_HIGH_FLOAT,range,&precision);
        if(std::getenv("VF2_GLES_MEDIUMP"))precision=0;
        bool low_coordinates=precision==0||std::getenv("VF2_GLES_LOW_COORDS");
        direct_tiles=(precision==0||std::getenv("VF2_GLES_TILES")||std::getenv("VF2_GLES_BATCHED"))&&!std::getenv("VF2_GLES_MANUAL");
        use_batches=direct_tiles&&std::getenv("VF2_GLES_BATCHED");
        indexed_batches=use_batches&&std::getenv("VF2_GLES_INDEXED");
        stack_packets=use_batches&&std::getenv("VF2_GLES_STACK_PACKETS");
        GLuint vs=shader(GL_VERTEX_SHADER,direct_tiles?vf2_tiles::vertex_shader:vertex_source,precision>0,low_coordinates),fs=shader(GL_FRAGMENT_SHADER,direct_tiles?vf2_tiles::fragment_shader:fragment_source,precision>0,low_coordinates);
        if(!vs||!fs){if(vs)glDeleteShader(vs);if(fs)glDeleteShader(fs);return false;}
        program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);
        glBindAttribLocation(program,0,"aPosition");glBindAttribLocation(program,1,"aQ");glBindAttribLocation(program,2,"aTransformA");glBindAttribLocation(program,3,"aTransformB");glLinkProgram(program);glDeleteShader(vs);glDeleteShader(fs);
        GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);
        if(!ok){char log[4096];glGetProgramInfoLog(program,sizeof(log),nullptr,log);std::fprintf(stderr,"GLES2 link: %s\n",log);return false;}
#define VF2_FIND_UNIFORM(n) loc.n=glGetUniformLocation(program,#n);
        VF2_GLES_UNIFORMS(VF2_FIND_UNIFORM)
#undef VF2_FIND_UNIFORM
        glGenFramebuffers(1,&fbo);glGenRenderbuffers(1,&depth_buffer);glGenBuffers(1,&vbo);glGenBuffers(1,&tile_vbo);glGenBuffers(1,&tile_ibo);glGenTextures(4,sheets);
        palette_pixels.resize(palette_banks*palette_width*palette_height*4);glGenTextures(1,&palette_atlas);glBindTexture(GL_TEXTURE_2D,palette_atlas);texture_parameters();
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,palette_banks*palette_width,palette_height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        if(rotate_palette_atlas){
            glGenTextures(1,&palette_spare);glBindTexture(GL_TEXTURE_2D,palette_spare);texture_parameters();
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,palette_banks*palette_width,palette_height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        }
        if(use_batches){
            if(compact_meta)std::fprintf(stderr,"Probe: 40-byte vertex metadata\n");
            GLint prior_program=0;
            if(specialize_batches)glGetIntegerv(GL_CURRENT_PROGRAM,&prior_program);
            const char *attributes[8]={"aPositionQ","aUV0","aUV1","aDepth","aInfo0","aInfo1","aPalette","aColour"};
            for(unsigned layout=0;layout<(compact_meta?2u:1u);++layout){
                const bool packed=layout!=0;
                for(unsigned mode=0;mode<(specialize_batches?batch_programs.size():1);++mode){
                    const GLuint a=shader(GL_VERTEX_SHADER,vf2_batch::vertex_shader,precision>0,low_coordinates,mesh_render,0,packed);
                    const GLuint b=shader(GL_FRAGMENT_SHADER,mode==3?vf2_batch::fragment_shader_textured:vf2_batch::fragment_shader,precision>0,low_coordinates,false,mode);
                    if(!a||!b){if(a)glDeleteShader(a);if(b)glDeleteShader(b);return false;}
                    auto &p=packed?compact_programs[mode]:batch_programs[mode];
                    p.name=glCreateProgram();glAttachShader(p.name,a);glAttachShader(p.name,b);
                    for(unsigned i=0;i<8;i++)glBindAttribLocation(p.name,i,attributes[i]);
                    glLinkProgram(p.name);glDeleteShader(a);glDeleteShader(b);glGetProgramiv(p.name,GL_LINK_STATUS,&ok);
                    if(!ok){char log[4096];glGetProgramInfoLog(p.name,sizeof(log),nullptr,log);std::fprintf(stderr,"GLES batch link: %s\n",log);return false;}
                    p.size=glGetUniformLocation(p.name,"uSize");p.scale=glGetUniformLocation(p.name,"uScale");
                    p.height=glGetUniformLocation(p.name,"uHeight");p.owners=glGetUniformLocation(p.name,"uOwnerMode");
                    p.atlas_a=glGetUniformLocation(p.name,"uAtlasA");p.atlas_b=glGetUniformLocation(p.name,"uAtlasB");
                    p.palette=glGetUniformLocation(p.name,"uPalette");
                    if(specialize_batches){
                        glUseProgram(p.name);
                        glUniform1i(p.atlas_a,0);glUniform1i(p.atlas_b,1);glUniform1i(p.palette,2);
                    }
                }
            }
            if(specialize_batches)glUseProgram(GLuint(prior_program));
            if(mesh_render){
                mesh_rows_location=glGetUniformLocation(batch_programs[0].name,"uMeshRows[0]");
                if(mesh_rows_location<0)return false;
            }
            if(!batch_atlas.init())return false;
        }
        return glGetError()==GL_NO_ERROR;
    }
    const char *name()const override{return use_batches?"OpenGL ES 2.0 batched atlas rasterizer":"OpenGL ES 2.0 hardware rasterizer";}
    void set_rams(const gpu3d_rams &next)override {
        if(staged_plan&&!same_rams(next))staged_plan=false;
        apply_rams(next,false);
    }
    bool prewarm_atlas(const gpu3d_rams &next,const uint64_t *keys,size_t count){
        if(!use_batches||staged_uploads)return false;
        apply_rams(next,false);
        batch_atlas.defer_uploads(true);
        bool ok=true;
        for(size_t i=0;i<count&&ok;++i)ok=batch_atlas.prewarm(rams,keys[i]);
        batch_atlas.defer_uploads(false);
        if(ok&&batch_atlas.flush())return true;
        batch_atlas.clear();
        return false;
    }
    void set_quality(gpu3d_quality)override{}
    gpu3d_quality quality()const override{return gpu3d_quality::fast;}
    bool make_targets(int w,int h,int count)override {
        if(cull_sampleless)glGetIntegerv(GL_SUBPIXEL_BITS,&raster_subpixel_bits);
        if(w==width&&h==height&&int(targets.size())==count)return true;
        glDeleteTextures(targets.size(),targets.data());targets.assign(count,0);glGenTextures(count,targets.data());width=w;height=h;
        glBindRenderbuffer(GL_RENDERBUFFER,depth_buffer);
        edge_target_verified = false;
        if (edge_aa)
            depth_msaa(GL_RENDERBUFFER, edge_samples, GL_DEPTH_COMPONENT16, w, h);
        else
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
        for(GLuint t:targets){glBindTexture(GL_TEXTURE_2D,t);texture_parameters();glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);}
        return glGetError()==GL_NO_ERROR;
    }
    void *target_handle(int i)const override{return i>=0&&size_t(i)<targets.size()?reinterpret_cast<void*>(uintptr_t(targets[i])):nullptr;}
    bool targets_rgba()const override{return true;}
    bool prepare_cpu(const gpu3d_poly *polys,size_t count,const gpu3d_frame &f)override{
        if(f.w!=width||f.h!=height||f.scale<=0)return false;
        frame_indexed=indexed_batches;
        if(use_batches)prepare_packets(polys,count,f);return true;
    }
    template<class Polygon>
    bool prepare_staged(const Polygon *polys,size_t count,const gpu3d_frame &f,const gpu3d_rams &next){
        constexpr bool projected=std::is_same_v<Polygon,gpu3d_projected_poly>;
        if constexpr(projected){
            if(!indexed_batches||!use_prepared_draws||!compact_packets||mesh_render||
               !std::getenv("VF2_GLES_DIRECT_PACK")||std::getenv("VF2_CHECK_DIRECT_PACK"))return false;
        }
        if(f.w!=width||f.h!=height||f.scale<=0)return false;
        const auto start=std::chrono::steady_clock::now();
        staged_plan=false;staged_uploads=true;batch_atlas.defer_uploads(true);
        const auto rams_start=clock::now();
        apply_rams(next,true);
        const auto rams_end=clock::now();
        frame_indexed=indexed_batches;
        packets_ready=false;
        static const bool overlap_palette=std::getenv("VF2_OVERLAP_PALETTES")!=nullptr;
        static const bool allow_fans=std::getenv("VF2_RETAINED_FANS")&&std::getenv("VF2_GLES_DIRECT_PACK");
        bool palettes_ready=false;
        if(overlap_palette)prepare_packets(polys,count,f,&palettes_ready,allow_fans);
        else{
            prepare_packets(polys,count,f,nullptr,allow_fans);
            palettes_ready=prepare_palettes(polys,count,staged_rows,false);
        }
        if(vf2_tiles::clip_stats_on())vf2_tiles::clip_stats_print(stderr,"frame");
        const auto clipped=profile_packet_phases?clock::now():clock::time_point{};
        timing_packets=profile_packet_phases;
        current_pack_ms=0;
        static const bool plan_fail_stats=std::getenv("VF2_PLAN_FAIL_STATS")!=nullptr;
        if constexpr(projected){
            if(palettes_ready&&packets_ready&&draw_lists_ready){
                const auto claim_failures=batch_atlas.claim_failures;
                staged_plan=prepare_batch(nullptr,count,f,staged_rows,false);
                static const bool retry_atlas=std::getenv("VF2_RETRY_PROJECTED_ATLAS")!=nullptr;
                if(!staged_plan&&retry_atlas&&batch_atlas.claim_failures!=claim_failures){
                    // Clear deferred placements and retire GL names after the fence.
                    batch_atlas.clear();
                    if(cull_sampleless&&shared_packets_active&&
                       std::any_of(prepared_draws.begin(),prepared_draws.end(),
                                   [](const auto &range){return range.sampleless!=0;})){
                        // Culling required the omitted materials to be resident.
                        // Rebuild the packet list against the empty atlas.
                        prepare_packets(polys,count,f,nullptr,allow_fans);
                    }else packets_ready=true;
                    staged_plan=packets_ready&&draw_lists_ready&&prepare_batch(nullptr,count,f,staged_rows,false);
                    if(plan_fail_stats)std::fprintf(stderr,"Projected atlas retry at staged frame %llu: %s\n",
                        (unsigned long long)staged_frames+1,staged_plan?"ready":"failed");
                }
            }else if(plan_fail_stats)std::fprintf(stderr,"Projected plan skipped at staged frame %llu: palettes=%d packets=%d draw_lists=%d count=%zu\n",
                (unsigned long long)staged_frames+1,palettes_ready,packets_ready,draw_lists_ready,count);
            if(plan_fail_stats&&!staged_plan)std::fprintf(stderr,"Projected plan FAILED at staged frame %llu (falling back to expanded polygons)\n",
                (unsigned long long)staged_frames+1);
        }else if(palettes_ready){
            for(int attempt=0;attempt<2;++attempt){
                if(prepare_batch(polys,count,f,staged_rows,false)){staged_plan=true;break;}
                if(index_overflow){
                    if(plan_fail_stats)std::fprintf(stderr,"Retry %d: index overflow, re-planning non-indexed at staged frame %llu\n",
                        attempt,(unsigned long long)staged_frames+1);
                    frame_indexed=false;packets_ready=false;
                    if(prepare_batch(polys,count,f,staged_rows,false)){staged_plan=true;break;}
                }
                if(plan_fail_stats)std::fprintf(stderr,"Retry %d: clearing the atlas at staged frame %llu\n",
                    attempt,(unsigned long long)staged_frames+1);
                batch_atlas.clear();
            }
            if(plan_fail_stats&&!staged_plan)std::fprintf(stderr,"Staged plan FAILED at staged frame %llu\n",(unsigned long long)staged_frames+1);
        }
        staged_polys=polys;staged_projected=projected;staged_count=count;staged_frame=f;
        const auto completed=clock::now();
        if(timing_packets){
            packet_phases.push_back({phase_ms(rams_start,rams_end),phase_ms(rams_end,clipped),phase_ms(clipped,completed)-current_pack_ms,current_pack_ms});
            timing_packets=false;
        }
        staged_cpu_ms=phase_ms(start,completed);
        staged_cpu_total+=staged_cpu_ms;++staged_frames;
        // A planning-capacity failure retains the existing post-fence fallback.
        if constexpr(projected)return staged_plan;
        if(!staged_plan&&std::getenv("VF2_REQUIRE_STAGED_PLAN")){std::fprintf(stderr,"Expected a complete staged CPU plan\n");return false;}
        return true;
    }
    bool prepare_frame_cpu(const gpu3d_poly *p,size_t n,const gpu3d_frame &f,const gpu3d_rams &next)override{
        if(!stage_materials||!use_batches||!direct_tiles)return prepare_cpu(p,n,f);
        return prepare_staged(p,n,f,next);
    }
    bool prepare_projected(const gpu3d_projected_stream &stream,const gpu3d_frame &f,const gpu3d_rams &next){
        if(!stage_materials||!use_batches||!direct_tiles)return false;
        return prepare_staged(stream.polygons.data(),stream.polygons.size(),f,next);
    }
    bool submit_projected(const gpu3d_projected_stream &stream,const gpu3d_frame &f,int target){
        if(target<0||size_t(target)>=targets.size()||!have_rams||f.ss!=1||f.edge||f.w!=width||f.h!=height)return false;
        began=std::chrono::steady_clock::now();
        bool painted=false;
        const bool result=paint_staged(stream.polygons.data(),true,stream.polygons.size(),f,target,false,painted);
        if(painted)return result;
        std::vector<gpu3d_poly> fallback;
        stream.expand(fallback);
        return paint(fallback.data(),fallback.size(),f,target,false);
    }
    bool submit(const gpu3d_poly *p,size_t n,const gpu3d_frame &f,int target)override{return paint(p,n,f,target,false);}
    bool finish(double *gpu_ms=nullptr,double *cpu_ms=nullptr)override {
        glFinish();if(cpu_ms)*cpu_ms=cpu_time;
        if(gpu_ms)*gpu_ms=pending?std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count():0;
        pending=false;return glGetError()==GL_NO_ERROR;
    }
    bool settle(double *gpu_ms=nullptr,double *cpu_ms=nullptr)override {
        // The compositor uses this context and command queue: its sampling is
        // already ordered after our draw. Keep CPU and GPU work overlapped.
        if(cpu_ms)*cpu_ms=cpu_time;
        if(gpu_ms)*gpu_ms=pending?-1:0;
        return glGetError()==GL_NO_ERROR;
    }
    bool busy()const override{return pending;}
    bool read_target(int target,uint32_t *out,size_t stride)override {
        if(target<0||size_t(target)>=targets.size())return false;
        GLint prior;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&prior);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        attach_color(target);
        std::vector<uint8_t> bytes(size_t(width)*height*4);glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,bytes.data());
        for(int y=0;y<height;y++)for(int x=0;x<width;x++) {
            auto p=&bytes[(size_t(height-1-y)*width+x)*4];out[size_t(y)*stride+x]=(uint32_t(p[3])<<24)|(uint32_t(p[0])<<16)|(uint32_t(p[1])<<8)|p[2];
        }
        glBindFramebuffer(GL_FRAMEBUFFER,prior);return glGetError()==GL_NO_ERROR;
    }
    bool draw(const gpu3d_poly *p,size_t n,int w,int h,int scale,uint32_t *owner,size_t ostride,uint32_t *rgba,size_t rstride,double *gpu_ms)override {
        if(!make_targets(w,h,1))return false;
        gpu3d_frame f;f.w=w;f.h=h;f.scale=scale;
        if(owner) {
            if(!paint(p,n,f,0,true))return false;
            std::vector<uint32_t> pixels(size_t(w)*h);if(!read_target(0,pixels.data(),w))return false;
            for(int y=0;y<h;y++)for(int x=0;x<w;x++) {uint32_t c=pixels[size_t(y)*w+x];owner[size_t(y)*ostride+x]=(c&0xff00ff00)|((c&255)<<16)|((c>>16)&255);}
        }
        if(rgba){if(!paint(p,n,f,0,false)||!read_target(0,rgba,rstride))return false;}
        return finish(gpu_ms);
    }
};
#undef VF2_GLES_UNIFORMS
}
std::unique_ptr<gpu3d> gpu3d_create_gles2() {
    auto device=std::make_unique<gles2>();if(!device->init())return nullptr;return device;
}

bool gles2_prepare_projected(gpu3d &renderer,const gpu3d_projected_stream &stream,const gpu3d_frame &f,const gpu3d_rams &rams){
    return static_cast<gles2 &>(renderer).prepare_projected(stream,f,rams);
}
bool gles2_prewarm_atlas(gpu3d &renderer,const gpu3d_rams &rams,const uint64_t *keys,size_t count){
    return static_cast<gles2 &>(renderer).prewarm_atlas(rams,keys,count);
}
static bool atlas_source_matches(const gpu3d_rams &rams,const vf2_batch::atlas_source_signature &source,
                                 std::vector<uint32_t> &words){
    const auto [key,expected]=source;
    const unsigned x=key&2047,y=(key>>11)&1023,w=(key>>21)&4095,h=(key>>33)&4095;
    const auto *sheet=rams.tex[(key>>45)&1];
    if(!sheet)return false;
    const unsigned columns=((x&3)+w+3)/4,rows=((y&1)+h+1)/2;
    words.clear();
    words.reserve(size_t(columns)*rows);
    for(unsigned v=0;v<rows;++v)for(unsigned u=0;u<columns;++u)
        words.push_back(sheet[vf2_batch::atlas_word_address((x&~3u)+u*4,(y&~1u)+v*2)]);
    return m2::state_hash(reinterpret_cast<const uint8_t *>(words.data()),words.size()*sizeof(words[0]))==expected;
}
bool gles2_check_atlas_sources(const gpu3d_rams &rams,const vf2_batch::atlas_source_signature *signatures,size_t count){
    std::vector<uint32_t> words;
    return std::all_of(signatures,signatures+count,[&](const auto &s){return atlas_source_matches(rams,s,words);});
}
bool gles2_submit_projected(gpu3d &renderer,const gpu3d_projected_stream &stream,const gpu3d_frame &f,int target){
    return static_cast<gles2 &>(renderer).submit_projected(stream,f,target);
}
