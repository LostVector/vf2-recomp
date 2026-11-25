// Exact differences between absolute scheduler clocks. The clock remains
// 128-bit, including across the low limb's ~18-second rollover. Most CPU
// visits are only a few cycles apart: test their difference in one limb.
#pragma once
#include "wide_int.h"
#include <cstdint>
namespace vf2 {
inline bool scheduler_gap(uint128_t target,uint128_t local,uint64_t period,uint128_t &delta){
#if defined(__arm__) && defined(__GCC_ASM_FLAG_OUTPUTS__)
    uint32_t a=uint32_t(target),b=uint32_t(target>>32),c=uint32_t(target>>64),d=uint32_t(target>>96);
    const uint32_t e=uint32_t(local),f=uint32_t(local>>32),g=uint32_t(local>>64),h=uint32_t(local>>96);
    bool forward;
    // Carry from the last subtraction supplies the ordering test as well as
    // the difference. Early-clobber prevents a lower result destroying any
    // still-unread source limb. There is no wraparound assumption.
    asm("subs %0,%0,%5\n\tsbcs %1,%1,%6\n\tsbcs %2,%2,%7\n\tsbcs %3,%3,%8"
        : "+&r"(a),"+&r"(b),"+&r"(c),"+&r"(d),"=@cccs"(forward)
        : "r"(e),"r"(f),"r"(g),"r"(h));
    delta=(uint128_t(d)<<96)|(uint128_t(c)<<64)|(uint128_t(b)<<32)|a;
    return forward && ((c|d)!=0 || uint64_t(delta)>=period);
#else
    delta=target-local;
    return target>=local && delta>=uint128_t(period);
#endif
}
}
