// Differential camera clipping: preserved reference arithmetic versus buffer reuse.
#include "../../src/gpu/geometry_frame.h"
#include <bit>
#include <cstdio>
namespace reference {
struct vertex {float x,y,z,u,v;};
    static int clipped(const gpu3d_camera_poly &p,vertex *result){
        vertex a[16],b[16];vertex *in=a,*out=b;int n=p.material.n;
        for(int i=0;i<n;++i)in[i]={p.material.x[i],p.material.y[i],p.material.z[i],p.material.u[i],p.material.v[i]};
        for(const auto &plane:p.planes){
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
    }    static bool project(const gpu3d_camera_poly &p,gpu3d_poly &g){
        vertex v[8];int n=clipped(p,v);g=p.material;g.n=uint8_t(n);if(!n)return false;
        for(int i=0;i<n;++i){
            const float denominator=v[i].z+std::numeric_limits<float>::min();
            // Keep the arcade reference's separate rounded divisions.
            g.x[i]=p.center[0]+v[i].x/denominator;
            g.y[i]=p.center[1]-v[i].y/denominator;
            if(g.renderer&2){
                g.z[i]=1.0f/denominator;
                g.u[i]=v[i].u*g.z[i]*(1.0f/8.0f);
                g.v[i]=v[i].v*g.z[i]*(1.0f/8.0f);
            }
        }
        return true;
    }
}
int main(){
    uint32_t seed=0x61837291;auto rng=[&](){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;};
    auto number=[&](){return float(int32_t(rng()%8193)-4096)/128;};
    for(unsigned t=0;t<100000;++t){
        gpu3d_camera_poly p{};p.material.n=3+rng()%6;p.material.renderer=(t&1)?2:0;
        p.center[0]=number();p.center[1]=number();
        for(unsigned i=0;i<p.material.n;++i){p.material.x[i]=number();p.material.y[i]=number();p.material.z[i]=number();p.material.u[i]=number();p.material.v[i]=number();}
        for(auto &plane:p.planes)for(float &v:plane)v=number();
        if(t%4==0)for(auto &plane:p.planes){plane[0]=plane[1]=plane[2]=0;plane[3]=-1;} // all inside
        if(t%7==0)p.planes[0][3]=0; // exact plane boundary with zero normal
        if(t%13==0)p.material.x[rng()%p.material.n]=std::numeric_limits<float>::quiet_NaN();
        if(t%17==0)p.material.z[rng()%p.material.n]=std::numeric_limits<float>::infinity();
        if(t%19==0)p.planes[rng()%4][rng()%4]=std::numeric_limits<float>::quiet_NaN();
        gpu3d_poly a{},b{};bool ra=reference::project(p,a),rb=gpu3d_geometry_frame::project(p,b);
        bool same=ra==rb&&a.n==b.n;
        for(unsigned i=0;i<a.n&&same;++i){const float av[]={a.x[i],a.y[i],a.z[i],a.u[i],a.v[i]},bv[]={b.x[i],b.y[i],b.z[i],b.u[i],b.v[i]};for(unsigned j=0;j<5;++j)same&=std::bit_cast<uint32_t>(av[j])==std::bit_cast<uint32_t>(bv[j]);}
        if(!same){std::fprintf(stderr,"Clipping differs in case %u: %u/%u vertices\n",t,a.n,b.n);return 1;}
    }
    std::puts("Camera clipping exact across 100000 cases: inside, crossings, boundaries, NaN and infinity");
}
