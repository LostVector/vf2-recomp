#include "../../src/core/constant_divisor.h"
#include <cstdio>
#include <random>
int main(){
    std::mt19937_64 rng(0xacd1);vf2::constant_divisor fast;
    const uint32_t edges[]={1,2,3,4,7,8,15,16,17,31,32,33,0x7fffffff,0x80000000,0xfffffffe,0xffffffff,1250000000,1785714285,1874999962,1953125000,0x7ffff,0x80000,0x80001};
    uint64_t checked=0;
    for(uint32_t divisor:edges){
        fast.set(divisor);
        for(uint64_t n:{uint64_t(0),uint64_t(1),uint64_t(divisor)-1,uint64_t(divisor),uint64_t(divisor)+1,UINT64_MAX}){if(fast(n)!=n/divisor||fast.divide_frame(n)!=n/divisor)return 1;checked++;}
        for(unsigned i=0;i<10000;i++){uint64_t n=rng();if(fast(n)!=n/divisor||fast.divide_frame(n)!=n/divisor)return 1;checked++;}
    }
    for(unsigned i=0;i<1000000;i++){
        uint32_t d=uint32_t(rng())|1;fast.set(d);uint64_t n=rng();if(fast(n)!=n/d||fast.divide_frame(n)!=n/d)return 1;checked++;
    }
    const uint64_t cap=1ull<<49;
    for(uint32_t divisor:edges){
        fast.set(divisor);
        for(const uint64_t n:{cap-1,cap,cap+1}){if(fast.divide_frame(n)!=n/divisor)return 1;checked++;}
        const uint64_t maxquot=(cap-1)/divisor;
        for(unsigned i=0;i<100000;i++){
            const uint64_t q=rng()%maxquot;
            const uint64_t n=q*divisor;
            for(uint64_t v:{n,n+1,n+divisor-1}){if(fast.divide_frame(v)!=v/divisor)return 1;checked++;}
            const uint64_t arbitrary=rng()&(cap-1);
            if(fast.divide_frame(arbitrary)!=arbitrary/divisor)return 1;
            checked++;
        }
    }
    std::printf("Exact constant division: %llu edge/random cases\n",(unsigned long long)checked);
}
