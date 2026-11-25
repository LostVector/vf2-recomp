// Model 2 texture packets for GLES2's interpolator-to-sampler precision path.
// Texture coordinates are formed in the highp vertex stage and sampled directly
// in the fragment stage. No fragment coordinate arithmetic is required.
#pragma once
#include "projected_stream.h"
#include "gles2_packet_math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>
#include <type_traits>
#include <vector>

namespace vf2_tiles {
using point=gpu3d_projected_point; // q=1/z, u/v are divided by z
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
class vector_polygon {
    std::vector<point> values;
public:
    void reserve(size_t n){values.reserve(n);}
    void push_back(const point &p){values.push_back(p);}
    size_t size()const{return values.size();}
    const point &operator[](size_t i)const{return values[i];}
    auto begin()const{return values.begin();}auto end()const{return values.end();}
    template<class Distance> void clip_by(Distance distance){values=clip(std::move(values),distance);}
};
// Clipping usually needs fewer than a dozen points. Spill rather than truncate
// if an unusual polygon exceeds the inline capacity. Only active points are
// copied; plane passes swap buffer pointers instead of copying whole arrays.
class point_buffer {
    std::array<point,32> local;
    std::vector<point> overflow;
    size_t count=0;bool spilled=false;
public:
    const point *data()const{return spilled?overflow.data():local.data();}
    size_t size()const{return count;}
    void clear(){count=0;if(spilled)overflow.clear();}
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
public:
    stack_polygon()=default;
    stack_polygon(const stack_polygon &other){first.assign(other.input->data(),other.size());}
    stack_polygon &operator=(const stack_polygon &)=delete;
    void reserve(size_t){}
    void assign(const point *p,size_t count){input->assign(p,count);}
    void push_back(const point &p){input->push_back(p);}
    size_t size()const{return input->size();}
    const point &operator[](size_t i)const{return input->data()[i];}
    const point *begin()const{return input->data();}const point *end()const{return begin()+size();}
    template<class Distance> void clip_by(Distance distance){
        if(!size())return;
        size_t inside=0;for(const auto &p:*this)inside+=distance(p)>=0;
        if(inside==size())return;
        if(!inside){input->clear();return;}
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
        std::swap(input,scratch);
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
    return enabled&&band>=-1&&band<=4&&(band>=0||!p.utex)&&
        p.texx==1792&&p.texy==0&&p.sheet==0&&p.texw==256&&p.texh==256&&
        (p.renderer&3)==2&&p.wrapx&&p.wrapy&&!p.mirrorx&&!p.mirrory;
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

template<bool Indexed,class Packet,class Polygon,class MakeVertex>
inline void append_cell_triangles(Packet &out,
                                  const Polygon &polygon,MakeVertex make_vertex){
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
            out.indices.insert(out.indices.end(),{origin,middle,previous_end});
            previous_triangle=i;
        }else{
            for(const auto &vertex:{first,middle_point,end})out.push_back(make_vertex(vertex));
        }
    }
}

// Inside the viewport, one mip band, and the first ordinary texture cell,
// clipping leaves the fan unchanged and both transforms are the identity.
// Compare against the same rounded float denominators as the double ratios.
template<class Packet,class Polygon>
inline bool origin_indexed_fan(Packet &out,const Polygon &p,int band,
                              float x0,float y0,float x1,float y1,double lower,double upper,bool compact=false){
    if(band<0&&p.utex)return false;
    struct original_points {
        const Polygon &poly;
        size_t size()const{return poly.n;}
        decltype(auto) operator[](size_t i)const{return projected_point(poly,i);}
    } polygon{p};
    for(size_t i=0;i<polygon.size();++i){
        const auto &[x,y,q,u,v]=polygon[i];
        if(!(x>=x0&&x<=x1&&y>=y0&&y<=y1&&q>0&&q>=lower&&q<=upper))return false;
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

template<class Polygon>
inline bool retain_fan(retained_fan &out,const Polygon &p,int max_level,
                       float x0,float y0,float x1,float y1,bool use_cell_index,bool use_cell_bounds=false,
                       bool fused_uv_bounds=false){
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
    const bool micro=band<0&&p.utex;
    double steps[2]={1,1};
    if(micro){
        const double s=vf2_packet_math::micro_scale(p.utexminlod);
        steps[0]=std::min(1.,128/(p.texw*s));steps[1]=std::min(1.,128/(p.texh*s));
    }
    int cell[2];
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
                if(!(denominator>0&&denominator<INFINITY&&
                     numerator(v)>=base*denominator&&numerator(v)<(base+1)*denominator))return false;
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
                             bool test_viewport_bounds=false){
    std::conditional_t<Stack,stack_polygon,vector_polygon> polygon;polygon.reserve(p.n);
    if constexpr(Stack&&std::is_same_v<Polygon,gpu3d_projected_poly>){
        if(p.n)polygon.assign(p.points,p.n);
    }else for(int i=0;i<p.n;i++)polygon.push_back(projected_point(p,i));
    // The projected renderer supplies finite viewport bounds. Only skip the
    // entire sequence when it would keep all of the input vertices untouched.
    if(!test_viewport_bounds||!std::all_of(polygon.begin(),polygon.end(),[&](const point &v){
        return v.x>=x0&&v.x<=x1&&v.y>=y0&&v.y<=y1;
    })){
        polygon.clip_by([&](const point &v){return double(v.x)-x0;});
        polygon.clip_by([&](const point &v){return x1-double(v.x);});
        polygon.clip_by([&](const point &v){return double(v.y)-y0;});
        polygon.clip_by([&](const point &v){return y1-double(v.y);});
    }
    polygon.clip_by([&](const point &v){return double(v.q)-lower;});
    if(std::isfinite(upper))polygon.clip_by([&](const point &v){return upper-double(v.q);});
    if(polygon.size()<3)return;
    double lo[2]={INFINITY,INFINITY},hi[2]={-INFINITY,-INFINITY};
    for(const auto &v:polygon){
        if(v.q<=0)return;
        double uv[2]={double(v.u)/(v.q*p.texw),double(v.v)/(v.q*p.texh)};
        for(int a=0;a<2;a++){lo[a]=std::min(lo[a],uv[a]);hi[a]=std::max(hi[a],uv[a]);}
    }
    double step[2]={1,1};
    bool split[2]={force_cells||!p.wrapx||p.mirrorx,force_cells||!p.wrapy||p.mirrory};
    if(band<0&&p.utex){
        double s=vf2_packet_math::micro_scale(p.utexminlod);
        step[0]=std::min(1.,128/(p.texw*s));step[1]=std::min(1.,128/(p.texh*s));
    }
    int first[2]={},last[2]={};
    for(int a=0;a<2;a++)if(split[a]){first[a]=int(std::floor(lo[a]/step[a]));last[a]=int(std::floor(hi[a]/step[a]));}
    if constexpr(Indexed){
        if((first[0]!=last[0]||first[1]!=last[1])&&force_cells&&native_repeat_material(p,band)){
            out.native_repeat=true;
            split[0]=split[1]=false;
            first[0]=last[0]=first[1]=last[1]=0;
        }
    }
    const bool micro=band<0&&p.utex;
    const bool ordinary_cells=use_cell_index&&!micro&&split[0]&&split[1];
    const bool use_ordinary=compact_cells&&ordinary_cells&&!p.mirrorx&&!p.mirrory&&
        first[0]>=INT16_MIN&&last[0]<=INT16_MAX&&first[1]>=INT16_MIN&&last[1]<=INT16_MAX;
    const int level=std::max(0,band),next_level=band<0?0:std::min(level+1,max_level);
    for(int cy=first[1];cy<=last[1];cy++)for(int cx=first[0];cx<=last[0];cx++){
        auto cell=polygon;
        if(split[0]&&first[0]!=last[0]){
            double l=cx*step[0],r=(cx+1)*step[0];
            cell.clip_by([&](const point &v){return double(v.u)-l*p.texw*v.q;});
            cell.clip_by([&](const point &v){return r*p.texw*v.q-double(v.u);});
        }
        if(split[1]&&first[1]!=last[1]){
            double t=cy*step[1],b=(cy+1)*step[1];
            cell.clip_by([&](const point &v){return double(v.v)-t*p.texh*v.q;});
            cell.clip_by([&](const point &v){return b*p.texh*v.q-double(v.v);});
        }
        if(cell.size()<3)continue;
        if constexpr(Indexed){
            if(use_ordinary){
                ordinary_packet_view view{out.ordinary,out.indices};
                append_cell_triangles<true>(view,cell,[&](const point &point){
                    return ordinary_vertex{point,int16_t(cx),int16_t(cy)};
                });
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
        }
        auto make_vertex=[&](const point &point){vertex v{};v.p=point;std::copy(a,a+4,v.a);std::copy(b,b+4,v.b);return v;};
        append_cell_triangles<Indexed>(out,cell,make_vertex);
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
                              bool test_viewport_bounds=false){
    if(!origin_fan||!origin_indexed_fan(out,p,band,x0,y0,x1,y1,lower,upper,compact_cells))
        clip_packet_into<true,Stack>(out,p,band,max_level,x0,y0,x1,y1,true,lower,upper,
                                    use_cell_index,compact_cells,test_viewport_bounds);
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
