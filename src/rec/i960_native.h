// The recompile done on the player's machine: VF2's i960 program, found with the shipped address
// map (assets/vf2_map.txt, src/rec/i960_analyse.h), made into native code (ARM64 or x86-64) with
// sljit when the game starts. Nothing made from the ROM ships; this is the job tools/recomp does
// into C++ for our own builds, and the lockstep check (vf2lock --native) covers it the same way.
#pragma once
#include "i960_rec.h"
#include "i960_analyse.h"

namespace i960rec {

struct native_stats { size_t functions = 0, instructions = 0, native = 0, code_bytes = 0; double ms = 0; };

// rd: the program as the CPU sees it (the program ROM, 2 MB). Makes every function of the map's
// program into native code and installs it in rt. False on failure (the interpreter runs it all).
bool build_native(runtime &rt, uint32_t (*rd)(void *, uint32_t), void *ctx, uint32_t limit, const hints &map, native_stats *stats = nullptr);

// Complete 23-cycle rounds of the recognized timer poll. Leave the
// potentially exiting iteration and partial slices to the normal execution.
inline constexpr uint32_t timer_poll_mask=0xfffff;
inline constexpr uint32_t timer_poll_elapsed(uint32_t counter,uint32_t limit,int budget){
    constexpr unsigned cost=23;
    if(budget<=int(cost)||counter>=timer_poll_mask||counter<=limit)return 0;
    const unsigned by_budget=unsigned(budget-1)/cost;
    const unsigned before_exit=(counter-limit-1)/cost;
    return (by_budget<before_exit?by_budget:before_exit)*cost;
}

} // namespace i960rec
