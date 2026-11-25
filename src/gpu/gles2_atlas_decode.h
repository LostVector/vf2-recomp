#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vf2_batch {
struct atlas_source_signature { uint64_t key=0,hash=0; };

inline unsigned atlas_word_address(unsigned x,unsigned y){
    const unsigned px=x&2047,py=y&1023,right=px>=1024?1024:0,xo=(px-right)>>1;
    return (((((py>>1)<<9)^(right<<8))|xo)>>1)&0x7ffff;
}

inline uint32_t atlas_luma_row(uint32_t word)
{
    return ((word >> 8) & 0x00f000f0u) | ((word << 4) & 0xf000f000u);
}

// Source words are in 4-column, 2-row blocks. The scalar path also
// preserves the tile-relative nibble phases for partial edge blocks.
inline void decode_atlas_snapshot(uint8_t *output, const uint32_t *words,
                                  unsigned x, unsigned y, unsigned width, unsigned height,
                                  unsigned channels, bool transparent)
{
    const unsigned block_width = ((x & 3) + width + 3) / 4;
    if constexpr (std::endian::native == std::endian::little) {
        if (channels == 1 && ((x | width) & 3) == 0 && ((y | height) & 1) == 0) {
            for (unsigned v = 0; v < height; v += 2) {
                auto *top = output + size_t(v) * width;
                auto *bottom = top + width;
                for (unsigned u = 0; u < width; u += 4) {
                    const uint32_t word = *words++;
                    const uint32_t top_row = atlas_luma_row(word);
                    const uint32_t bottom_row = atlas_luma_row(word << 8);
                    std::memcpy(top + u, &top_row, sizeof(top_row));
                    std::memcpy(bottom + u, &bottom_row, sizeof(bottom_row));
                }
            }
            return;
        }
    }
    const unsigned x_phase = x & 3, y_phase = y & 1;
    for (unsigned v = 0; v < height; ++v) {
        const auto *row = words + size_t((y_phase + v) / 2) * block_width;
        const unsigned vertical = (~v & 1) << 3;
        for (unsigned u = 0; u < width; ++u) {
            const unsigned column = x_phase + u;
            const unsigned shift = ((column & 2) << 3) + ((~u & 1) << 2) + vertical;
            const unsigned index = (row[column / 4] >> shift) & 15;
            *output++ = uint8_t(index << 4);
            if (channels == 2) *output++ = transparent && index == 15 ? 0 : 255;
        }
    }
}
}
