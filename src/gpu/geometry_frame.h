// Owned camera-space render packets. No pointers into a running machine.
#pragma once
#include "gpu3d.h"
#include "projected_stream.h"
#include "../machine/parallel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numeric>
#include <vector>

struct gpu3d_camera_poly {
    gpu3d_poly material{}; // x/y/z: camera position; u/v: unprojected texture coordinates
    float planes[4][4]{}; // normal x/y/z, distance
    float center[2]{};   // final screen centre including CRTC offsets
    uint16_t depth=0;
    uint8_t window=0;
};

// Owned native geometry handoff. Texture headers and UVs retain their arcade
// integer representation until a worker builds the projected raster packet.
struct gpu3d_camera_attributes {
    float position[4][3]{};
    uint16_t uv[4][2]{},header[4]{};
    int32_t texlod=0;
    uint32_t colour=0;
    int16_t hue=-1;
    uint16_t depth=0,palette_index=0,luma=0,pal=0;
    uint8_t n=0,window=0,build=0;
};

struct gpu3d_camera_context {
    float planes[4][4]{},center[2]{};
    int16_t viewport[4]{};
};

struct gpu3d_compact_camera_poly : gpu3d_camera_attributes, gpu3d_camera_context {};

struct gpu3d_context_camera_poly : gpu3d_camera_attributes {
    uint32_t context=0;
};

// The published list owns both the polygons and the contexts they index.
struct gpu3d_shared_camera_list {
    std::vector<gpu3d_context_camera_poly> polygons;
    std::vector<gpu3d_camera_context> contexts;

    void clear(){polygons.clear();contexts.clear();}
};

// The optional retained-mesh input is owned by the captured frame. The
// object index selects a frame-local transform, while address/count/mode and
// polygon ordinal identify immutable topology across frames.
struct gpu3d_mesh_object {
    uint32_t address=0,count=0,mode=0;
    std::array<float,12> matrix{};
    std::array<float,2> focus{};
};

struct gpu3d_mesh_polygon {
    static constexpr uint32_t invalid=~uint32_t(0);
    uint32_t object=invalid,ordinal=0;
    std::array<std::array<float,3>,4> position{};
};

class gpu3d_geometry_frame {
    struct vertex {float x,y,z,u,v;};
    std::vector<gpu3d_poly> prepared;
    std::vector<size_t> order;
    std::vector<uint64_t> sort_keys;
    using hair_lookup=uint32_t (*)(uint16_t,int);
    struct camera_tables {
        std::array<uint16_t,0x2000> palette;
        std::array<uint16_t,0x6000> xlat;
        std::array<uint8_t,256> gamma;
        const uint16_t *xlat_view=nullptr;
        const uint8_t *gamma_view=nullptr;
        hair_lookup hair=nullptr;
    };
    std::shared_ptr<const gpu3d_shared_camera_list> shared_compact;
    std::vector<gpu3d_camera_context> resolved_contexts;
    std::unique_ptr<camera_tables> captured_tables;
    template<class Polygons> void sort_camera_indices(const Polygons &input){
        sort_keys.clear();
        sort_keys.reserve(input.size());
        for(size_t i=0;i<input.size();++i){
            const auto &p=input[i];
            if(p.window>final_window)continue;
            // Descending window, ascending depth, then newest polygon first.
            // The direct projection capacity limit makes the index fit in 16 bits.
            sort_keys.push_back((uint64_t(UINT8_MAX-p.window)<<32)|
                                (uint64_t(p.depth)<<16)|uint16_t(UINT16_MAX-i));
        }
        std::sort(sort_keys.begin(),sort_keys.end());
        order.resize(sort_keys.size());
        for(size_t i=0;i<sort_keys.size();++i)order[i]=UINT16_MAX-uint16_t(sort_keys[i]);
    }
    static int clipped_reference(vertex (&a)[16],int n,const float (&planes)[4][4],vertex *result){
        vertex b[16];vertex *in=a,*out=b;
        for(const auto &plane:planes){
            if(!n)break;int count=0;
            auto dot=[&](const vertex &v){return (v.x*plane[0]+v.y*plane[1])+v.z*plane[2];};
            float curdot=dot(in[0]);bool curin=curdot>=plane[3];
            for(int i=0;i<n;++i){
                const int next=i+1==n?0:i+1;
                if(curin){if(count==16)return 0;out[count++]=in[i];}
                const float nextdot=dot(in[next]);const bool nextin=nextdot>=plane[3];
                if(curin!=nextin&&!std::isnan(curdot)&&!std::isnan(nextdot)){
                    if(count==16)return 0;
                    const float scale=(plane[3]-curdot)/(nextdot-curdot);
                    const vertex &c=in[i],&d=in[next];
                    out[count++]={c.x+(d.x-c.x)*scale,c.y+(d.y-c.y)*scale,c.z+(d.z-c.z)*scale,c.u+(d.u-c.u)*scale,c.v+(d.v-c.v)*scale};
                }
                curdot=nextdot;curin=nextin;
            }
            n=count;std::swap(in,out);
        }
        if(n<3||n>8)return 0;
        std::copy(in,in+n,result);return n;
    }
    static int clipped(const gpu3d_camera_poly &p,vertex *result){
        vertex a[16];int n=p.material.n;
        for(int i=0;i<n;++i)a[i]={p.material.x[i],p.material.y[i],p.material.z[i],p.material.u[i],p.material.v[i]};
        return clipped_vertices(a,n,p.planes,result);
    }
    static int clipped_vertices(vertex (&a)[16],int n,const float (&planes)[4][4],vertex *result){
        static const bool reference=std::getenv("VF2_REFERENCE_CAMERA_CLIP")!=nullptr;
        if(reference)return clipped_reference(a,n,planes,result);
        vertex b[16];vertex *in=a,*out=b;
        for(const auto &plane:planes){
            if(!n)break;int count=0;
            auto dot=[&](const vertex &v){return (v.x*plane[0]+v.y*plane[1])+v.z*plane[2];};
            float dots[16];int inside=0;
            for(int i=0;i<n;++i){dots[i]=dot(in[i]);inside+=dots[i]>=plane[3];}
            // No boundary crossings: retain this vertex buffer without copying.
            if(inside==n)continue;
            if(!inside)return 0;
            float curdot=dots[0];bool curin=curdot>=plane[3];
            for(int i=0;i<n;++i){
                const int next=i+1==n?0:i+1;
                if(curin){if(count==16)return 0;out[count++]=in[i];}
                const float nextdot=dots[next];const bool nextin=nextdot>=plane[3];
                if(curin!=nextin&&!std::isnan(curdot)&&!std::isnan(nextdot)){
                    if(count==16)return 0;
                    const float scale=(plane[3]-curdot)/(nextdot-curdot);
                    const vertex &c=in[i],&d=in[next];
                    out[count++]={c.x+(d.x-c.x)*scale,c.y+(d.y-c.y)*scale,c.z+(d.z-c.z)*scale,c.u+(d.u-c.u)*scale,c.v+(d.v-c.v)*scale};
                }
                curdot=nextdot;curin=nextin;
            }
            n=count;std::swap(in,out);
        }
        if(n<3||n>8)return 0;
        std::copy(in,in+n,result);return n;
    }
public:
    std::vector<gpu3d_camera_poly> polygons;
    std::vector<gpu3d_compact_camera_poly> compact_polygons;
    std::vector<gpu3d_mesh_object> mesh_objects;
    std::vector<gpu3d_mesh_polygon> mesh_polygons;
    // Indexed like the projected render list. Entries name an unchanged
    // compact source polygon, or invalid when it needs projected geometry.
    std::vector<uint32_t> mesh_projected_sources;
    bool compact=false;
    uint8_t final_window=255;
    void release_shared_compact(){shared_compact.reset();}
    bool has_shared_compact()const{return bool(shared_compact);}
    void retain_compact(std::shared_ptr<const gpu3d_shared_camera_list> list,int xoffset,int yoffset,
            const uint16_t *palette,const uint16_t *xlat,const uint8_t *gamma,hair_lookup hair,
            const gpu3d_rams *color_snapshot=nullptr){
        if(!captured_tables)captured_tables=std::make_unique<camera_tables>();
        auto &tables=*captured_tables;
        std::copy_n(palette,tables.palette.size(),tables.palette.data());
        // An external snapshot must outlive projection of this picture.
        // Other callers keep their own table storage as before.
        if(color_snapshot){
            tables.xlat_view=color_snapshot->xlat;
            tables.gamma_view=color_snapshot->gamma;
        }else{
            std::copy_n(xlat,tables.xlat.size(),tables.xlat.data());
            std::copy_n(gamma,tables.gamma.size(),tables.gamma.data());
            tables.xlat_view=tables.xlat.data();
            tables.gamma_view=tables.gamma.data();
        }
        tables.hair=hair;
        // Offsets belong to the captured picture, not the reusable raw list.
        resolved_contexts=list->contexts;
        for(auto &context:resolved_contexts)resolve_context(context,xoffset,yoffset);
        shared_compact=std::move(list);
    }
    static void resolve_compact(gpu3d_compact_camera_poly &r,int xoffset,int yoffset,
            const uint16_t *palette,const uint16_t *xlat,const uint8_t *gamma,hair_lookup hair_fn){
        resolve_context(r,xoffset,yoffset);
        resolve_material(r,palette,xlat,gamma,hair_fn);
    }
    static void resolve_context(gpu3d_camera_context &r,int xoffset,int yoffset){
        r.center[0]+=xoffset;r.center[1]=(384-r.center[1])+yoffset;
        const int top=r.viewport[1],right=r.viewport[2],bottom=r.viewport[3];
        r.viewport[0]=std::max(0,int(r.viewport[0])+xoffset);r.viewport[1]=std::min(495,right+xoffset);
        r.viewport[2]=std::max(0,384-bottom+yoffset);r.viewport[3]=std::min(383,384-top+yoffset);
    }
    static void resolve_material(gpu3d_camera_attributes &r,
            const uint16_t *palette,const uint16_t *xlat,const uint8_t *gamma,hair_lookup hair_fn){
        auto rgb=[](uint32_t r,uint32_t g,uint32_t b){return 0xff000000u|(r<<16)|(g<<8)|b;};
        const uint32_t colour=palette[r.palette_index],original_luma=r.luma;uint32_t pal=0;
        const unsigned renderer=(r.header[0]>>13)&3;
        if(r.hue>=0&&(r.header[1]&255)==0&&(renderer&2)){
            const uint32_t hair=hair_fn(uint16_t(colour&0x7fff),r.hue);pal=uint16_t(hair);
            if(hair>>16)r.luma=uint16_t(std::min<uint32_t>(uint32_t(r.luma)*(hair>>16)/16,0xffff));
        }
        if(r.build>=2){
            const uint32_t v=r.build==2?255u:64u+(original_luma*9>>4);
            r.header[0]&=0x1fff;r.colour=rgb(v,v,r.build==2?255u:std::min(v+6,255u));
        }else if(renderer&2)r.pal=pal?pal&0x7fff:colour&0x7fff;
        else {
            const uint32_t c=pal?pal&0x7fff:colour&0xffff,l=r.luma>>2;
            const uint32_t red=xlat[(((c>>0)&31)<<8)+l]&255;
            const uint32_t green=xlat[0x2000+(((c>>5)&31)<<8)+l]&255;
            const uint32_t blue=xlat[0x4000+(((c>>10)&31)<<8)+l]&255;
            r.colour=rgb(gamma[red],gamma[green],gamma[blue]);
        }
    }
    static void expand_compact(const gpu3d_compact_camera_poly &r,gpu3d_camera_poly &p){
        expand_compact(r,r,p);
    }
    static void expand_compact(const gpu3d_camera_attributes &r,const gpu3d_camera_context &context,
            gpu3d_camera_poly &p){
        p={};auto &g=p.material;
        g.n=r.n;g.renderer=(r.header[0]>>13)&3;g.checker=(r.header[0]>>15)&1;
        for(unsigned i=0;i<r.n;++i){g.x[i]=r.position[i][0];g.y[i]=r.position[i][1];g.z[i]=r.position[i][2];g.u[i]=r.uv[i][0];g.v[i]=r.uv[i][1];}
        for(unsigned j=0;j<4;++j){std::copy_n(context.planes[j],4,p.planes[j]);g.vp[j]=context.viewport[j];}
        p.center[0]=context.center[0];p.center[1]=context.center[1];p.depth=r.depth;p.window=r.window;
        fill_attributes(r,g);
    }
    template<class Polygon>
    static void fill_attributes(const gpu3d_camera_attributes &r,Polygon &g){
        g.lumabase=(r.header[1]&255)<<7;g.luma=r.luma;g.texlod=r.texlod;g.pal=r.pal;g.colour=r.colour;
        g.sheet=(r.header[2]&0x1000)!=0;
        g.texw=32<<((r.header[0]>>0)&7);g.texh=32<<((r.header[0]>>3)&7);
        g.texx=32*((r.header[2]>>0)&63);g.texy=32*((r.header[2]>>6)&31);
        g.mirrorx=(r.header[0]>>8)&1;g.mirrory=(r.header[0]>>9)&1;
        g.wrapx=(r.header[0]>>6)&1&~g.mirrorx;g.wrapy=(r.header[0]>>7)&1&~g.mirrory;
        g.utex=(r.header[0]>>12)&1;g.utexminlod=(r.header[0]>>10)&3;
        g.utexx=((r.header[2]>>13)&1)*128;g.utexy=((r.header[2]>>14)&3)*128;
    }
    static bool visible(const gpu3d_camera_poly &p){vertex v[8];return clipped(p,v)!=0;}
    static bool project(const gpu3d_camera_poly &p,gpu3d_poly &g){
        vertex v[8];int n=clipped(p,v);g=p.material;g.n=uint8_t(n);if(!n)return false;
        project_vertices(v,n,p.center,g);
        return true;
    }
    static void project_vertices(const vertex *v,int n,const float (&center)[2],gpu3d_poly &g){
        for(int i=0;i<n;++i){
            const auto point=project_point(v[i],center,g.renderer&2);
            g.x[i]=point.x;g.y[i]=point.y;
            if(g.renderer&2){g.z[i]=point.q;g.u[i]=point.u;g.v[i]=point.v;}
        }
    }
    static gpu3d_projected_point project_point(const vertex &v,const float (&center)[2],bool textured){
        const float denominator=v.z+std::numeric_limits<float>::min();
        // Keep the arcade reference's separate rounded divisions.
        gpu3d_projected_point point{center[0]+v.x/denominator,center[1]-v.y/denominator,0,0,0};
        if(textured){
            point.q=1.f/denominator;
            point.u=v.u*point.q*(1.f/8.f);point.v=v.v*point.q*(1.f/8.f);
        }
        return point;
    }
    static void project_compact(const gpu3d_context_camera_poly &r,const gpu3d_camera_context &context,
            const camera_tables &tables,gpu3d_poly &g){
        vertex a[16],v[8];
        for(unsigned i=0;i<r.n;++i)
            a[i]={r.position[i][0],r.position[i][1],r.position[i][2],float(r.uv[i][0]),float(r.uv[i][1])};
        const int n=clipped_vertices(a,r.n,context.planes,v);
        if(!n){g.n=0;return;}

        auto material=static_cast<const gpu3d_camera_attributes &>(r);
        resolve_material(material,tables.palette.data(),tables.xlat_view,tables.gamma_view,tables.hair);
        g={};
        g.n=uint8_t(n);g.renderer=(material.header[0]>>13)&3;g.checker=(material.header[0]>>15)&1;
        std::copy_n(context.viewport,4,g.vp);
        fill_attributes(material,g);
        project_vertices(v,n,context.center,g);
    }
    bool prepare_projected(gpu3d_projected_stream &out){
        if(!shared_compact||shared_compact->polygons.size()>32767||!mesh_polygons.empty()||
           std::getenv("VF2_REFERENCE_CAMERA_SORT"))return false;
        const auto &records=shared_compact->polygons;
        sort_camera_indices(records);
        out.polygons.resize(order.size());
        const int count=int(order.size());
        const int lanes=std::max(1,std::min(count,row_pool::get().threads()*4));
        out.lanes.resize(lanes);
        parallel_tasks(lanes,[&](int lane){
            const int begin=count*lane/lanes,end=count*(lane+1)/lanes;
            auto &points=out.lanes[lane];
            points.clear();
            // At most eight points per clipped polygon. Keep record pointers
            // stable until the renderer has consumed the completed stream.
            points.reserve(size_t(end-begin)*8);
            for(int i=begin;i<end;++i){
                const auto &r=records[order[i]];
                const auto &context=resolved_contexts[r.context];
                auto &g=out.polygons[i];
                vertex a[16],v[8];
                for(unsigned j=0;j<r.n;++j)
                    a[j]={r.position[j][0],r.position[j][1],r.position[j][2],float(r.uv[j][0]),float(r.uv[j][1])};
                const int n=clipped_vertices(a,r.n,context.planes,v);
                g.n=uint8_t(n);
                if(!n)continue;
                auto material=static_cast<const gpu3d_camera_attributes &>(r);
                resolve_material(material,captured_tables->palette.data(),captured_tables->xlat_view,captured_tables->gamma_view,captured_tables->hair);
                g.renderer=(material.header[0]>>13)&3;g.checker=(material.header[0]>>15)&1;
                std::copy_n(context.viewport,4,g.vp);
                fill_attributes(material,g);
                const size_t first=points.size();
                for(int j=0;j<n;++j)points.push_back(project_point(v[j],context.center,g.renderer&2));
                g.points=points.data()+first;
            }
        });
        out.polygons.erase(std::remove_if(out.polygons.begin(),out.polygons.end(),[](const auto &p){return !p.n;}),out.polygons.end());
        return true;
    }
    static uint32_t visible_count(const std::vector<gpu3d_camera_poly> &list){
        uint32_t count=0;vertex v[8];
        for(const auto &p:list)if(clipped(p,v)&&++count==32767)break;
        return count;
    }
    void prepare(std::vector<gpu3d_poly> &out,bool direct=false){
        if(shared_compact){
            const auto &records=shared_compact->polygons;
            prepare_records(out,direct,records,[&](size_t i,gpu3d_poly &result){
                const auto &r=records[i];
                project_compact(r,resolved_contexts[r.context],*captured_tables,result);
            });
            return;
        }
        prepare_records(out,direct,compact_polygons,[&](size_t i,gpu3d_poly &result){
            if(!compact){project(polygons[i],result);return;}
            gpu3d_camera_poly packet;
            expand_compact(compact_polygons[i],packet);
            project(packet,result);
        });
    }
private:
    template<class Records,class Project>
    void prepare_records(std::vector<gpu3d_poly> &out,bool direct,const Records &compact_records,Project project_at){
        mesh_projected_sources.clear();
        const bool track_mesh=compact&&!mesh_polygons.empty()&&mesh_polygons.size()==compact_polygons.size();
        auto mesh_source=[&](size_t i){
            const auto &r=compact_polygons[i];
            if(mesh_polygons[i].object==gpu3d_mesh_polygon::invalid)return gpu3d_mesh_polygon::invalid;
            // A polygon crossing a frustum plane can keep the same vertex
            // count, yet its new vertices cannot address the resident fan.
            for(const auto &plane:r.planes)
                for(unsigned v=0;v<r.n;++v){
                    const auto &p=r.position[v];
                    if(!((p[0]*plane[0]+p[1]*plane[1])+p[2]*plane[2]>=plane[3]))
                        return gpu3d_mesh_polygon::invalid;
                }
            return uint32_t(i);
        };
        const size_t count=compact?compact_records.size():polygons.size();
        auto before=[&](size_t a,size_t b){
            const auto wx=compact?compact_records[a].window:polygons[a].window,wy=compact?compact_records[b].window:polygons[b].window;
            const auto dx=compact?compact_records[a].depth:polygons[a].depth,dy=compact?compact_records[b].depth:polygons[b].depth;
            if(wx!=wy)return wx>wy;
            if(dx!=dy)return dx<dy;
            return a>b;
        };
        if(direct&&count<=32767){
            // Below hardware capacity, hidden/culled polygons cannot displace
            // another survivor. Sort immutable source indices first and write
            // projection results directly into their final ordered positions.
            static const bool reference_sort=std::getenv("VF2_REFERENCE_CAMERA_SORT")!=nullptr;
            if(!reference_sort){
                if(compact)sort_camera_indices(compact_records);
                else sort_camera_indices(polygons);
            }else{
                order.clear();order.reserve(count);
                for(size_t i=0;i<count;++i)if((compact?compact_records[i].window:polygons[i].window)<=final_window)order.push_back(i);
                std::sort(order.begin(),order.end(),before);
            }
            out.resize(order.size());
            parallel_rows(int(order.size()),[&](int a,int b){
                for(int i=a;i<b;++i){
                    project_at(order[i],out[i]);
                }
            },4);
            if(track_mesh){
                mesh_projected_sources.reserve(order.size());
                for(size_t i=0;i<order.size();++i)
                    if(out[i].n)mesh_projected_sources.push_back(mesh_source(order[i]));
            }
            out.erase(std::remove_if(out.begin(),out.end(),[](const auto &p){return !p.n;}),out.end());return;
        }
        prepared.resize(count);
        parallel_rows(int(count),[&](int a,int b){
            for(int i=a;i<b;++i){
                project_at(i,prepared[i]);
            }
        },4);
        order.clear();order.reserve(count);
        // The hardware keeps the first 32,767 surviving polygons, then walks
        // windows backwards and depth buckets forwards (newest polygon first).
        size_t survived=0;
        for(size_t i=0;i<count;++i)if(prepared[i].n){
            if(++survived>32767)break;
            if((compact?compact_records[i].window:polygons[i].window)<=final_window)order.push_back(i);
        }
        std::sort(order.begin(),order.end(),before);
        out.clear();
        out.reserve(order.size());
        if(track_mesh)mesh_projected_sources.reserve(order.size());
        for(size_t i:order){
            out.push_back(prepared[i]);
            if(track_mesh)mesh_projected_sources.push_back(mesh_source(i));
        }
    }
};
