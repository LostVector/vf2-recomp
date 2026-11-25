#include "../../src/core/scheduler_gap.h"
#include <cstdio>
#include <array>
#include <random>
#include <vector>
int main(){
    using T=vf2::uint128_t;
    std::mt19937_64 random(0x5c4ed);
    uint64_t cases=0;
    auto check=[&](T target,T local,uint64_t period){
        T delta;bool got=vf2::scheduler_gap(target,local,period,delta);
        if(delta!=target-local||got!=(target>=local&&target-local>=T(period))){
            std::fprintf(stderr,"Scheduler gap mismatch at case %llu\n",(unsigned long long)cases);std::exit(1);
        }
        ++cases;
    };
    std::vector<T> edges={0,1,2,T(1)<<32,(T(1)<<64)-1,T(1)<<64,(T(1)<<64)+1,(T(1)<<96)-1,T(1)<<96,~T(0),~T(0)-1};
    for(T a:edges)for(T b:edges)for(uint64_t period:std::array<uint64_t,6>{0,1,40000000000ull,59999998800ull,125000000000ull,UINT64_MAX})check(a,b,period);
    for(unsigned i=0;i<200000;++i){
        T local=(T(random())<<64)|random();uint64_t period=random();
        check((T(random())<<64)|random(),local,period);
        for(uint64_t gap:std::array<uint64_t,5>{0,period,period-1,period+1,random()%100000000000000ull}){
            check(local+T(gap),local,period);check(local-T(gap),local,period);
        }
    }
    std::printf("Scheduler gap passed: %llu full-width, backwards, carry and period-boundary cases\n",(unsigned long long)cases);
}
