#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vf2_batch {
inline void fill_transparent_edges(uint8_t *pixels,unsigned width,unsigned height,bool wrap_x,bool wrap_y){
    const size_t pitch=size_t(width)*2;
    for(unsigned y=0;y<height;++y){
        auto *row=pixels+size_t(y)*pitch;
        const auto *above=pixels+size_t(y?y-1:(wrap_y?height-1:0))*pitch;
        const auto *below=pixels+size_t(y+1<height?y+1:(wrap_y?0:height-1))*pitch;
        for(unsigned x=0;x<width;++x){
            auto *pixel=row+2*x;
            if(pixel[1])continue;
            const auto *left=x?pixel-2:row+(wrap_x?pitch-2:0);
            const auto *right=x+1<width?pixel+2:row+(wrap_x?0:pitch-2);
            if(right[1])pixel[0]=right[0];
            else if(left[1])pixel[0]=left[0];
            else if(below[2*x+1])pixel[0]=below[2*x];
            else if(above[2*x+1])pixel[0]=above[2*x];
        }
    }
}

inline void copy_tile_border(uint8_t *output,const uint8_t *pixels,unsigned width,unsigned height,
                             unsigned channels,bool wrap_x,bool wrap_y){
    const size_t pitch=size_t(width)*channels;
    const size_t bordered_pitch=pitch+2*channels;
    const size_t left=wrap_x?pitch-channels:0,right=wrap_x?0:pitch-channels;
    for(unsigned y=0;y<height+2;++y){
        const unsigned row=y==0?(wrap_y?height-1:0):y==height+1?(wrap_y?0:height-1):y-1;
        const auto *source=pixels+size_t(row)*pitch;
        auto *destination=output+size_t(y)*bordered_pitch;
        std::memcpy(destination,source+left,channels);
        std::memcpy(destination+channels,source,pitch);
        std::memcpy(destination+channels+pitch,source+right,channels);
    }
}
}
