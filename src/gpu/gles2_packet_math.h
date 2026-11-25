#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace vf2_packet_math {
inline bool reference(){static const bool enabled=std::getenv("VF2_REFERENCE_PACKET_MATH")!=nullptr;return enabled;}
inline int texture_mip_limit(uint16_t width,uint16_t height){
    const uint16_t smaller=width<height?width:height;
    const int bits=std::bit_width(smaller);
    return bits>2?bits-2:0;
}
// LOD boundaries are exp2(-k/256), with integer k. The fractional result is
// independent of the integral exponent; scaling a normal double by a power
// of two changes only its exponent bits. Outside the validated range, use libm.
inline double lod_boundary(int64_t k){
    if(reference()||k < -65536||k > 65535)return std::exp2(-double(k)/256);
    static const std::array<uint64_t,256> fractions=[](){
        std::array<uint64_t,256> out{};
        for(unsigned i=0;i<256;++i)out[i]=std::bit_cast<uint64_t>(std::exp2(-double(i)/256));
        return out;
    }();
    const unsigned fraction=uint64_t(k)&255;
    const int64_t exponent=(k-int64_t(fraction))/256;
    return std::bit_cast<double>(uint64_t(int64_t(fractions[fraction])-exponent*int64_t(1ull<<52)));
}
inline double micro_scale(unsigned level){
    static constexpr double scale[4]={2,4,16,256};
    return !reference()&&level<4?scale[level]:std::exp2(std::exp2(double(level)));
}
inline bool odd_cell(double cell){
    const double a=std::abs(cell);
    return reference()?std::fmod(a,2.)>=1:a-std::floor(a*.5)*2>=1;
}
}
