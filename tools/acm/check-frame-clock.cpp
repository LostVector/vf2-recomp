#include "../../src/core/frame_clock.h"
#include <array>
#include <cstdio>
#include <random>
int main(){
    using T=vf2::uint128_t;std::mt19937_64 random(0xf4a6e);uint64_t cases=0;
    auto check=[&](T a,T b){
        const T delta=a>=b?a-b:b-a;
        if(delta<T(uint64_t(1)<<63)&&vf2::frame_clock::less(uint64_t(a),uint64_t(b))!=(a<b)){std::fputs("Frame clock ordering mismatch\n",stderr);std::exit(1);}
        for(uint64_t radius:std::array<uint64_t,4>{0,1,1000000000000000000ull,UINT64_MAX})
            if(vf2::frame_clock::near(a,b,radius)!=(delta<=T(radius))){std::fputs("Frame clock guard mismatch\n",stderr);std::exit(1);}
        ++cases;
    };
    const uint64_t offsets[]={0,1,2,40000000000ull,1000000000000000000ull,3000000000000000000ull,(uint64_t(1)<<63)-1};
    for(T epoch:{T(0),T(1)<<64,(T(1)<<64)-1,T(1)<<96})for(uint64_t x:offsets)for(uint64_t y:offsets)check(epoch+x,epoch+y);
    for(unsigned i=0;i<300000;++i){
        T origin=(T(random()&0xffff)<<64)|random();
        uint64_t distance=random()%3000000000000000000ull;
        T a=origin+distance,b=origin;
        check(a,b);check(b,a);
        T advanced=origin;vf2::frame_clock::advance(advanced,distance);
        if(advanced!=origin+T(distance)){std::fputs("Frame clock advance mismatch\n",stderr);return 1;}
        T wrap=~T(0)-random()%100;T expected=wrap+T(distance);vf2::frame_clock::advance(wrap,distance);
        if(wrap!=expected){std::fputs("Frame clock carry mismatch\n",stderr);return 1;}
        if(vf2::frame_clock::restore(a,uint64_t(b))!=b){std::fputs("Frame clock reconstruction mismatch\n",stderr);return 1;}
        check((T(random())<<64)|random(),(T(random())<<64)|random());
    }
    std::printf("Frame clocks: %llu exact ordering/window cases and 300000 target reconstructions and 600000 carries/advances pass\n",(unsigned long long)cases);
}
