#pragma once
#include "gpu3d.h"
#include <vector>

// Render-side projection only. The board and the general backends retain
// their original polygon format and serialized state.
struct gpu3d_projected_point {float x,y,q,u,v;};

struct gpu3d_projected_poly {
    const gpu3d_projected_point *points;
    uint32_t colour;
    int32_t texlod;
    int16_t vp[4];
    uint16_t pal,lumabase,luma,texw,texh,texx,texy,utexx,utexy;
    uint8_t n,renderer,checker,sheet,wrapx,wrapy,mirrorx,mirrory,utex,utexminlod;
};

inline gpu3d_projected_point projected_point(const gpu3d_poly &p,size_t i){
    return {p.x[i],p.y[i],p.z[i],p.u[i],p.v[i]};
}
inline const gpu3d_projected_point &projected_point(const gpu3d_projected_poly &p,size_t i){
    return p.points[i];
}

struct gpu3d_projected_stream {
    gpu3d_projected_stream()=default;
    gpu3d_projected_stream(const gpu3d_projected_stream &)=delete;
    gpu3d_projected_stream &operator=(const gpu3d_projected_stream &)=delete;

    std::vector<gpu3d_projected_poly> polygons;
    std::vector<std::vector<gpu3d_projected_point>> lanes;

    void expand(std::vector<gpu3d_poly> &out)const{
        out.resize(polygons.size());
        for(size_t i=0;i<polygons.size();++i){
            const auto &p=polygons[i];
            auto &g=out[i];
            g={};
            g.colour=p.colour;g.texlod=p.texlod;
            for(unsigned j=0;j<4;++j)g.vp[j]=p.vp[j];
            g.pal=p.pal;g.lumabase=p.lumabase;g.luma=p.luma;
            g.texw=p.texw;g.texh=p.texh;g.texx=p.texx;g.texy=p.texy;g.utexx=p.utexx;g.utexy=p.utexy;
            g.n=p.n;g.renderer=p.renderer;g.checker=p.checker;g.sheet=p.sheet;
            g.wrapx=p.wrapx;g.wrapy=p.wrapy;g.mirrorx=p.mirrorx;g.mirrory=p.mirrory;
            g.utex=p.utex;g.utexminlod=p.utexminlod;
            for(unsigned j=0;j<p.n;++j){
                const auto &v=p.points[j];
                g.x[j]=v.x;g.y[j]=v.y;g.z[j]=v.q;g.u[j]=v.u;g.v[j]=v.v;
            }
        }
    }
};

bool gles2_prepare_projected(gpu3d &,const gpu3d_projected_stream &,const gpu3d_frame &,const gpu3d_rams &);
bool gles2_submit_projected(gpu3d &,const gpu3d_projected_stream &,const gpu3d_frame &,int);
