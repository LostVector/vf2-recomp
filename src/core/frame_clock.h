// Full-width storage with modular low-limb comparisons inside a proven time
// window. Ordering is exact whenever the compared gap is below 2^63 attoseconds.
#pragma once
#include "wide_int.h"
#include <bit>
#include <cstdint>
namespace vf2 {
struct frame_clock {
    static constexpr bool less(uint64_t a,uint64_t b){return std::bit_cast<int64_t>(a-b)<0;}
    static constexpr bool near(uint128_t a,uint128_t b,uint64_t radius){return a>=b?a-b<=uint128_t(radius):b-a<=uint128_t(radius);}
    static constexpr void advance(uint128_t &clock,uint64_t elapsed){
#if !defined(__SIZEOF_INT128__) && !defined(VF2_USE_BOOST_U128)
        const uint64_t before=clock.lo;clock.lo+=elapsed;
        if(clock.lo<before)++clock.hi;
#else
        clock+=uint128_t(elapsed);
#endif
    }
    // The scheduler's target only decreases from its initial deadline. Recover
    // its absolute value once per pass, including across a low-limb rollover.
    static constexpr uint128_t restore(uint128_t initial,uint64_t target){return initial-uint128_t(uint64_t(initial)-target);}
};
}
