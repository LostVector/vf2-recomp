// Native scalar arithmetic versus the independent MAME instruction loop.
#include "../../src/rec/i960_native.h"
#include "../../src/rec/i960_ops.h"
#include <array>
#include <cstdio>
#include <cstring>
struct arithmetic_bus:mame_bus32{
    std::array<u8,65536> code{};
    u8 read_byte(offs_t a)override{return code.at(a);}
    u16 read_word(offs_t a)override{return read_byte(a)|(u16(read_byte(a+1))<<8);}
    u32 read_dword(offs_t a)override{return read_word(a)|(u32(read_word(a+2))<<16);}
    void write_byte(offs_t,u8)override{std::abort();}
    void write_word(offs_t,u16)override{std::abort();}
    void write_dword(offs_t,u32)override{std::abort();}
};
int main(){
    arithmetic_bus bus;i960_device reference(bus),native(bus);i960rec::runtime runtime;runtime.cpu=&native;
    runtime.rpage.resize(runtime.PAGES);runtime.wpage.resize(runtime.PAGES);
    i960rec::hints hints;std::vector<u32> entries;unsigned words=0;
    struct operand{unsigned index;bool special;};
    const operand sources[]={{0,false},{17,false},{31,false},{0,true},{3,true},{22,true},{7,true}};
    const operand destinations[]={{0,false},{17,false},{31,false},{0,true},{1,true},{3,true}};
    auto add=[&](u32 op){u32 pc=0x100+words*4;entries.push_back(pc);hints.entries.insert(pc);std::memcpy(bus.code.data()+pc,&op,4);u32 branch=0x08000000;std::memcpy(bus.code.data()+pc+4,&branch,4);words+=2;};
    for(unsigned sub:{0xbu,0xcu,0xdu,0xfu})for(auto s1:sources)for(auto s2:sources)for(auto dst:destinations)
        add(0x78000000|(sub<<7)|s1.index|(s2.index<<14)|(dst.index<<19)|(s1.special?0x800:0)|(s2.special?0x1000:0)|(dst.special?0x2000:0));
    for(unsigned major:{0x70u,0x74u})for(auto s1:sources)for(auto s2:sources)for(unsigned dst:{0u,17u,31u})
        add((major<<24)|(1<<7)|s1.index|(s2.index<<14)|(dst<<19)|(s1.special?0x800:0)|(s2.special?0x1000:0));
    auto read=[](void *ctx,u32 a){return static_cast<arithmetic_bus*>(ctx)->read_dword(a);};
    i960rec::native_stats stats;if(!i960rec::build_native(runtime,read,&bus,bus.code.size(),hints,&stats))return 2;
    if(stats.native!=entries.size()*2){std::fprintf(stderr,"Expected arithmetic and branch to be native: %llu/%zu\n",(unsigned long long)stats.native,entries.size()*2);return 1;}
    native.set_rec(&runtime);
    const u32 edges[]={0,0x80000000,1,0x80000001,0x007fffff,0x00800000,0x3f800000,0xbf800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc00000,0xffc12345,0x7f800001,0x3f000000};
    const u64 doubles[]={0,0x8000000000000000ull,1,0x0010000000000000ull,0x3ff0000000000000ull,0xbff0000000000000ull,0x7fefffffffffffffull,0x7ff0000000000000ull,0xfff0000000000000ull,0x7ff0000000000001ull,0xfff8deadbeef1234ull,0x3ff0000000000001ull};
    u32 rng=0x12345678;auto random=[&](){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;};
    unsigned checks=0;
    auto run=[&](u32 pc,unsigned seed,int budget,bool edge,bool hooks){
        std::array<u32,32> regs;std::array<u64,4> fp;
        for(unsigned k=0;k<regs.size();++k)regs[k]=edge?edges[(seed+k)%std::size(edges)]:random();
        for(unsigned k=0;k<fp.size();++k)fp[k]=edge?doubles[(seed+k)%std::size(doubles)]:(u64(random())<<32)|random();
        regs[0]=edge?edges[seed%16]:random();regs[17]=edge?edges[(seed/16)%16]:random();
        std::vector<u32> ha,hb;
        for(auto *c:{&reference,&native}){c->power_on();for(unsigned k=0;k<32;++k)c->set_reg(k,regs[k]);for(unsigned k=0;k<4;++k)i960_rec::fpregs(*c)[k]=u2d(fp[k]);c->set_reg(I960_AC,seed);c->set_reg(I960_IP,pc);}
        if(hooks){reference.on_instruction=[&](offs_t a){ha.push_back(a);};native.on_instruction=[&](offs_t a){hb.push_back(a);};}
        else reference.on_instruction=native.on_instruction=nullptr;
        for(unsigned slice=0;slice<2;++slice){
            int ca=reference.run(budget),cb=native.run(budget);std::vector<u32> a,b;reference.state_words(a);native.state_words(b);
            if(ca!=cb||a!=b||ha!=hb){std::fprintf(stderr,"Arithmetic mismatch PC %x opcode %08x seed %u budget %d slice %u cycles %d/%d\n",pc,bus.read_dword(pc),seed,budget,slice,ca,cb);for(unsigned k=0;k<a.size();++k)if(a[k]!=b[k])std::fprintf(stderr,"word %u %08x/%08x\n",k,a[k],b[k]);std::exit(1);}
            ++checks;
        }
    };
    for(u32 pc:entries){for(unsigned seed=0;seed<256;++seed)run(pc,seed,1+(seed%41),true,false);for(unsigned seed=0;seed<32;++seed)run(pc,seed,1+(seed%41),false,false);run(pc,7,19,true,true);}
    // Extra random coverage of the binary32-only path, with distinct sources.
    if(std::getenv("VF2_REC_SCALAR32"))for(u32 pc:entries){
        const u32 op=bus.read_dword(pc);
        if(op>>24==0x78&&!(op&0x3800)&&(op&31)!=((op>>14)&31)&&((op>>7)&15)!=0xb)
            for(unsigned seed=0;seed<4096;++seed)run(pc,seed,1+(seed%41),false,false);
    }
    std::printf("Native arithmetic: %zu operand forms, %u full-state slice comparisons, NaNs, subnormals, overflow, literal/FP/GPR aliases and debug hooks pass; %llu native instructions\n",entries.size(),checks,(unsigned long long)stats.native);
}
