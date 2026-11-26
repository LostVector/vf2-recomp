// Model 2 texture packets for GLES2's interpolator-to-sampler precision path.
// Texture coordinates are formed in the highp vertex stage and sampled directly
// in the fragment stage. No fragment coordinate arithmetic is required.
#pragma once
#include "projected_stream.h"
#include "gles2_packet_math.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>
#include <type_traits>
#include <vector>

namespace vf2_tiles {
using point=gpu3d_projected_point; // q=1/z, u/v are divided by z
// Diagnostic counters for the tile clipper: where a frame's clipping time goes.
// Enabled only by VF2_CLIP_STATS; the counters are plain (a race under the row
// pool only blurs them, and diagnosis runs single-threaded).
struct clip_counters {
    uint64_t calls=0,points=0,cells=0,cell_u=0,cell_v=0,verts=0,inside_skip=0,ordinary=0,triangles=0;
    uint64_t wide_grids=0,wide_cells=0,wide_u_omitted=0,wide_unhandled=0;
    double t_copy=0,t_outer=0,t_cells=0,t_cell_copy=0,t_cell_clip=0,t_cell_tri=0;
    // Per-polygon cell-count distribution: is the busy-frame tail one huge-UV-span
    // polygon straggling a single core (the parallel split is by polygon index), or
    // many ordinary polygons? max_cells is the largest single clip_packet_into call;
    // hist buckets calls by log2(cells) so a straggler shows as mass in the top bins.
    uint64_t max_cells=0,hist[17]={};
    // Native-repeat A/B diagnostics: nr_track = track flag read at init,
    // nr_guard = indexed splitting calls that reached the collapse guard,
    // nr_pred = predicate matched, nr_fire = suppression executed (cells -> one).
    // nr_guard>0 but nr_pred==0 => flag/identity/band blocks the predicate;
    // nr_pred>0 but nr_fire==0 is impossible (same block); nr_guard==0 => the
    // indexed clip path never clipped this material (wrong instantiation).
    uint64_t nr_track=0,nr_guard=0,nr_pred=0,nr_fire=0;
    // Focused probe on the (832,384) 64x64 track material: how many of its
    // splitting calls reach the Indexed guard (nr_tguard), at which bands
    // (nr_tband[band+1]), and how many the predicate accepts (nr_tpred). Compare
    // nr_tguard to the material's split_calls: a gap means the splits are on the
    // non-indexed instantiation; nr_tguard high but nr_tpred low means band_ok fails.
    uint64_t nr_tguard=0,nr_tpred=0,nr_tband[8]={};
    // Per-material ranking for the native-repeat A/B: which materials pay the most
    // per-cell splitting (cells) and how many of those splits a native-repeat
    // collapse could remove. Keyed by (texx,texy,texw,texh,renderer). Flags show the
    // real wrap/mirror state so eligibility is visible, not assumed.
    struct mat_row {uint64_t cells=0,calls=0,split_calls=0;
        uint16_t texx=0,texy=0,texw=0,texh=0;uint8_t rend=0,wrapx=0,wrapy=0,mirrorx=0,mirrory=0;};
    std::vector<mat_row> mats;
    int mat_find(uint16_t texx,uint16_t texy,uint16_t texw,uint16_t texh,uint8_t rend){
        for(int i=0;i<(int)mats.size();++i){const auto&m=mats[i];
            if(m.texx==texx&&m.texy==texy&&m.texw==texw&&m.texh==texh&&m.rend==rend)return i;}
        mats.push_back({0,0,0,texx,texy,texw,texh,rend,0,0,0,0});return int(mats.size())-1;
    }
};
inline clip_counters &clip_stats(){ static clip_counters s; return s; }
inline bool clip_stats_on(){ static const bool on=std::getenv("VF2_CLIP_STATS")!=nullptr; return on; }
inline double clip_clock_ms(){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline void clip_stats_print(FILE *out,const char *label){
    auto &s=clip_stats();
    std::fprintf(out,"Clip stats %s calls %llu points %llu cells %llu u %llu v %llu verts %llu skip %llu ordinary %llu tri %llu copy %.3f outer %.3f cells %.3f ccopy %.3f cclip %.3f ctri %.3f\n",
        label,(unsigned long long)s.calls,(unsigned long long)s.points,(unsigned long long)s.cells,
        (unsigned long long)s.cell_u,(unsigned long long)s.cell_v,(unsigned long long)s.verts,
        (unsigned long long)s.inside_skip,(unsigned long long)s.ordinary,(unsigned long long)s.triangles,
        s.t_copy,s.t_outer,s.t_cells,s.t_cell_copy,s.t_cell_clip,s.t_cell_tri);
    std::fprintf(out,"Clip cells-per-call %s max %llu hist",label,(unsigned long long)s.max_cells);
    for(int b=0;b<17;++b)if(s.hist[b])std::fprintf(out," [%d:%llu]",b,(unsigned long long)s.hist[b]);
    std::fprintf(out,"\n");
    std::fprintf(out,"Clip wide columns %s grids %llu cells %llu U clips omitted %llu unhandled %llu\n",
        label,(unsigned long long)s.wide_grids,(unsigned long long)s.wide_cells,
        (unsigned long long)s.wide_u_omitted,(unsigned long long)s.wide_unhandled);
    std::fprintf(out,"Clip natrep %s track %u guard %llu pred %llu fire %llu\n",label,
        (unsigned)(std::getenv("VF2_NATIVE_REPEAT_TRACK")!=nullptr),
        (unsigned long long)s.nr_guard,(unsigned long long)s.nr_pred,(unsigned long long)s.nr_fire);
    std::fprintf(out,"Clip natrep832 %s tguard %llu tpred %llu bands",label,
        (unsigned long long)s.nr_tguard,(unsigned long long)s.nr_tpred);
    for(int b=0;b<8;++b)if(s.nr_tband[b])std::fprintf(out," [%d:%llu]",b-1,(unsigned long long)s.nr_tband[b]);
    std::fprintf(out,"\n");
    // Per-material ranking for the native-repeat A/B, sorted by split cells: the
    // cells paid by calls that split across repeat boundaries. Flags show the real
    // wrap/mirror state so eligibility (wrap both axes, no mirror) is visible.
    {
        std::vector<int> ord;ord.reserve(s.mats.size());
        for(int i=0;i<(int)s.mats.size();++i)ord.push_back(i);
        std::sort(ord.begin(),ord.end(),[&](int a,int b){return s.mats[a].cells>s.mats[b].cells;});
        int shown=0;
        for(int i:ord){const auto&m=s.mats[i];
            if(m.cells==0)continue;
            if(++shown>24)break;
            std::fprintf(out,"Clip mat %s texx %u texy %u %ux%u rend %u wx%u wy%u mx%u my%u calls %llu splits %llu cells %llu\n",
                label,m.texx,m.texy,m.texw,m.texh,m.rend,m.wrapx,m.wrapy,m.mirrorx,m.mirrory,
                (unsigned long long)m.calls,(unsigned long long)m.split_calls,(unsigned long long)m.cells);}
    }
    s={};
}
struct vertex {
    point p;
    float a[4],b[4]; // UV scale/bias for each of the two texture samples
};
struct ordinary_vertex {
    point p;
    int16_t cell_x,cell_y;
    vertex expand()const{
        const float x=-float(cell_x),y=-float(cell_y);
        return {p,{1,1,x,y},{1,1,x,y}};
    }
};
static_assert(sizeof(vertex)==52&&sizeof(ordinary_vertex)==24);
struct indexed_packet {
    std::vector<vertex> vertices;
    std::vector<uint32_t> indices;
    bool native_repeat=false;
    std::vector<ordinary_vertex> ordinary;
    size_t vertex_count()const{return ordinary.empty()?vertices.size():ordinary.size();}
    vertex vertex_at(size_t i)const{return ordinary.empty()?vertices[i]:ordinary[i].expand();}
};
template<class Vertices,class Indices>
struct ordinary_packet_view {
    Vertices &vertices;
    Indices &indices;
};
// Emitted-vertex count for either packet shape (the clip_timer is instantiated
// for both indexed_packet and std::vector<vertex>).
template<class Packet> inline size_t packet_vertex_count(const Packet &o){return o.size();}
inline size_t packet_vertex_count(const indexed_packet &o){return o.vertex_count();}

// The clipper indexes the current packet from zero. Append its elements to a
// chunk's backing vector while reporting the size relative to this packet.
template<class T>
struct packet_tail {
    std::vector<T> &backing;
    size_t first;
    explicit packet_tail(std::vector<T> &out):backing(out),first(out.size()){}
    size_t size()const{return backing.size()-first;}
    auto end(){return backing.end();}
    void clear(){backing.resize(first);}
    void push_back(const T &value){backing.push_back(value);}
    void insert(typename std::vector<T>::iterator at,std::initializer_list<T> values){backing.insert(at,values);}
    void insert(typename std::vector<T>::iterator at,const T *begin,const T *end){
        backing.insert(at,begin,end);
    }
};
struct packet_storage {
    std::vector<vertex> vertices;
    std::vector<ordinary_vertex> ordinary;
    std::vector<uint32_t> indices;
    void clear(){vertices.clear();ordinary.clear();indices.clear();}
    size_t spare_bytes()const{
        return (vertices.capacity()-vertices.size())*sizeof(vertex)+
            (ordinary.capacity()-ordinary.size())*sizeof(ordinary_vertex)+
            (indices.capacity()-indices.size())*sizeof(uint32_t);
    }
};
struct packet_writer {
    packet_tail<vertex> vertices;
    packet_tail<ordinary_vertex> ordinary;
    packet_tail<uint32_t> indices;
    bool native_repeat=false;
    explicit packet_writer(packet_storage &out):vertices(out.vertices),ordinary(out.ordinary),indices(out.indices){}
};
template<class Distance> std::vector<point> clip(std::vector<point> &&in,Distance distance){
    if(in.empty())return {};
    size_t inside=0;for(const auto &v:in)inside+=distance(v)>=0;
    if(inside==in.size())return std::move(in);
    if(!inside)return {};
    std::vector<point> out;out.reserve(in.size()+2);
    point a=in.back();double da=distance(a);
    for(const point &b:in){
        double db=distance(b);
        if((da>=0)!=(db>=0)){
            double t=da/(da-db);
            out.push_back({float(a.x+(b.x-a.x)*t),float(a.y+(b.y-a.y)*t),float(a.q+(b.q-a.q)*t),float(a.u+(b.u-a.u)*t),float(a.v+(b.v-a.v)*t)});
        }
        if(db>=0)out.push_back(b);
        a=b;da=db;
    }
    return out;
}
template<class Distance> std::vector<point> clip_float(std::vector<point> &&in,Distance distance){
    if(in.empty())return {};
    size_t inside=0;for(const auto &v:in)inside+=distance(v)>=0;
    if(inside==in.size())return std::move(in);
    if(!inside)return {};
    std::vector<point> out;out.reserve(in.size()+2);
    point a=in.back();float da=distance(a);
    for(const point &b:in){
        float db=distance(b);
        if((da>=0)!=(db>=0)){
            float t=da/(da-db);
            out.push_back({a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.q+(b.q-a.q)*t,a.u+(b.u-a.u)*t,a.v+(b.v-a.v)*t});
        }
        if(db>=0)out.push_back(b);
        a=b;da=db;
    }
    return out;
}
class vector_polygon {
    std::vector<point> values;
public:
    void reserve(size_t n){values.reserve(n);}
    void push_back(const point &p){values.push_back(p);}
    void assign(const point *p,size_t n){values.assign(p,p+n);}
    size_t size()const{return values.size();}
    const point *data()const{return values.data();}
    const point &operator[](size_t i)const{return values[i];}
    auto begin()const{return values.begin();}auto end()const{return values.end();}
    template<class Distance> void clip_by(Distance distance){values=clip(std::move(values),distance);}
    template<class Distance> void clip_by_float(Distance distance){values=clip_float(std::move(values),distance);}
};
// Clipping usually needs fewer than a dozen points. Spill rather than truncate
// if an unusual polygon exceeds the inline capacity. Only active points are
// copied; plane passes swap buffer pointers instead of copying whole arrays.
class point_buffer {
    static constexpr size_t inline_capacity=32;
    std::array<point,inline_capacity> local;
    std::vector<point> overflow;
    size_t count=0;bool spilled=false;
public:
    const point *data()const{return spilled?overflow.data():local.data();}
    size_t size()const{return count;}
    void clear(){count=0;if(spilled)overflow.clear();}
    static constexpr size_t max_inline_clip_input=(2*inline_capacity+1)/3;
    point *inline_write(){return local.data();}
    void commit_inline(size_t n){
        if(spilled)overflow.clear();
        spilled=false;count=n;
    }

    void push_back(const point &p){
        if(!spilled&&count<local.size()){local[count++]=p;return;}
        if(!spilled){overflow.assign(local.begin(),local.begin()+count);spilled=true;}
        overflow.push_back(p);++count;
    }
    void assign(const point *p,size_t n){
        if(n<=local.size()){spilled=false;std::copy_n(p,n,local.begin());overflow.clear();}
        else {overflow.assign(p,p+n);spilled=true;}
        count=n;
    }
};
class stack_polygon {
    point_buffer first,second;
    point_buffer *input=&first,*scratch=&second;
    const point *borrowed=nullptr;
    size_t borrowed_count=0;
    void discard_input(){borrowed=nullptr;input->clear();}
    void use_scratch(){borrowed=nullptr;std::swap(input,scratch);}
public:
    stack_polygon()=default;
    stack_polygon(const stack_polygon &other){first.assign(other.data(),other.size());}
    stack_polygon &operator=(const stack_polygon &)=delete;
    void reserve(size_t){}
    void assign(const point *p,size_t count){borrowed=nullptr;input->assign(p,count);}
    void borrow(const point *p,size_t count){borrowed=p;borrowed_count=count;}
    void push_back(const point &p){
        if(borrowed)assign(borrowed,borrowed_count);
        input->push_back(p);
    }
    size_t size()const{return borrowed?borrowed_count:input->size();}
    const point *data()const{return borrowed?borrowed:input->data();}
    const point &operator[](size_t i)const{return data()[i];}
    const point *begin()const{return data();}const point *end()const{return begin()+size();}
    template<class Distance> void clip_by(Distance distance){
        const size_t n=size();
        if(!n)return;
        const point *source=data();
        // Compute each vertex's signed distance once and reuse it when
        // clipping, keeping the same interpolation and output.
        if(n<=32){
            double d[32];bool any=false,all=true;
            for(size_t i=0;i<n;++i){d[i]=distance(source[i]);if(d[i]>=0)any=true;else all=false;}
            if(!any){discard_input();return;}
            if(all)return;
            static const bool write_inline=std::getenv("DAYTONA_CLIP_INLINE_OUTPUT")!=nullptr;
            // A clipped ring emits one point per inside vertex and two per
            // inside run. Even for a nonconvex ring, at most floor(3*n/2)
            // points survive one plane, so this range always fits.
            if(write_inline&&n<=point_buffer::max_inline_clip_input){
                point *const base=scratch->inline_write();
                point *out=base;
                const point *a=source+n-1;
                double da=d[n-1];
                for(size_t i=0;i<n;++i){
                    const point &b=source[i];const double db=d[i];
                    if((da>=0)!=(db>=0)){
                        const double t=da/(da-db);
                        *out++={float(a->x+(b.x-a->x)*t),float(a->y+(b.y-a->y)*t),
                                float(a->q+(b.q-a->q)*t),float(a->u+(b.u-a->u)*t),
                                float(a->v+(b.v-a->v)*t)};
                    }
                    if(db>=0)*out++=b;
                    a=&b;da=db;
                }
                scratch->commit_inline(size_t(out-base));
                use_scratch();
                return;
            }
            scratch->clear();point a=source[n-1];double da=d[n-1];
            for(size_t i=0;i<n;++i){
                const point &b=source[i];const double db=d[i];
                if((da>=0)!=(db>=0)){
                    const double t=da/(da-db);
                    scratch->push_back({float(a.x+(b.x-a.x)*t),float(a.y+(b.y-a.y)*t),float(a.q+(b.q-a.q)*t),float(a.u+(b.u-a.u)*t),float(a.v+(b.v-a.v)*t)});
                }
                if(db>=0)scratch->push_back(b);
                a=b;da=db;
            }
            use_scratch();
            return;
        }
        size_t inside=0;for(const auto &p:*this)inside+=distance(p)>=0;
        if(inside==size())return;
        if(!inside){discard_input();return;}
        scratch->clear();point a=(*this)[size()-1];double da=distance(a);
        for(const point &b:*this){
            const double db=distance(b);
            if((da>=0)!=(db>=0)){
                const double t=da/(da-db);
                scratch->push_back({float(a.x+(b.x-a.x)*t),float(a.y+(b.y-a.y)*t),float(a.q+(b.q-a.q)*t),float(a.u+(b.u-a.u)*t),float(a.v+(b.v-a.v)*t)});
            }
            if(db>=0)scratch->push_back(b);
            a=b;da=db;
        }
        use_scratch();
    }
    // Optional single-precision cell planes. Rounding may change the
    // clipping decision or the interpolated points.
    template<class Distance> void clip_by_float(Distance distance){
        const size_t n=size();
        if(!n)return;
        const point *source=data();
        if(n<=32){
            float d[32];bool any=false,all=true;
            for(size_t i=0;i<n;++i){d[i]=distance(source[i]);if(d[i]>=0)any=true;else all=false;}
            if(!any){discard_input();return;}
            if(all)return;
            scratch->clear();point a=source[n-1];float da=d[n-1];
            for(size_t i=0;i<n;++i){
                const point &b=source[i];const float db=d[i];
                if((da>=0)!=(db>=0)){
                    const float t=da/(da-db);
                    scratch->push_back({a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.q+(b.q-a.q)*t,a.u+(b.u-a.u)*t,a.v+(b.v-a.v)*t});
                }
                if(db>=0)scratch->push_back(b);
                a=b;da=db;
            }
            use_scratch();
            return;
        }
        clip_by(distance);
    }
};
template<class Polygon>
inline void transform(float out[4],const Polygon &p,int level,double u,double v,bool micro){
    double factor[2]={1,1};int size[2]={p.texw>>level,p.texh>>level};
    if(micro){
        double s=vf2_packet_math::micro_scale(p.utexminlod);
        factor[0]=p.texw*s/128;factor[1]=p.texh*s/128;size[0]=size[1]=128;
    }
    double centre[2]={u,v};bool mirror[2]={p.mirrorx!=0,p.mirrory!=0};
    for(int a=0;a<2;a++){
        double cell=std::floor(centre[a]*factor[a]);
        bool reflected=mirror[a]&&vf2_packet_math::odd_cell(cell);
        out[a]=float((reflected?-1:1)*factor[a]);
        out[a+2]=float(reflected?cell+1-1./(256*size[a]):-cell);
    }
}
// An ordinary split cell has centre (x+0.5, y+0.5) and unit
// scale before mirroring, so its integer coordinates are already known.
template<class Polygon>
inline void cell_transform(float out[4],const Polygon &p,int level,int x,int y){
    const auto axis=[](int cell,int size,bool mirror,float &scale,float &bias){
        const bool reflected=mirror&&(unsigned(cell)&1);
        scale=reflected?-1.f:1.f;
        bias=reflected?float(double(cell)+1-1./(256*size)):-float(cell);
    };
    axis(x,p.texw>>level,p.mirrorx,out[0],out[2]);
    axis(y,p.texh>>level,p.mirrory,out[1],out[3]);
}

// Only the profiled VF2 material takes this route. Its remaining mip
// bands and texture modes retain the ordinary atlas sampler.
template<class Polygon>
inline bool native_repeat_material(const Polygon &p,int band){
    static const bool enabled=std::getenv("VF2_NATIVE_REPEAT_MATERIAL")!=nullptr;
    // Daytona native-repeat A/B: the top-ranked opaque track material on the slow
    // Beginner window (atlas 832,384, 64x64, opaque, wraps both axes, no mirror)
    // pays the most per-cell repeat splits of any native-repeat-eligible material
    // (642 cells / 18 calls in the ranking run). Opt-in, branch-local; the VF2
    // material below is unchanged. The 64x64 identity guarantees a square
    // power-of-two tile, so the standalone repeat page always builds, and the
    // transform's centre rebase keeps the wrapped UV near the origin for mediump
    // sub-texel precision.
    static const bool track=std::getenv("VF2_NATIVE_REPEAT_TRACK")!=nullptr;
    // The probe showed the (832,384) road material splits at band 5 (the 2x2 mip
    // pair), not the shallow bands the VF2 material used, so the track route spans
    // -1..5. Band 6 (a 1x1 mip) is left on the split path: a 1x1 standalone repeat
    // page is degenerate and no band-6 splits were observed.
    const bool wrap_nmirror=p.wrapx&&p.wrapy&&!p.mirrorx&&!p.mirrory;
    const bool vf2=enabled&&band>=-1&&band<=4&&(band>=0||!p.utex)&&
        p.texx==1792&&p.texy==0&&p.sheet==0&&
        p.texw==256&&p.texh==256&&(p.renderer&3)==2&&wrap_nmirror;
    const bool daytona=track&&band>=-1&&band<=5&&(band>=0||!p.utex)&&
        p.texx==832&&p.texy==384&&
        p.texw==64&&p.texh==64&&(p.renderer&3)==2&&wrap_nmirror;
    // Broader Daytona coverage (VF2_NATIVE_REPEAT_ALL): any opaque square
    // power-of-two texture that wraps both axes without mirroring can take the
    // hardware repeat route. Square power-of-two is mandatory — the standalone
    // repeat page only builds for w==h POT, so a non-square match would fail
    // get_pair and DROP the draw. The band is capped so the per-level tile stays
    // >=2x2 (a 1x1 repeat page is degenerate). Subsumes the (832,384) route.
    static const bool all=std::getenv("VF2_NATIVE_REPEAT_ALL")!=nullptr;
    // The full broad set broke the image with a whole-screen bbox (mean |delta| ~1),
    // which points at a large-area material (the 128x128 ground/wall, 2nd-most-split
    // at 379 cells) rather than the byte-exact 64x64 road. VF2_NATIVE_REPEAT_CAP64
    // restricts the broad route to <=64x64 tiles to test whether excluding the large
    // material keeps most of the win while holding the image.
    static const int cap=std::getenv("VF2_NATIVE_REPEAT_CAP64")?64:0;
    const bool size_ok=!cap||p.texw<=cap;
    const bool square_pot=p.texw==p.texh&&p.texw&&!(p.texw&(p.texw-1));
    const bool tile_ok=(p.texw>>(band>0?band:0))>=2;
    const bool broad=all&&size_ok&&band>=-1&&(band>=0||!p.utex)&&square_pot&&tile_ok&&
        (p.renderer&3)==2&&wrap_nmirror;
    return vf2||daytona||broad;
}
// A band covers one adjacent mip pair, or base+microtexture (band -1).
// Splits at repeat boundaries preserve Model 2's independent mirror/wrap flags.
template<bool Indexed>
inline void clear_packet(std::conditional_t<Indexed,indexed_packet,std::vector<vertex>> &out){
    if constexpr(Indexed){out.vertices.clear();out.ordinary.clear();out.indices.clear();out.native_repeat=false;}
    else out.clear();
}

template<class Polygon>
inline std::pair<float,float> depth_range(const Polygon &p){
    float minq=INFINITY,maxq=-INFINITY;
    for(int i=0;i<p.n;i++){
        const float q=projected_point(p,i).q;
        minq=std::min(minq,q);maxq=std::max(maxq,q);
    }
    return {minq,maxq};
}

// The inline clipper can spill, so only bounded fans use the
// buffered path. One append per fan preserves the original index order.
inline constexpr size_t buffered_fan_points=32;
template<bool Indexed,bool BufferIndices,class Packet,class Polygon,class MakeVertex>
inline void append_cell_triangles_impl(Packet &out,const Polygon &polygon,MakeVertex make_vertex){
    std::array<uint32_t,BufferIndices?(buffered_fan_points-2)*3:0> buffered;
    size_t buffered_count=0;
    uint32_t origin=UINT32_MAX,previous_end=0;
    size_t previous_triangle=0;
    const auto &first=polygon[0];
    for(size_t i=1;i+1<polygon.size();++i){
        const auto &middle_point=polygon[i],&end=polygon[i+1];
        const double area=(middle_point.x-first.x)*double(end.y-first.y)-
            (middle_point.y-first.y)*double(end.x-first.x);
        if(std::abs(area)<1e-10)continue;
        if constexpr(Indexed){
            const bool adjacent=origin!=UINT32_MAX&&previous_triangle+1==i;
            if(origin==UINT32_MAX){
                origin=uint32_t(out.vertices.size());
                out.vertices.push_back(make_vertex(first));
            }
            uint32_t middle=previous_end;
            if(!adjacent){
                middle=uint32_t(out.vertices.size());
                out.vertices.push_back(make_vertex(middle_point));
            }
            previous_end=uint32_t(out.vertices.size());
            out.vertices.push_back(make_vertex(end));
            if constexpr(BufferIndices){
                buffered[buffered_count++]=origin;
                buffered[buffered_count++]=middle;
                buffered[buffered_count++]=previous_end;
            }else out.indices.insert(out.indices.end(),{origin,middle,previous_end});
            previous_triangle=i;
        }else{
            for(const auto &vertex:{first,middle_point,end})out.push_back(make_vertex(vertex));
        }
    }
    if constexpr(BufferIndices)
        out.indices.insert(out.indices.end(),buffered.data(),buffered.data()+buffered_count);
}

template<bool Indexed,class Packet,class Polygon,class MakeVertex>
inline void append_cell_triangles(Packet &out,const Polygon &polygon,MakeVertex make_vertex){
#if defined(VF2_CELL_BUFFER_INDICES)
    if constexpr(Indexed){
        if(polygon.size()<=buffered_fan_points){
            append_cell_triangles_impl<true,true>(out,polygon,make_vertex);
            return;
        }
    }
#endif
    append_cell_triangles_impl<Indexed,false>(out,polygon,make_vertex);
}

// Inside the viewport, one mip band, and the first ordinary texture cell,
// clipping leaves the fan unchanged and both transforms are the identity.
// Compare against the same rounded float denominators as the double ratios.
template<class Packet,class Polygon>
inline bool origin_indexed_fan(Packet &out,const Polygon &p,int band,
                              float x0,float y0,float x1,float y1,double lower,double upper,bool compact=false,bool outer_kept=false){
    if(band<0&&p.utex)return false;
    struct original_points {
        const Polygon &poly;
        size_t size()const{return poly.n;}
        decltype(auto) operator[](size_t i)const{return projected_point(poly,i);}
    } polygon{p};
    for(size_t i=0;i<polygon.size();++i){
        const auto &[x,y,q,u,v]=polygon[i];
        if(!outer_kept&&!(x>=x0&&x<=x1&&y>=y0&&y<=y1&&q>0&&q>=lower&&q<=upper))return false;
        const float w=q*p.texw,h=q*p.texh;
        if(!(w<INFINITY&&h<INFINITY&&u>=0&&u<w&&v>=0&&v<h))return false;
    }
    if(compact){
        ordinary_packet_view view{out.ordinary,out.indices};
        append_cell_triangles<true>(view,polygon,[](const point &p){return ordinary_vertex{p,0,0};});
    }else append_cell_triangles<true>(out,polygon,[](const point &p){
        return vertex{p,{1,1,-0.0f,-0.0f},{1,1,-0.0f,-0.0f}};
    });
    return true;
}

// For a complete uncut fan, keep the projected vertices as the source
// and reuse the index topology. Only the two cell transforms are new.
inline constexpr uint32_t fan_indices[]={0,1,2,0,2,3,0,3,4,0,4,5,0,5,6,0,6,7};
struct retained_fan {
    const gpu3d_poly *polygon=nullptr;
    const gpu3d_projected_point *points=nullptr;
    unsigned n=0;
    int band=-1;
    float a[4]{},b[4]{};

    point position(size_t i)const{
        return points?points[i]:projected_point(*polygon,i);
    }
    size_t index_count()const{return (n-2)*3;}
};

// A rounded edge preserves the sign of comparisons against any other
// binary32 value. Only equality can conceal which side of the exact
// binary64 edge the numerator lies on.
inline bool fan_cell_contains(float numerator,float denominator,float base){
    const float lower=base*denominator,upper=(base+1)*denominator;
    return numerator>=lower&&numerator<=upper&&
        (numerator!=lower||double(numerator)>=double(base)*denominator)&&
        (numerator!=upper||double(numerator)<double(base+1)*denominator);
}

template<class Polygon>
inline bool retain_fan(retained_fan &out,const Polygon &p,int max_level,
                       float x0,float y0,float x1,float y1,bool use_cell_index,bool use_cell_bounds=false,
                       bool fused_uv_bounds=false,int *uncut_band=nullptr){
    if(p.n<3||p.n>8||!p.texw||!p.texh)return false;
    float minq=INFINITY,maxq=0;
    for(unsigned i=0;i<p.n;++i){
        const auto &[x,y,q,u,v]=projected_point(p,i);
        if(!(x>=x0&&x<=x1&&y>=y0&&y<=y1&&q>0&&q<INFINITY))return false;
        minq=std::min(minq,q);maxq=std::max(maxq,q);
    }
    int band=-1;
    double upper=INFINITY;
    for(;band<=max_level;++band){
        const double lower=band==max_level?0:vf2_packet_math::lod_boundary(int64_t(p.texlod)+(int64_t(band)+1)*128);
        if(maxq>=lower){
            // Equality can emit a fan in both bands.
            if(!(minq>lower&&maxq<upper))return false;
            break;
        }
        upper=lower;
    }
    if(band>max_level)return false;
    if(uncut_band)*uncut_band=band;
    const bool micro=band<0&&p.utex;
    double steps[2]={1,1};
    if(micro){
        const double s=vf2_packet_math::micro_scale(p.utexminlod);
        steps[0]=std::min(1.,128/(p.texw*s));steps[1]=std::min(1.,128/(p.texh*s));
    }
    int cell[2];
    static const bool fast_cells=std::getenv("VF2_FAST_FAN_CELL_COMPARE")!=nullptr;
    if(fused_uv_bounds&&(!use_cell_bounds||micro)){
        if(!(steps[0]>0&&steps[1]>0))return false;
        double low[2]={INFINITY,INFINITY},high[2]={-INFINITY,-INFINITY};
        for(unsigned i=0;i<p.n;++i){
            const auto &v=projected_point(p,i);
            const float w=v.q*p.texw,h=v.q*p.texh;
            if(!(w>0&&w<INFINITY&&h>0&&h<INFINITY))return false;
            const double uv[2]={double(v.u)/w,double(v.v)/h};
            if(!std::isfinite(uv[0])||!std::isfinite(uv[1]))return false;
            for(unsigned axis=0;axis<2;++axis){
                low[axis]=std::min(low[axis],uv[axis]);
                high[axis]=std::max(high[axis],uv[axis]);
            }
        }
        for(unsigned axis=0;axis<2;++axis){
            const double base=std::floor(low[axis]/steps[axis]);
            if(base<std::numeric_limits<int>::min()||base>std::numeric_limits<int>::max()||
               base!=std::floor(high[axis]/steps[axis]))return false;
            cell[axis]=int(base);
        }
    }else for(int axis=0;axis<2;++axis){
        if(!(steps[axis]>0))return false;
        const auto numerator=[&](const point &v){return axis?v.v:v.u;};
        const unsigned dim=axis?p.texh:p.texw;
        if(use_cell_bounds&&!micro){
            // The denominator is binary32 and the (bounded) integer
            // edge has at most 16 bits. Their product in binary64 is
            // exact. A distinct binary32 numerator cannot produce a
            // quotient within half a binary64 ulp of that integer edge.
            const auto &first_position=projected_point(p,0);
            const float first=first_position.q*dim;
            if(!(first>0&&first<INFINITY))return false;
            const double base=std::floor(double(numerator(first_position))/first);
            if(!(base>=-32768&&base<=32767))return false;
            cell[axis]=int(base);
            for(unsigned i=1;i<p.n;++i){
                const auto &v=projected_point(p,i);
                const float denominator=v.q*dim;
                if(!(denominator>0&&denominator<INFINITY))return false;
                const float value=numerator(v);
                if(fast_cells){
                    if(!fan_cell_contains(value,denominator,float(base)))return false;
                }else if(!(value>=base*denominator&&value<(base+1)*denominator))return false;
            }
        }else{
            double lo=INFINITY,hi=-INFINITY;
            for(unsigned i=0;i<p.n;++i){
                const auto &v=projected_point(p,i);
                const float denominator=v.q*dim;
                if(!(denominator>0&&denominator<INFINITY))return false;
                const double uv=double(numerator(v))/denominator;
                if(!std::isfinite(uv))return false;
                lo=std::min(lo,uv);hi=std::max(hi,uv);
            }
            const double base=std::floor(lo/steps[axis]);
            if(base<std::numeric_limits<int>::min()||base>std::numeric_limits<int>::max()||
               base!=std::floor(hi/steps[axis]))return false;
            cell[axis]=int(base);
        }
    }
    const auto &first=projected_point(p,0);
    for(unsigned i=1;i+1<p.n;++i){
        const auto &m=projected_point(p,i),&end=projected_point(p,i+1);
        const double area=(m.x-first.x)*double(end.y-first.y)-(m.y-first.y)*double(end.x-first.x);
        if(std::abs(area)<1e-10)return false;
    }
    const int level=std::max(0,band),next=band<0?0:std::min(level+1,max_level);
    if(use_cell_index&&!micro){
        cell_transform(out.a,p,level,cell[0],cell[1]);
        cell_transform(out.b,p,next,cell[0],cell[1]);
    }else{
        const double x=(cell[0]+.5)*steps[0],y=(cell[1]+.5)*steps[1];
        transform(out.a,p,level,x,y,false);
        transform(out.b,p,next,x,y,micro);
    }
    out.band=band;
    out.n=p.n;
    if constexpr(std::is_same_v<Polygon,gpu3d_poly>){out.polygon=&p;out.points=nullptr;}
    else{out.polygon=nullptr;out.points=p.points;}
    return true;
}

template<bool Indexed,bool Stack,class Packet,class Polygon>
inline void clip_packet_into(Packet &out,
                             const Polygon &p,int band,int max_level,float x0,float y0,
                             float x1,float y1,bool force_cells,double lower,double upper,
                             bool use_cell_index=false,bool compact_cells=false,
                             bool test_viewport_bounds=false,unsigned uniform_axes=0,bool outer_kept=false){
    std::conditional_t<Stack,stack_polygon,vector_polygon> polygon;polygon.reserve(p.n);
    const bool stats=clip_stats_on();
    const auto stats_now=[&]{return stats?std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count():0.0;};
    // Adds this call's counts and times however the function exits (the early
    // returns are common for clipped-away polygons).
    struct clip_timer {
        bool on;double t0,t1=0,t2=0,ccopy=0,cclip=0,ctri=0;uint64_t points,cells=0,cu=0,cv=0,ordinary=0,triangles=0;bool skipped=false;
        uint16_t mtexx=0,mtexy=0,mtexw=0,mtexh=0;uint8_t mrend=0;bool mtrack=false;
        uint8_t mwrapx=0,mwrapy=0,mmirrorx=0,mmirrory=0;
        ~clip_timer(){if(!on)return;auto &s=clip_stats();s.calls++;s.points+=points;s.cells+=cells;s.cell_u+=cu;s.cell_v+=cv;s.inside_skip+=skipped;
            s.ordinary+=ordinary;s.triangles+=triangles;
            if(cells>s.max_cells)s.max_cells=cells;
            const int bucket=cells<=1?0:std::min(16,63-__builtin_clzll((unsigned long long)cells));
            s.hist[bucket]++;
            if(mtrack){auto&m=s.mats[s.mat_find(mtexx,mtexy,mtexw,mtexh,mrend)];
                m.cells+=cells;m.calls++;m.split_calls+=cells>1?1:0;
                m.wrapx=mwrapx;m.wrapy=mwrapy;m.mirrorx=mmirrorx;m.mirrory=mmirrory;}
            s.t_copy+=t1-t0;s.t_outer+=t2-t1;s.t_cells+=stats_now_()-t2;
            s.t_cell_copy+=ccopy;s.t_cell_clip+=cclip;s.t_cell_tri+=ctri;}
        static double stats_now_(){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();}
    } timer{};timer.on=stats;timer.t0=stats_now();timer.points=uint64_t(p.n);
    if(stats){timer.mtexx=p.texx;timer.mtexy=p.texy;timer.mtexw=p.texw;timer.mtexh=p.texh;timer.mrend=p.renderer&3;timer.mtrack=true;
        timer.mwrapx=p.wrapx?1:0;timer.mwrapy=p.wrapy?1:0;timer.mmirrorx=p.mirrorx?1:0;timer.mmirrory=p.mirrory?1:0;}
    const double s0=stats_now();
    bool fused_outer_bounds=false;
    if constexpr(Stack&&std::is_same_v<Polygon,gpu3d_projected_poly>){
        static const bool fuse_outer=[](){
            const char *flag=std::getenv("VF2_BORROW_FUSED_OUTER_CLIP");
            return flag&&flag[0]=='1'&&flag[1]=='\0';
        }();
        fused_outer_bounds=fuse_outer&&test_viewport_bounds&&p.n!=0;
        if(fused_outer_bounds)polygon.borrow(p.points,p.n);
        else if(p.n)polygon.assign(p.points,p.n);
    }else for(int i=0;i<p.n;i++)polygon.push_back(projected_point(p,i));
    // The projected renderer supplies finite viewport bounds. Only skip the
    // entire sequence when it would keep all of the input vertices untouched.
    const double s1=stats_now();
    timer.t1=s1;
    bool skipped=false;
    static const bool float_clipping=std::getenv("VF2_CLIP_FLOAT_ALL")!=nullptr;
    const auto in_viewport=[&](const point &v){return v.x>=x0&&v.x<=x1&&v.y>=y0&&v.y<=y1;};
    bool inside=outer_kept,lower_is_noop=outer_kept,upper_is_noop=outer_kept;
    // The checked fan bounds already cover every outer plane in this band.
    if(!outer_kept&&fused_outer_bounds&&!float_clipping&&std::isfinite(lower)){
        inside=true;
        lower_is_noop=true;
        upper_is_noop=true;
        for(const auto &v:polygon){
            if(!in_viewport(v)){inside=false;break;}
            lower_is_noop&=double(v.q)>=lower;
            upper_is_noop&=double(v.q)<=upper;
        }
        // A viewport or lower-plane intersection can add rounded points.
        // Later planes must then evaluate the updated polygon.
        lower_is_noop&=inside;
        upper_is_noop&=lower_is_noop;
    }else if(!outer_kept&&test_viewport_bounds){
        inside=std::all_of(polygon.begin(),polygon.end(),in_viewport);
    }
    if(!inside){
        if(float_clipping){
            polygon.clip_by_float([&](const point &v){return v.x-x0;});
            polygon.clip_by_float([&](const point &v){return x1-v.x;});
            polygon.clip_by_float([&](const point &v){return v.y-y0;});
            polygon.clip_by_float([&](const point &v){return y1-v.y;});
        }else{
            polygon.clip_by([&](const point &v){return double(v.x)-x0;});
            polygon.clip_by([&](const point &v){return x1-double(v.x);});
            polygon.clip_by([&](const point &v){return double(v.y)-y0;});
            polygon.clip_by([&](const point &v){return y1-double(v.y);});
        }
    }else skipped=true;
    timer.skipped=skipped;
    if(!outer_kept&&float_clipping){
        polygon.clip_by_float([&](const point &v){return v.q-float(lower);});
        if(std::isfinite(upper))polygon.clip_by_float([&](const point &v){return float(upper)-v.q;});
    }else{
        if(!lower_is_noop)polygon.clip_by([&](const point &v){return double(v.q)-lower;});
        if(!upper_is_noop&&std::isfinite(upper))polygon.clip_by([&](const point &v){return upper-double(v.q);});
    }
    timer.t2=stats_now();
    if(polygon.size()<3)return;
    double lo[2]={INFINITY,INFINITY},hi[2]={-INFINITY,-INFINITY};
    for(const auto &v:polygon){
        if(v.q<=0)return;
        double uv[2]={double(v.u)/(v.q*p.texw),double(v.v)/(v.q*p.texh)};
        for(int a=0;a<2;a++){lo[a]=std::min(lo[a],uv[a]);hi[a]=std::max(hi[a],uv[a]);}
    }
    double step[2]={1,1};
    // Daytona diagnostic (VF2_CELL_MERGE): the indexed path force-splits every
    // axis, but a texture that wraps and does not mirror on an axis samples the
    // same tile in every cell along it, so the reference (non-indexed) path does
    // not split that axis. Dropping the force_cells term on such axes merges the
    // adjacent cells into a single clip+emit. Geometry hash differs from the
    // split path; the render matches the non-indexed reference. Default off.
    static const bool cell_merge=std::getenv("VF2_CELL_MERGE")!=nullptr;
    const bool force_split=force_cells&&!cell_merge;
    bool split[2]={force_split||!p.wrapx||p.mirrorx,force_split||!p.wrapy||p.mirrory};
    if(band<0&&p.utex){
        double s=vf2_packet_math::micro_scale(p.utexminlod);
        step[0]=std::min(1.,128/(p.texw*s));step[1]=std::min(1.,128/(p.texh*s));
    }
    int first[2]={},last[2]={};
    for(int a=0;a<2;a++)if(split[a]){first[a]=int(std::floor(lo[a]/step[a]));last[a]=int(std::floor(hi[a]/step[a]));}
    if constexpr(Indexed){
        if((first[0]!=last[0]||first[1]!=last[1])&&force_cells){
            if(stats)++clip_stats().nr_guard;
            const bool probe=stats&&p.texx==832&&p.texy==384&&p.texw==64&&p.texh==64;
            if(probe){auto&s=clip_stats();++s.nr_tguard;if(band>=-1&&band<=6)++s.nr_tband[band+1];}
            const bool pred=native_repeat_material(p,band);
            if(stats&&pred)++clip_stats().nr_pred;
            if(probe&&pred)++clip_stats().nr_tpred;
            if(pred){
                out.native_repeat=true;
                split[0]=split[1]=false;
                first[0]=last[0]=first[1]=last[1]=0;
                if(stats)++clip_stats().nr_fire;
            }
        }
    }
    // Daytona fidelity-safe merge (VF2_CELL_MERGE_SAFE=N, default off). The full
    // VF2_CELL_MERGE diagnostic merges every wrapping non-mirror axis, which
    // softens texture mip/filter locality in wide spans (visible on crowds).
    // This bounded variant merges an axis only when the span is at most N cells,
    // so the merged cell's interpolated texture coordinate stays inside the
    // shader's exact 16.8 fixed-point range and samples the same texels as the
    // per-cell split (the merged transform differs from the split transform only
    // by an integer tile offset, which the wrap mode maps back to the same texel;
    // the residual is float rounding that vanishes for small spans). N=0 disables.
    // Independent of the full merge; when that is on it already merged everything.
    static const int cell_merge_safe=[](){
        const char *e=std::getenv("VF2_CELL_MERGE_SAFE");
        return e?std::max(0,std::atoi(e)):0;
    }();
    bool already_merged=false;
    if constexpr(Indexed)already_merged=out.native_repeat;
    if(!cell_merge&&cell_merge_safe>0&&!already_merged)for(int a=0;a<2;a++){
        if(!(force_cells&&split[a]))continue;
        const bool wrap_nmirror=a?p.wrapy&&!p.mirrory:p.wrapx&&!p.mirrorx;
        if(!wrap_nmirror)continue;
        if(last[a]-first[a]+1<=cell_merge_safe){split[a]=false;first[a]=last[a]=0;}
    }
    for(int axis=0;axis<2;++axis)if(uniform_axes&(1u<<axis)){
        split[axis]=false;
        first[axis]=last[axis]=0;
    }
    const bool micro=band<0&&p.utex;
    const bool ordinary_cells=use_cell_index&&!micro&&split[0]&&split[1];
    const bool use_ordinary=compact_cells&&ordinary_cells&&!p.mirrorx&&!p.mirrory&&
        first[0]>=INT16_MIN&&last[0]<=INT16_MAX&&first[1]>=INT16_MIN&&last[1]<=INT16_MAX;
    const int level=std::max(0,band),next_level=band<0?0:std::min(level+1,max_level);
    // Hierarchical column clip: the per-cell u-band clip depends only on the
    // column, but the (cy,cx) loop recomputes it for every row. Precompute each
    // column's u-clipped points once and reuse them across that column's rows.
    // The per-cell order stays u-then-v and the emission order stays (cy,cx), so
    // the emitted vertices and indices are byte-identical; only the redundant
    // per-row u-clips are removed. Single-row grids go straight through the
    // cell loop because no other row can reuse their column points.
    // Keep common grids on the stack and
    // reuse bounded thread-local storage for wider grids with multiple rows.
    static const bool col_clip=std::getenv("VF2_CELL_COL_CLIP")!=nullptr;
    static const bool wide_col_clip=std::getenv("VF2_WIDE_CELL_COLUMNS")!=nullptr;
    static const bool borrow_cells=std::getenv("VF2_BORROW_CELL_POINTS")!=nullptr;
    static const bool float_clip_pre=std::getenv("VF2_CELL_CLIP_FLOAT")!=nullptr;
    constexpr int col_max=8,wide_max=64,col_pts=32;
    struct column { point points[col_pts]; unsigned count; };
    column local_columns[col_max];
    column *columns=local_columns;
    const int64_t width=int64_t(last[0])-first[0]+1;
    const int64_t rows=int64_t(last[1])-first[1]+1;
    const bool reuse_columns=Stack&&col_clip&&!float_clip_pre&&split[0]&&width>1&&rows>1;
    const bool wide=reuse_columns&&width>col_max;
    bool col_ok=reuse_columns&&width<=col_max;
    if constexpr(Stack)if(wide&&wide_col_clip&&width<=wide_max){
        thread_local std::vector<column> wide_columns;
        if(wide_columns.size()<size_t(width))wide_columns.resize(size_t(width));
        columns=wide_columns.data();
        col_ok=true;
    }
    if constexpr(Stack)if(col_ok)for(int cx=first[0];cx<=last[0];cx++){
        stack_polygon col;
        if(borrow_cells)col.borrow(polygon.data(),polygon.size());
        else col.assign(polygon.data(),polygon.size());
        const double l=cx*step[0],r=(cx+1)*step[0],lu=l*p.texw,ru=r*p.texw;
        col.clip_by([&](const point &v){return double(v.u)-lu*v.q;});
        col.clip_by([&](const point &v){return ru*v.q-double(v.u);});
        if(col.size()>col_pts){col_ok=false;break;}
        auto &saved=columns[cx-first[0]];
        saved.count=static_cast<unsigned>(col.size());
        for(unsigned i=0;i<col.size();++i)saved.points[i]=col[i];
    }
    if(stats&&wide){
        auto &s=clip_stats();
        ++s.wide_grids;
        s.wide_cells+=uint64_t(width)*rows;
        if(col_ok)s.wide_u_omitted+=2*uint64_t(width)*(rows-1);
        else ++s.wide_unhandled;
    }
    for(int cy=first[1];cy<=last[1];cy++)for(int cx=first[0];cx<=last[0];cx++){
        ++timer.cells;
        const double c0=stats_now();
        std::conditional_t<Stack,stack_polygon,vector_polygon> cell;
        const column *saved=col_ok?&columns[cx-first[0]]:nullptr;
        const point *source=saved?saved->points:polygon.data();
        const size_t source_count=saved?saved->count:polygon.size();
        if constexpr(Stack){
            if(borrow_cells)cell.borrow(source,source_count);
            else cell.assign(source,source_count);
        }else cell.assign(source,source_count);
        const double c1=stats_now();
        timer.ccopy+=c1-c0;
        static const bool float_clip=std::getenv("VF2_CELL_CLIP_FLOAT")!=nullptr;
        // The cell-plane distance recomputes the loop-invariant product
        // (cell edge * texture width) for every vertex. Hoisting it out of the
        // per-vertex lambda removes a multiply per vertex per cell clip. The
        // association is unchanged: `l*p.texw*v.q` is `((l*p.texw)*v.q)` and l is
        // constant within the cell, so the hoisted form is byte-identical.
        static const bool hoist_cell=std::getenv("VF2_CELL_CLIP_HOIST")!=nullptr;
        if(!col_ok&&split[0]&&first[0]!=last[0]){
            ++timer.cu;
            double l=cx*step[0],r=(cx+1)*step[0];
            if(float_clip){
                const float lf=float(l*p.texw),rf=float(r*p.texw);
                cell.clip_by_float([&](const point &v){return v.u-lf*v.q;});
                cell.clip_by_float([&](const point &v){return rf*v.q-v.u;});
            }else if(hoist_cell){
                const double lu=l*p.texw,ru=r*p.texw;
                cell.clip_by([&](const point &v){return double(v.u)-lu*v.q;});
                cell.clip_by([&](const point &v){return ru*v.q-double(v.u);});
            }else{
                cell.clip_by([&](const point &v){return double(v.u)-l*p.texw*v.q;});
                cell.clip_by([&](const point &v){return r*p.texw*v.q-double(v.u);});
            }
        }
        if(split[1]&&first[1]!=last[1]){
            ++timer.cv;
            double t=cy*step[1],b=(cy+1)*step[1];
            if(float_clip){
                const float tf=float(t*p.texh),bf=float(b*p.texh);
                cell.clip_by_float([&](const point &v){return v.v-tf*v.q;});
                cell.clip_by_float([&](const point &v){return bf*v.q-v.v;});
            }else if(hoist_cell){
                const double tu=t*p.texh,bu=b*p.texh;
                cell.clip_by([&](const point &v){return double(v.v)-tu*v.q;});
                cell.clip_by([&](const point &v){return bu*v.q-double(v.v);});
            }else{
                cell.clip_by([&](const point &v){return double(v.v)-t*p.texh*v.q;});
                cell.clip_by([&](const point &v){return b*p.texh*v.q-double(v.v);});
            }
        }
        const double c2=stats_now();
        timer.cclip+=c2-c1;
        if(cell.size()<3)continue;
        if constexpr(Indexed){
            if(use_ordinary){
                ordinary_packet_view view{out.ordinary,out.indices};
                append_cell_triangles<true>(view,cell,[&](const point &point){
                    return ordinary_vertex{point,int16_t(cx),int16_t(cy)};
                });
                ++timer.ordinary;
                timer.ctri+=stats_now()-c2;
                continue;
            }
        }
        float a[4],b[4];
        if(ordinary_cells){
            cell_transform(a,p,level,cx,cy);
            cell_transform(b,p,next_level,cx,cy);
        }else{
            double centre[2]={split[0]?(cx+.5)*step[0]:(lo[0]+hi[0])*.5,split[1]?(cy+.5)*step[1]:(lo[1]+hi[1])*.5};
            transform(a,p,level,centre[0],centre[1],false);
            transform(b,p,next_level,centre[0],centre[1],micro);
            // A constant sampler axis only needs a coordinate inside its tile.
            for(int axis=0;axis<2;++axis)if(uniform_axes&(1u<<axis)){
                a[axis]=b[axis]=0.f;
                a[axis+2]=b[axis+2]=0.5f;
            }
        }
        // Every field below is assigned, so the aggregate's zero-initialised
        // storage is dead. Avoid the 52-byte memset on ~18k vertices a frame.
        auto make_vertex=[&](const point &point){vertex v;v.p=point;std::copy(a,a+4,v.a);std::copy(b,b+4,v.b);return v;};
        append_cell_triangles<Indexed>(out,cell,make_vertex);
        ++timer.triangles;
        timer.ctri+=stats_now()-c2;
    }
}
template<bool Indexed,bool Stack,class Polygon>
inline void packet_into(std::conditional_t<Indexed,indexed_packet,std::vector<vertex>> &out,
                        const Polygon &p,int band,int max_level,float x0,float y0,
                        float x1,float y1,bool force_cells){
    clear_packet<Indexed>(out);
    const double upper=band<0?INFINITY:vf2_packet_math::lod_boundary(int64_t(p.texlod)+int64_t(band)*128);
    const double lower=band==max_level?0:vf2_packet_math::lod_boundary(int64_t(p.texlod)+int64_t(band+1)*128);
    const auto [minq,maxq]=depth_range(p);
    if(maxq<lower||minq>upper)return;
    clip_packet_into<Indexed,Stack>(out,p,band,max_level,x0,y0,x1,y1,force_cells,lower,upper);
}

template<class Polygon,class Visit>
inline void visit_indexed_bands(const Polygon &p,int max_level,bool stop_below_range,Visit visit){
    const auto [minq,maxq]=depth_range(p);
    double upper=INFINITY;
    for(int band=-1;band<=max_level;++band){
        // Later upper bounds can only decrease. Keep equality in both bands.
        if(stop_below_range&&minq>upper)break;
        const double lower=band==max_level?0:vf2_packet_math::lod_boundary(int64_t(p.texlod)+int64_t(band+1)*128);
        if(!(maxq<lower||minq>upper))visit(band,lower,upper);
        upper=lower;
    }
}

template<bool Stack,class Packet,class Polygon>
inline void clip_indexed_band(Packet &out,const Polygon &p,int band,int max_level,
                              float x0,float y0,float x1,float y1,double lower,double upper,
                              bool origin_fan,bool use_cell_index,bool compact_cells,
                              bool test_viewport_bounds=false,unsigned uniform_axes=0,bool outer_kept=false){
    if(!origin_fan||!origin_indexed_fan(out,p,band,x0,y0,x1,y1,lower,upper,compact_cells,outer_kept))
        clip_packet_into<true,Stack>(out,p,band,max_level,x0,y0,x1,y1,true,lower,upper,
                                    use_cell_index,compact_cells,test_viewport_bounds,uniform_axes,outer_kept);
}

template<bool Stack,class Polygon>
inline int sparse_indexed_packets(std::vector<indexed_packet> &slots,int &first,int old_count,
                                  const Polygon &p,int max_level,float x0,float y0,float x1,float y1,
                                  bool stop_below_range=false,bool origin_fan=false,
                                  bool use_cell_index=false,bool compact_cells=false,bool keep_inactive=false){
    int count=0;
    first=-1;
    visit_indexed_bands(p,max_level,stop_below_range,[&](int band,double lower,double upper){
        if(!count)first=band;
        if(size_t(count)==slots.size())slots.emplace_back();
        auto &out=slots[count++];
        clear_packet<true>(out);
        clip_indexed_band<Stack>(out,p,band,max_level,x0,y0,x1,y1,lower,upper,
                                 origin_fan,use_cell_index,compact_cells);
    });
    if(!keep_inactive)for(int i=count;i<old_count;++i)clear_packet<true>(slots[i]);
    return count;
}

inline std::vector<vertex> packet(const gpu3d_poly &p,int band,int max_level,float x0,float y0,float x1,float y1,bool force_cells=false){
    std::vector<vertex> out;packet_into<false,false>(out,p,band,max_level,x0,y0,x1,y1,force_cells);return out;
}
inline indexed_packet packet_indexed(const gpu3d_poly &p,int band,int max_level,float x0,float y0,float x1,float y1,bool force_cells=false){
    indexed_packet out;packet_into<true,false>(out,p,band,max_level,x0,y0,x1,y1,force_cells);return out;
}
}
