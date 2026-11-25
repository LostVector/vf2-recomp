#include "../../src/gpu/gles2_atlas_edges.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

static void require(bool ok){
    if(!ok){std::fputs("Atlas texel comparison failed\n",stderr);std::exit(1);}
}
static int neighbour(int offset,unsigned extent,bool wrap){
    return wrap?(offset+int(extent))%int(extent):std::clamp(offset,0,int(extent)-1);
}
int main(){
    std::mt19937 random(7321);
    size_t cases=0;
    for(unsigned width:{1u,2u,3u,4u,7u,8u,16u,127u,256u,513u})
        for(unsigned height:{1u,2u,3u,4u,5u,8u,32u,128u,256u})
            for(unsigned mode=0;mode<4;++mode)
                for(unsigned channels:{1u,2u}){
                    const bool wrap_x=mode&1,wrap_y=mode&2;
                    std::vector<uint8_t> expected(size_t(width)*height*channels);
                    for(size_t i=0;i<expected.size();++i)
                        expected[i]=channels==2&&(i&1)?(random()%3?0:255):uint8_t(random());
                    auto actual=expected;
                    if(channels==2){
                        const auto original=expected;
                        for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
                            const size_t pixel=(size_t(y)*width+x)*2;
                            if(original[pixel+1])continue;
                            const int offsets[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
                            for(const auto &offset:offsets){
                                const unsigned u=neighbour(int(x)+offset[0],width,wrap_x);
                                const unsigned v=neighbour(int(y)+offset[1],height,wrap_y);
                                const size_t other=(size_t(v)*width+u)*2;
                                if(original[other+1]){expected[pixel]=original[other];break;}
                            }
                        }
                        vf2_batch::fill_transparent_edges(actual.data(),width,height,wrap_x,wrap_y);
                        require(actual==expected);
                    }
                    std::vector<uint8_t> border(size_t(width+2)*(height+2)*channels);
                    vf2_batch::copy_tile_border(border.data(),actual.data(),width,height,channels,wrap_x,wrap_y);
                    for(unsigned y=0;y<height+2;++y)for(unsigned x=0;x<width+2;++x){
                        const unsigned u=neighbour(int(x)-1,width,wrap_x);
                        const unsigned v=neighbour(int(y)-1,height,wrap_y);
                        for(unsigned c=0;c<channels;++c)
                            require(border[(size_t(y)*(width+2)+x)*channels+c]==expected[(size_t(v)*width+u)*channels+c]);
                    }
                    ++cases;
                }
    std::printf("Atlas texel rows matched %zu wrap/clamp, transparency and border cases\n",cases);
}
