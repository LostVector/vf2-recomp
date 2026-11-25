// Exact cycle-to-attosecond conversion. Short products fit in one 64-bit
// limb; the absolute scheduler clock and long/signed products remain 128-bit.
#pragma once
#include "wide_int.h"
#include <algorithm>
#include <cstdint>
#include <limits>
namespace vf2 {
class cycle_duration {
    uint64_t period=0;
    uint32_t short_limit=0;
public:
    void set(uint64_t attoseconds_per_cycle){
        period=attoseconds_per_cycle;
        short_limit=uint32_t(std::min<uint64_t>(std::numeric_limits<int>::max(),period?UINT64_MAX/period:UINT64_MAX));
    }
    uint128_t operator()(int cycles)const{
        if(uint32_t(cycles)<=short_limit)return uint128_t(uint64_t(uint32_t(cycles))*period);
        return uint128_t(cycles)*uint128_t(period);
    }
};
}
