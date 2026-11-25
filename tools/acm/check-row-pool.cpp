#include "../../src/machine/parallel.h"
#include <array>
#include <cstdio>

static std::atomic<unsigned> hooks{0},expected_hooks{0};
static std::atomic<unsigned> generations{0};
static std::atomic<bool> valid{true};

static void check_generation(unsigned salt) {
    const int unit_count=9+int(salt%23);
    std::array<std::atomic<unsigned>,32> unit{};
    row_pool::get().run(unit_count,[&](int begin,int end){
        if(end!=begin+1||begin<0||end>unit_count){
            valid=false;
            return;
        }
        // Nested calls execute inline, so the same worker owns every value.
        parallel_rows(3,[&](int b,int e){
            if(b!=0||e!=3)valid=false;
            unit[begin].fetch_add(unsigned(e-b),std::memory_order_relaxed);
        });
    },1,true);
    for(int i=0;i<unit_count;++i)if(unit[i]!=3)valid=false;

    const int n=129+int(salt%129),requested=1+int(salt%5);
    const int parts=std::min(n,row_pool::get().threads()*requested);
    std::array<std::atomic<unsigned>,258> rows{};
    std::array<std::atomic<unsigned>,160> bands{};
    parallel_rows(n,[&](int begin,int end){
        if(begin<0||end>n||begin>=end){valid=false;return;}
        int band=-1;
        for(int i=0;i<parts;++i)if(begin==n*i/parts&&end==n*(i+1)/parts){band=i;break;}
        if(band<0){valid=false;return;}
        bands[band].fetch_add(1,std::memory_order_relaxed);
        for(int i=begin;i<end;++i)rows[i].fetch_add(1,std::memory_order_relaxed);
    },requested);
    for(int i=0;i<parts;++i)if(bands[i]!=1)valid=false;
    for(int i=0;i<n;++i)if(rows[i]!=1)valid=false;
    expected_hooks.fetch_add(unsigned(unit_count+parts),std::memory_order_relaxed);
    generations.fetch_add(2,std::memory_order_relaxed);
}

int main(){
    if(row_pool::get().threads()<2)return 2;
    row_pool::band_hook=[] {hooks.fetch_add(1,std::memory_order_relaxed);};
    auto worker=[](unsigned id){for(unsigned i=0;i<1000;++i)check_generation(id*1000+i);};
    std::thread first(worker,0),second(worker,1);
    worker(2);
    first.join();second.join();
    if(!valid||generations!=6000||hooks!=expected_hooks)return 1;
    std::printf("%u synchronized generations and %u exact hook invocations\n",
                generations.load(),hooks.load());
}
