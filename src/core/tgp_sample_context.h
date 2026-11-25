// Optional DSP location tag for CPU-time sampling. The hot execution path only
// writes it in an explicit diagnostic mode; the signal handler reads an atomic.
#pragma once
#include <atomic>
#include <cstdint>
namespace vf2_tgp_sample {
inline std::atomic<uint32_t> location{};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
inline void instruction(uint16_t pc,uint8_t &command){
    if(pc==0x35)command=255;
    else if(pc>=0x3f&&pc<0xaf)command=uint8_t(pc-0x3f);
    location.store(0x80000000u|(uint32_t(command)<<12)|(pc&4095),std::memory_order_relaxed);
}
// Bit 30 marks command-only samples, which do not carry a valid DSP PC.
inline void command(uint8_t value){location.store(0xc0000000u|(uint32_t(value)<<12),std::memory_order_relaxed);}
inline void leave(){location.store(0,std::memory_order_relaxed);}
}
