// RGB shade ramps derived from the Model 2 colour-translation and gamma RAM.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>

namespace vf2_batch {
class palette_shades {
    struct entry {
        std::array<uint32_t,64> colours;
        uint16_t key=0xffff;
    };
    std::array<entry,256> entries;

    const std::array<uint32_t,64> &find(uint16_t pal,const uint16_t *xlat,const uint8_t *gamma)
    {
        const uint16_t key=pal&0x7fff;
        auto &slot=entries[(uint32_t(key)*0x9e3779b9u)>>24];
        if(slot.key==key)return slot.colours;

        const auto *red=xlat+((key&31u)<<8);
        const auto *green=xlat+0x2000+(((key>>5)&31u)<<8);
        const auto *blue=xlat+0x4000+(((key>>10)&31u)<<8);
        for(unsigned light=0;light<slot.colours.size();++light){
            const std::array<uint8_t,4> rgba={gamma[red[light]&255],gamma[green[light]&255],
                                              gamma[blue[light]&255],255};
            slot.colours[light]=std::bit_cast<uint32_t>(rgba);
        }
        slot.key=key;
        return slot.colours;
    }

public:
    void reset()
    {
        for(auto &entry:entries)entry.key=0xffff;
    }

    void write(uint8_t *rgba,unsigned count,uint16_t pal,uint16_t base,uint16_t weight,
               const uint8_t *luma,const uint16_t *xlat,const uint8_t *gamma)
    {
        const auto &colours=find(pal,xlat,gamma);
        for(unsigned i=0;i<count;++i){
            const unsigned light=std::min((unsigned(luma[(base+i)&0x7fff])*weight)>>8,63u);
            std::memcpy(rgba+i*4,&colours[light],4);
        }
    }
};
}
