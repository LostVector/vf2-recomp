// Exact unsigned 64/32 division when the divisor is fixed across many calls.
#pragma once
#include <cstdint>
#include <cstdlib>
namespace vf2 {
class constant_divisor {
    uint32_t divisor=1;
    uint64_t reciprocal=0;
    static constexpr uint64_t high(uint64_t a,uint64_t b){
        uint64_t a0=uint32_t(a),a1=a>>32,b0=uint32_t(b),b1=b>>32;
        uint64_t t=a1*b0+((a0*b0)>>32);
        uint64_t middle=uint32_t(t)+a0*b1;
        return a1*b1+(t>>32)+(middle>>32);
    }
public:
    void set(uint32_t value){
        if(!value)std::abort();divisor=value;
        reciprocal=value==1?0:UINT64_MAX/value+(UINT64_MAX%value==value-1);
    }
    uint64_t divide_frame(uint64_t value)const {
        // The scaled reciprocal turns a frame-sized dividend into a single
        // 32x32 multiply. With these bounds, truncating the dividend costs
        // less than 1/2 and rounding the reciprocal costs less than 1/2.
        // Their sum keeps the estimate within one of the true quotient.
        if(divisor<(1u<<19)||value>=(1ull<<49))return (*this)(value);
        const uint32_t mantissa=uint32_t(value>>18);
        const uint32_t multiplier=uint32_t(reciprocal>>14);
        const uint32_t quotient=uint32_t((uint64_t(mantissa)*multiplier)>>32);
        return quotient+(value-uint64_t(quotient)*divisor>=divisor);
    }

    uint64_t operator()(uint64_t value)const {
        if(divisor==1)return value;
        // floor(2^64/d) yields the true quotient or one less. Its product
        // cannot exceed value; one remainder comparison gives exact division.
        uint64_t quotient=high(value,reciprocal);
        return quotient+(value-quotient*divisor>=divisor);
    }
};
}
