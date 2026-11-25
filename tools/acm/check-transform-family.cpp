#include "matrix-test-bus.h"
#include <utility>

struct rotation_bus : matrix_bus {
    std::vector<std::pair<u32,int>> read_events;
    u32 io_r(u32 address) override {
        read_events.emplace_back(address,cpu.cycles_used());
        return matrix_bus::io_r(address);
    }
};

// Verify the two affine bodies and both write destinations, including forced
// state observations partway through each original instruction prefix.
static bool check_translation(matrix_bus &a,matrix_bus &b){
    unsigned cases=0;
    for(unsigned variant=0;variant<2;++variant){
        const uint64_t before=a.cpu.native_translations(),resumed=a.cpu.resumed_translations();
        const unsigned span=variant?37:31;
        for(unsigned seed=1;seed<=1200;++seed)for(unsigned cycles:{1u,5u,12u,18u,25u,30u,31u,35u,37u})
            for(bool observe:{false,true}){
                uint32_t random=seed*1013904223u;
                auto number=[&](){
                    random=random*1664525u+1013904223u;
                    return seed%5==0?random:f2u(float(int(random%16385)-8192)/1024.f);
                };
                for(auto &v:a.low)v=number();
                for(auto &v:a.high)v=number();
                a.low[0]=seed%29==0?1:0;
                const unsigned index=seed%19==0?501:seed%23==0?500:(seed*3)%501;
                a.low[0x3f]=seed%7==0?index+0x20000:index; // MOV truncates x0/x1 to 16 bits
                b.low=a.low;b.high=a.high;b.io=a.io;b.rf=a.rf;
                a.isolated=b.isolated=seed%17!=0;
                const unsigned x0=variant?17:index, x1=seed%83;
                const u16 pc=variant?0xb44:0xb31;
                mb86233_matrix_fixture::rotation(a.cpu,pc,x0,x1,false,seed);
                mb86233_matrix_fixture::rotation(b.cpu,pc,x0,x1,false,seed);
                const uint64_t old=a.cpu.native_translations();
                const int ca=a.cpu.run(cycles),cb=b.cpu.run(cycles);
                const bool active=mb86233_matrix_fixture::pending(a.cpu);
                const auto low=a.low;
                const auto high=a.high;
                if(observe){
                    if(a.view()!=b.view()||a.low!=low||a.high!=high||
                       active!=mb86233_matrix_fixture::pending(a.cpu))return false;
                    if(a.state()!=b.state())return false;
                }
                if(cycles<span){
                    const int tail=1+seed%(span-cycles+1);
                    if(a.cpu.run(tail)!=b.cpu.run(tail))return false;
                }
                auto sa=a.state(),sb=b.state();
                if(ca!=cb||sa!=sb||a.cpu.compiled_instructions()!=b.cpu.compiled_instructions()){
                    size_t offset=0;
                    while(offset<sa.size()&&sa[offset]==sb[offset])++offset;
                    std::fprintf(stderr,"Translation mismatch pc %x seed %u budget %u observe %d offset %zu native %d\n",
                                 pc,seed,cycles,observe,offset,a.cpu.native_translations()!=old);
                    return false;
                }
                ++cases;
            }
        const auto calls=a.cpu.native_translations()-before;
        const auto continued=a.cpu.resumed_translations()-resumed;
        if(!continued||calls<=continued)return false;
        std::printf("Translation variant %u native hits %llu, resumable %llu\n",variant,
                    (unsigned long long)calls,(unsigned long long)continued);
    }
    std::printf("%u exact affine translation cases\n",cases);
    return true;
}

int main(int argc,char **argv){
    if(argc!=2||!std::getenv("VF2_TGP_NATIVE_ROTATION")||
       !std::getenv("VF2_TGP_NATIVE_TRANSLATION"))return 2;
    rotation_bus a,b;
    FILE *rom=std::fopen(argv[1],"rb");
    if(!rom)return 2;
    const bool loaded=std::fread(a.program.data(),1,sizeof(a.program),rom)==sizeof(a.program);
    std::fclose(rom);
    if(!loaded)return 2;
    b.program=a.program;
    b.cpu.on_instruction=[](u16){};
    const u16 entries[]={0xb79,0xb93,0xbad};
    unsigned cases=0;uint64_t resumed=0;
    uint64_t calls[3]{};
    for(unsigned axis=0;axis<3;++axis)for(unsigned seed=1;seed<=1200;++seed)
        for(bool include_reads:{false,true})for(unsigned tail:{1u,8u,21u,32u,33u})for(bool materialize:{false,true}){
            uint32_t random=seed*1013904223u;
            auto next=[&](){random=random*1664525u+1013904223u;return random;};
            auto number=[&](){const uint32_t raw=next();return seed%5==0?raw:f2u(float(int(raw%16385)-8192)/1024.f);};
            for(auto &v:a.low)v=number();
            for(auto &v:a.high)v=number();
            for(auto &v:a.io)v=number();
            a.low[0]=seed%29==0?1:0;
            b.low=a.low;b.high=a.high;b.io=a.io;b.rf=a.rf;
            a.isolated=b.isolated=seed%17!=0;
            a.read_events.clear();b.read_events.clear();
            const unsigned x0=seed%19==0?504:(seed*3)%504;
            const unsigned x1=include_reads||seed%23!=0?x0:x0+1;
            const u16 start=entries[axis]-(include_reads?5:0);
            mb86233_matrix_fixture::rotation(a.cpu,start,x0,x1,!include_reads,seed);
            mb86233_matrix_fixture::rotation(b.cpu,start,x0,x1,!include_reads,seed);
            const auto before=a.cpu.native_rotations();
            const unsigned cycles=tail+(include_reads?5:0);
            const auto before_resumed=a.cpu.resumed_rotations();
            const int ca=a.cpu.run(cycles),cb=b.cpu.run(cycles);
            resumed+=a.cpu.resumed_rotations()-before_resumed;
            if(materialize){
                if(a.state()!=b.state())return 4;
            }else{
                const int next_budget=1+(seed%36);
                if(a.cpu.run(next_budget)!=b.cpu.run(next_budget))return 5;
            }
            const uint64_t used=a.cpu.native_rotations()-before;
            auto sa=a.state(),sb=b.state();
            if(ca!=cb||sa!=sb||a.read_events!=b.read_events||a.cpu.compiled_instructions()!=b.cpu.compiled_instructions()){
                size_t offset=0;
                while(offset<sa.size()&&sa[offset]==sb[offset])++offset;
                std::fprintf(stderr,"Mismatch axis %u seed %u reads %d budget %u cycles %d/%d offset %zu native %llu\n",
                    axis,seed,include_reads,cycles,ca,cb,offset,(unsigned long long)used);
                return 1;
            }
            if(ca!=cb)return 3;
            calls[axis]+=used;
            ++cases;
        }
    if(!calls[0]||!calls[1]||!calls[2])return 4;
    if(!resumed)return 6;
    std::printf("Verified %llu resumable rotations\n",(unsigned long long)resumed);
    std::printf("%u exact rotation cases, native hits %llu/%llu/%llu with matching RAM, registers, instruction counts and timed I/O reads\n",
        cases,(unsigned long long)calls[0],(unsigned long long)calls[1],(unsigned long long)calls[2]);
    if(!check_translation(a,b))return 7;
}
