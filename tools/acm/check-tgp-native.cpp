#include "../../src/core/mb86233.h"
#if !defined(TGP_REFERENCE_COMPILED)
#include "../../build/acm/tgp-native-jit-trial/mb86233_reference.h"
#endif
#include "../../src/machine/state_io.h"
#include <array>
#include <vector>
#include <cstdio>
struct mb86233_matrix_fixture {
    template<class C> static void repeat(C &c,u8 count){c.m_r=count;c.m_i0=c.m_i1=1;}
    template<class C> static void clean(C &c){c.m_r=1;c.m_stall=false;c.m_a=f2u(1.f);c.m_b=f2u(.5f);c.m_d=f2u(.25f);c.m_p=f2u(.125f);c.m_b0=c.m_b1=0;c.m_x0=0x20;c.m_x1=0x40;}
    template<class C> static void seed(C &c,u32 seed,u16 pc){
        auto next=[&](){seed=seed*1664525u+1013904223u;return seed;};c.power_on();c.m_pc=pc;
        c.m_a=next();c.m_b=next();c.m_d=next();c.m_p=next();c.m_st=next();
        c.m_alu_stmask=next();c.m_alu_stset=next();c.m_alu_r1=next();c.m_alu_r2=next();
        c.m_b0=next();c.m_b1=next();c.m_x0=next();c.m_x1=next();c.m_i0=next();c.m_i1=next();
        c.m_c0=next();c.m_c1=next();c.m_rpc=next();c.m_sft=next();c.m_m=next();c.m_vsmr=next();c.m_vsm=next();c.m_mask=next();c.m_sp=next();
        if(seed%3==0){c.m_b0=seed&15;c.m_b1=(seed>>4)&15;c.m_x0=(seed>>8)%240;c.m_x1=(seed>>16)%240;}
        c.m_r=seed%9==0?3:1;c.m_stall=seed%17==0;
        const u32 edges[]={0,0x80000000,0x7fc00001,0x7fc12345,0xffc12345,0x7f800001,0x7f800000,0xff800000,1,0x80000001,0x7f7fffff,0x00800000,0xbf800000,0x3f800000,0x3f000000,0x3f800001};
        if(seed&1){c.m_a=edges[(seed>>4)&15];c.m_b=edges[(seed>>8)&15];c.m_d=edges[(seed>>12)&15];c.m_p=edges[(seed>>16)&15];}
    }
};
template<class CPU,class Interface> struct native_bus:Interface{
    std::array<u32,4096> program{},alternate_program{};bool alternate=false,direct=true;std::array<u32,256> low{};std::array<u32,512> high{};std::array<u32,64> io{},rf{};
    CPU cpu{*this};uint64_t revision=1;bool stall=false,abort_only=false,control_cap=true,stall_aborts=true,inspect=false;std::vector<std::vector<uint8_t>> snapshots;std::vector<std::array<u32,5>> events;
    const u32 *program_data()const override{return alternate?alternate_program.data():program.data();}uint64_t program_revision()const override{return revision;}
    typename Interface::internal_ram direct_internal_ram()override{return direct?typename Interface::internal_ram{low.data(),high.data()}:typename Interface::internal_ram{};}
    bool native_control_callbacks()const override{return control_cap;}
    u32 prog_r(u32 a)override{return program_data()[a&4095];}
    u32 data_r(u32 a)override{a&=65535;return a<256?low[a]:a-512<512?high[a-512]:0;}
    void data_w(u32 a,u32 v)override{a&=65535;if(a<256)low[a]=v;else if(a-512<512)high[a-512]=v;}
    void event(u32 kind,u32 address,u32 value){events.push_back({kind,address,value,u32(cpu.cycles_used()),u32(cpu.pc())});if(inspect){std::vector<uint8_t> out;m2::StateWriter writer(out);cpu.state(writer);snapshots.push_back(std::move(out));}if(kind%2)cpu.gpio0_w(value&1);if(stall&&kind%2==0){stall=false;cpu.stall();if(stall_aborts)cpu.abort_timeslice();}else if(abort_only){abort_only=false;cpu.abort_timeslice();}}
    u32 io_r(u32 a)override{u32 v=io[a&63];event(0,a,v);return v;}void io_w(u32 a,u32 v)override{event(1,a,v);io[a&63]=v;}
    u32 rf_r(u32 a)override{u32 v=rf[a&63];event(2,a,v);return v;}void rf_w(u32 a,u32 v)override{event(3,a,v);rf[a&63]=v;}
    std::vector<uint8_t> state(){std::vector<uint8_t> out;m2::StateWriter writer(out);cpu.state(writer);writer.pod(low);writer.pod(high);writer.pod(io);writer.pod(rf);return out;}
};
int main(int argc,char **argv){
    if(argc!=2)return 2;native_bus<mb86233_device,mb86233_bus> a;
#if defined(TGP_REFERENCE_COMPILED)
    native_bus<mb86233_device,mb86233_bus> b;b.cpu.on_instruction=[](u16){};
#else
    native_bus<mb86233_reference,mb86233_reference_bus> b;
#endif

    FILE *f=std::fopen(argv[1],"rb");if(!f||std::fread(a.program.data(),1,sizeof(a.program),f)!=sizeof(a.program))return 2;std::fclose(f);b.program=a.program;
    u32 rng=0x12345;auto next=[&](){rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;};unsigned checks=0;
    for(unsigned pc=0;pc<4096;++pc)for(unsigned seed=0;seed<16;++seed)for(int budget:{1,2,3,7,23,67}){
        const u32 value=next();for(auto &v:a.low)v=next();for(auto &v:a.high)v=next();for(auto &v:a.io)v=next();for(auto &v:a.rf)v=next();
        // Include valid windows, unmapped gaps, wrapping address arithmetic and aliases.
        b.low=a.low;b.high=a.high;b.io=a.io;b.rf=a.rf;a.events.clear();b.events.clear();a.stall=b.stall=seed%7==0;a.abort_only=b.abort_only=seed%11==0;a.stall_aborts=b.stall_aborts=seed%3!=0;
        mb86233_matrix_fixture::seed(a.cpu,value,pc);mb86233_matrix_fixture::seed(b.cpu,value,pc);
        for(unsigned slice=0;slice<2;++slice){
            auto initial=a.state();const auto native_before=a.cpu.native_instructions();
            const int ca=a.cpu.run(budget),cb=b.cpu.run(budget);auto sa=a.state(),sb=b.state();
            if(ca!=cb||sa!=sb||a.events!=b.events
#if defined(TGP_REFERENCE_COMPILED)
                ||a.cpu.compiled_instructions()!=b.cpu.compiled_instructions()
#endif
            ){size_t at=0;while(at<sa.size()&&sa[at]==sb[at])++at;std::fprintf(stderr,"Native TGP mismatch PC %x op %08x seed %u value %x budget %d slice %u cycles %d/%d offset %zu native %llu events %zu/%zu\n",pc,a.program[pc],seed,value,budget,slice,ca,cb,at,(unsigned long long)a.cpu.native_instructions(),a.events.size(),b.events.size());std::fprintf(stderr,"native delta %llu\n",(unsigned long long)(a.cpu.native_instructions()-native_before));for(unsigned k=0;k<48;k+=4){u32 x;std::memcpy(&x,initial.data()+k,4);std::fprintf(stderr,"initial word %u: %08x\n",k/4,x);}for(size_t i=0;i<sa.size()&&i<180;++i)if(sa[i]!=sb[i])std::fprintf(stderr,"byte %zu: %02x/%02x\n",i,sa[i],sb[i]);return 1;}
            ++checks;
        }
    }
#if defined(TGP_REFERENCE_COMPILED)
    if(std::getenv("VF2_TGP_NATIVE_IO")){
        unsigned target=4096;
        for(unsigned pc=0;pc<4096;++pc){u32 op=a.program[pc];unsigned r1=op&63,r2=op>>9&511;
            if(op>>26==7&&(op>>18&7)==7&&(op>>21&31)==0&&(r2>>6)==6&&r1>=32&&r1<48&&((r2&63)==16||(r2&63)==19||(r2&63)==25||(r2&63)==28)){target=pc;break;}}
        if(target==4096){std::fprintf(stderr,"No RF transfer capability fixture\n");return 1;}
        for(bool capable:{false,true}){
            a.control_cap=b.control_cap=capable;a.inspect=b.inspect=!capable;
            mb86233_matrix_fixture::seed(a.cpu,7,target);mb86233_matrix_fixture::seed(b.cpu,7,target);mb86233_matrix_fixture::clean(a.cpu);mb86233_matrix_fixture::clean(b.cpu);
            a.low.fill(f2u(.5f));a.high.fill(f2u(.25f));b.low=a.low;b.high=a.high;a.rf.fill(f2u(.5f));b.rf=a.rf;a.io.fill(0);b.io=a.io;
            a.events.clear();b.events.clear();a.snapshots.clear();b.snapshots.clear();a.stall=b.stall=a.abort_only=b.abort_only=false;
            const auto before=a.cpu.native_helpers();int ca=a.cpu.run(1),cb=b.cpu.run(1);
            if(ca!=cb||a.state()!=b.state()||a.events!=b.events||a.snapshots!=b.snapshots||a.cpu.native_helpers()-before!=(capable?0u:1u)||(!capable&&a.snapshots.empty())){std::fprintf(stderr,"Native I/O capability/snapshot fixture failed: %d\n",capable);return 1;}++checks;
        }
        a.control_cap=b.control_cap=true;a.inspect=b.inspect=false;
    }
    {
        unsigned target=4096;
        for(unsigned pc=0;pc<4096;++pc){u32 op=a.program[pc];if(op>>26==7&&(op>>21&31)==0&&(op>>18&7)==3){target=pc;break;}}
        if(target==4096)return 1;
        for(unsigned count:{0u,1u,2u,255u})for(int budget:{1,2,3,17,255,256,257,511}){
            mb86233_matrix_fixture::seed(a.cpu,7,target);mb86233_matrix_fixture::seed(b.cpu,7,target);mb86233_matrix_fixture::clean(a.cpu);mb86233_matrix_fixture::clean(b.cpu);
            mb86233_matrix_fixture::repeat(a.cpu,count);mb86233_matrix_fixture::repeat(b.cpu,count);
            a.low.fill(f2u(.5f));a.high.fill(f2u(.25f));b.low=a.low;b.high=a.high;a.io.fill(0);b.io=a.io;a.rf.fill(0);b.rf=a.rf;
            a.events.clear();b.events.clear();a.stall=b.stall=a.abort_only=b.abort_only=false;
            for(unsigned slice=0;slice<2;++slice){int ca=a.cpu.run(budget),cb=b.cpu.run(budget);if(ca!=cb||a.state()!=b.state()||a.events!=b.events||a.cpu.compiled_instructions()!=b.cpu.compiled_instructions()){std::fprintf(stderr,"Native repeat wrap/slice fixture failed: count %u budget %d slice %u\n",count,budget,slice);return 1;}++checks;}
        }
    }
    const auto original=a.program[0xbc4];
    for(unsigned phase=0;phase<7;++phase){
        a.program[0xbc4]=b.program[0xbc4]=phase==1?original^1u:original;++a.revision;++b.revision;
        a.alternate_program=a.program;b.alternate_program=b.program;a.alternate=b.alternate=phase==5;
        a.cpu.on_instruction=phase==3?std::function<void(u16)>([](u16){}):std::function<void(u16)>();
        mb86233_matrix_fixture::seed(a.cpu,7,0xbc4);mb86233_matrix_fixture::seed(b.cpu,7,0xbc4);mb86233_matrix_fixture::clean(a.cpu);mb86233_matrix_fixture::clean(b.cpu);
        a.low.fill(f2u(.5f));a.high.fill(f2u(.25f));b.low=a.low;b.high=a.high;a.io.fill(0);b.io=a.io;a.rf.fill(0);b.rf=a.rf;a.events.clear();b.events.clear();a.stall=b.stall=false;
        const auto before=a.cpu.native_instructions();const int ca=a.cpu.run(31),cb=b.cpu.run(31);
        if(ca!=cb||a.state()!=b.state()||a.events!=b.events||((a.cpu.native_instructions()!=before)!=(phase!=1&&phase!=3))){std::fprintf(stderr,"Native TGP fallback phase %u failed\n",phase);return 1;}++checks;
    }
    {
        native_bus<mb86233_device,mb86233_bus> slow,ref;slow.direct=ref.direct=false;slow.program=ref.program=a.program;ref.cpu.on_instruction=[](u16){};
        mb86233_matrix_fixture::seed(slow.cpu,7,0xbc4);mb86233_matrix_fixture::seed(ref.cpu,7,0xbc4);mb86233_matrix_fixture::clean(slow.cpu);mb86233_matrix_fixture::clean(ref.cpu);
        slow.low.fill(f2u(.5f));slow.high.fill(f2u(.25f));ref.low=slow.low;ref.high=slow.high;
        int ca=slow.cpu.run(31),cb=ref.cpu.run(31);if(ca!=cb||slow.state()!=ref.state()||slow.events!=ref.events||slow.cpu.native_instructions()){std::fprintf(stderr,"Native TGP slow bus fallback failed\n");return 1;}++checks;
    }
#endif
    if(!a.cpu.native_instructions()){std::fprintf(stderr,"Translator never executed\n");return 1;}
    std::printf("Native TGP: %u full selected-baseline state/RAM/callback comparisons, %llu translated instructions in %llu regions\n",checks,(unsigned long long)a.cpu.native_instructions(),(unsigned long long)a.cpu.native_entries());
}
