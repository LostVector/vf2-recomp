// ARM NEON vs scalar geometry, with strict per-operation rounding and fallback.
#include "../../src/machine/geometry_math.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>
static void check(const std::array<float,12> &m,std::array<float,3> v,unsigned &accelerated){
    geometry_math::matrix3x4 scalar(m.data(),false),vector(m.data(),true);accelerated+=vector.accelerated();
    for(bool translate:{false,true}){
        auto a=v,b=v;scalar.apply(a[0],a[1],a[2],translate);vector.apply(b[0],b[1],b[2],translate);
        for(unsigned i=0;i<3;i++)if(std::bit_cast<uint32_t>(a[i])!=std::bit_cast<uint32_t>(b[i])){
            std::fprintf(stderr,"Geometry differs at lane %u, translate %d: %08x vs %08x\n",i,translate,std::bit_cast<uint32_t>(a[i]),std::bit_cast<uint32_t>(b[i]));std::exit(1);
        }
    }
}
int main(){
    std::mt19937 rng(0xac13d);unsigned accelerated=0;
    for(unsigned i=0;i<100000;i++){
        auto number=[&](){return std::bit_cast<float>((rng()&0x807fffffu)|((97+rng()%60)<<23));};
        std::array<float,12> m;for(auto &v:m)v=number();check(m,{number(),number(),number()},accelerated);
        // Exact cancellation with and without translation.
        for(unsigned c=0;c<3;c++)m[c+3]=-m[c];check(m,{1,1,0},accelerated);
    }
    std::array<float,12> identity={1,0,0,0,1,0,0,0,1,0,0,0};
    const uint32_t edges[]={0,0x80000000,1,0x80000001,0x007fffff,0x00800000,0x30800000,0x4e800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc00001};
    for(auto bits:edges){
        float value=std::bit_cast<float>(bits);check(identity,{value,0,-0.f},accelerated);
        auto m=identity;m[0]=value;check(m,{1,2,3},accelerated);
    }
#if defined(__ARM_NEON)
    if(accelerated<200000)return 2;
#endif
    std::printf("Geometry bit-exact: 200000 random/cancellation matrices plus fallback edges; %u NEON matrices\n",accelerated);
}
