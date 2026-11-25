// Scalar reference and optional ARMv7 NEON for the geometry engine's 3x4 matrix.
#pragma once
#include <bit>
#include <cstdint>
#include <cstdlib>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace geometry_math {
class matrix3x4 {
    const float *m;
    bool neon=false;
#if defined(__ARM_NEON)
    float32x4_t columns[4];
#endif
    // NEON flushes denormals. Bounding nonzero operands to [2^-30,2^30]
    // keeps products and every possible cancellation normal (or exactly zero),
    // and prevents overflow. Exceptional/very small/large values use VFP.
    static bool bounded(float value){
        uint32_t bits=std::bit_cast<uint32_t>(value)&0x7fffffffu;
        return !bits||(bits>=0x30800000u&&bits<=0x4e800000u);
    }
public:
    static bool enabled(){static const bool value=std::getenv("VF2_GEOMETRY_NEON")!=nullptr;return value;}
    explicit matrix3x4(const float *matrix,bool accelerate=enabled()):m(matrix){
#if defined(__ARM_NEON)
        neon=accelerate;
        for(unsigned i=0;i<12&&neon;i++)neon=bounded(m[i]);
        if(neon){
            float32x4_t a=vld1q_f32(m),b=vld1q_f32(m+4),c=vld1q_f32(m+8);
            columns[0]=a;columns[1]=vextq_f32(a,b,3);columns[2]=vextq_f32(b,c,2);columns[3]=vextq_f32(c,c,1);
        }
#else
        (void)accelerate;
#endif
    }
    bool accelerated()const{return neon;}
    void apply(float &x,float &y,float &z,bool translate)const {
#if defined(__ARM_NEON)
        if(neon&&bounded(x)&&bounded(y)&&bounded(z)){
            // Separate multiply/add intrinsics preserve the scalar operation
            // order; no reassociation, reciprocal estimate or fused rounding.
            float32x4_t sum=vaddq_f32(vmulq_n_f32(columns[0],x),vmulq_n_f32(columns[1],y));
            sum=vaddq_f32(sum,vmulq_n_f32(columns[2],z));
            if(translate)sum=vaddq_f32(sum,columns[3]);
            x=vgetq_lane_f32(sum,0);y=vgetq_lane_f32(sum,1);z=vgetq_lane_f32(sum,2);return;
        }
#endif
        float tx=(x*m[0])+(y*m[3])+(z*m[6]);
        float ty=(x*m[1])+(y*m[4])+(z*m[7]);
        float tz=(x*m[2])+(y*m[5])+(z*m[8]);
        if(translate){tx+=m[9];ty+=m[10];tz+=m[11];}
        x=tx;y=ty;z=tz;
    }
};
}
