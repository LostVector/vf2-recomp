#include "../../src/core/cycle_duration.h"
#include <array>
#include <bit>
#include <cstdio>
#include <random>
int main(){
    uint64_t cases=0;std::mt19937_64 random(0xc1c1e);
    auto check=[&](uint64_t period,int cycles){
        vf2::cycle_duration duration;duration.set(period);
        const auto expected=vf2::uint128_t(cycles)*vf2::uint128_t(period);
        const vf2::uint128_t clock=(vf2::uint128_t(random()%16)<<64)|random();
        if(duration(cycles)!=expected||clock+duration(cycles)!=clock+expected){
            std::fprintf(stderr,"Cycle duration mismatch: period %llu cycles %d\n",(unsigned long long)period,cycles);std::exit(1);
        }
        ++cases;
    };
    const uint64_t periods[]={0,1,2,28571428571ull,40000000000ull,59999998800ull,88577097505ull,125000000000ull,1000000000000000000ull,UINT64_MAX};
    for(uint64_t period:periods){
        for(int cycles:{INT32_MIN,-2000000,-1,0,1,2,2000000,INT32_MAX})check(period,cycles);
        uint64_t boundary=std::min<uint64_t>(INT32_MAX,period?UINT64_MAX/period:UINT64_MAX);
        if(boundary)check(period,int(boundary-1));check(period,int(boundary));if(boundary<INT32_MAX)check(period,int(boundary+1));
        for(unsigned i=0;i<20000;++i)check(period,std::bit_cast<int32_t>(uint32_t(random())));
    }
    for(unsigned i=0;i<100000;++i){check(random(),std::bit_cast<int32_t>(uint32_t(random())));check(periods[4+random()%4],int(random()%2000000));}
    std::printf("Cycle duration passed: %llu signed, overflow-boundary, clock-carry and random cases\n",(unsigned long long)cases);
}
