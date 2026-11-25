// Compare native blocks, single handlers and virtual/direct RAM paths, including
// microcode mutation, revision invalidation and debugger-hook fallback.
#include "../../src/core/mb86233.h"
#include "../../src/machine/state_io.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

struct test_bus:mb86233_bus {
    std::array<u32,4096> program{};
    std::array<u32,0x100> low{};
    std::array<u32,0x200> high{};
    std::array<u32,64> io{};
    std::array<u32,16> rf{};
    uint64_t revision=1;
    bool isolated_ram=false;
    bool stall_fifo_read=false;
    bool abort_fifo_read=false;
    std::vector<int> fifo_read_times;
    mb86233_device cpu{*this};
    const u32 *program_data()const override{return program.data();}
    uint64_t program_revision()const override{return revision;}
    bool isolated_internal_ram()const override{return isolated_ram;}
    internal_ram direct_internal_ram()override{return {low.data(),high.data()};}
    u32 prog_r(u32 a)override{return program[a&4095];}
    u32 data_r(u32 a)override{a&=65535;return a<0x100?low[a]:a-0x200<0x200?high[a-0x200]:0;}
    void data_w(u32 a,u32 v)override{a&=65535;if(a<0x100)low[a]=v;else if(a-0x200<0x200)high[a-0x200]=v;}
    u32 io_r(u32 a)override{return io[a&63];}
    void io_w(u32 a,u32 v)override{io[a&63]=v;}
    u32 rf_r(u32 a)override{
        if((a&15)==1){
            fifo_read_times.push_back(cpu.cycles_used());
            if(stall_fifo_read)cpu.stall();
            if(abort_fifo_read)cpu.abort_timeslice();
        }
        return rf[a&15];
    }
    void rf_w(u32 a,u32 v)override{rf[a&15]=v;}
    void report(const char *phase){
        std::vector<uint8_t> bytes;m2::StateWriter writer(bytes);cpu.state(writer);writer.pod(low);writer.pod(high);writer.pod(io);writer.pod(rf);
        std::printf("STATE %s %016llx\n",phase,(unsigned long long)m2::state_hash(bytes.data(),bytes.size()));
        std::printf("COUNTS %llu %llu\n",(unsigned long long)cpu.compiled_instructions(),(unsigned long long)cpu.interpreted_instructions());
    }
};
static void require(bool value,const char *what){if(!value){std::fprintf(stderr,"TGP test failed: %s\n",what);std::exit(1);}}
struct mb86233_matrix_fixture {
    static void enter(mb86233_device &cpu, u16 pc, u16 base, unsigned mode)
    {
        cpu.power_on();
        cpu.m_pc=pc;
        cpu.m_a=0x3f800000;
        cpu.m_b=0x40000000;
        cpu.m_b1=base;
        if(mode==1)cpu.m_r=2;
    }
    static void check_ram_window(test_bus &fast)
    {
        test_bus reference;
        std::array<u32,0x400> window{};
        fast.cpu.m_fast_ram={window.data(),window.data()+0x200,window.data()};
        for(u32 address=0;address<0x10000;++address){
            const u32 value=0x13763423u^(address*0xa529c81bu);
            fast.cpu.data_write(address+0x10000,value);
            reference.cpu.data_write(address,value);
            const u32 expected=reference.cpu.data_read(address);
            require(fast.cpu.data_read(address)==expected&&
                    fast.cpu.data_read<true>(address+0x20000)==expected,
                    "contiguous RAM read matches wrapped and unmapped bus addresses");
        }
        require(std::equal(window.begin(),window.begin()+0x100,reference.low.begin())&&
                std::equal(window.begin()+0x200,window.end(),reference.high.begin()),
                "contiguous RAM writes preserve bank contents");
        for(unsigned i=0x100;i<0x200;++i)require(window[i]==0,"unmapped RAM stays zero");
        std::puts("RAM window matches the virtual bus across all 65,536 addresses and wrapped aliases");
    }

    static void check_dispatch_suffix(test_bus &fast)
    {
        test_bus reference;
        reference.program=fast.program;
        reference.revision=0;
        const bool enabled=std::getenv("VF2_TGP_NATIVE_DISPATCH")&&std::getenv("VF2_TGP_RESUME_DISPATCH")&&
            !std::getenv("VF2_TGP_FETCH_GUARDS")&&!std::getenv("VF2_TGP_SLOW_RAM");
        unsigned entered=0,cases=0;
        for(u16 pc=0x3b;pc<=0x3e;++pc)for(unsigned command=0;command<256;++command){
            for(int budget=1;budget<=5;++budget)for(unsigned mode=0;mode<4;++mode){
                auto prepare=[&](test_bus &bus){
                    enter(bus.cpu,pc,0,mode==1?1:0);
                    bus.cpu.m_c0=command;
                    bus.cpu.m_a=0x3f;
                    bus.cpu.m_d=command+(pc==0x3e?0x3f:0);
                    bus.fifo_read_times.clear();
                    if(mode==2)bus.cpu.stall();
                };
                prepare(fast);
                prepare(reference);
                if(mode==3)fast.cpu.on_instruction=[](u16){};
                const auto blocks=fast.cpu.compiled_blocks_run();
                const auto x0=fast.cpu.compiled_instructions(),y0=reference.cpu.compiled_instructions();
                require(fast.cpu.run(budget)==reference.cpu.run(budget),"dispatch suffix exact cycles");
                fast.cpu.on_instruction=nullptr;
                if(fast.cpu.compiled_blocks_run()!=blocks){
                    require(mode!=3,"dispatch suffix debugger fallback");
                    ++entered;
                }
                require(fast.cpu.compiled_instructions()-x0==reference.cpu.compiled_instructions()-y0,
                        "dispatch suffix exact instruction totals");
                std::vector<uint8_t> a,b;
                m2::StateWriter x(a),y(b);
                fast.cpu.state(x);reference.cpu.state(y);
                require(a==b&&fast.cpu.m_sample_command==reference.cpu.m_sample_command&&
                        fast.fifo_read_times==reference.fifo_read_times&&fast.rf==reference.rf&&
                        fast.io==reference.io&&fast.low==reference.low&&fast.high==reference.high,
                        "dispatch suffix exact CPU, bus and sampler state");
                ++cases;
            }
        }
        require(enabled?entered>0:entered==0,"dispatch suffix configuration exercised");
        std::printf("Dispatch suffix: %u fused in %u exact slice comparisons\n",entered,cases);
    }

    static void check_dispatch(test_bus &fast)
    {
        test_bus reference;
        reference.program=fast.program;
        reference.revision=0;
        const bool enabled=std::getenv("VF2_TGP_NATIVE_DISPATCH")&&
            !std::getenv("VF2_NO_TGP_PRIVATE_REGIONS")&&
            !std::getenv("VF2_TGP_SLOW_RAM")&&!std::getenv("VF2_TGP_FETCH_GUARDS");
        unsigned entered=0,cases=0;
        for(unsigned command=0;command<256;++command){
            for(unsigned variant=0;variant<2;++variant){
                for(int budget=1;budget<=11;++budget){
                    for(unsigned mode=0;mode<6;++mode){
                        const u32 word=(command<<23)|((command+variant)&255);
                        const u16 pc=mode==3?0x36:0x35;
                        auto prepare=[&](test_bus &bus){
                            enter(bus.cpu,pc,0,(mode==3||mode==5)?1:0);
                            bus.cpu.m_b=word;
                            bus.rf={};
                            bus.rf[1]=word;
                            bus.stall_fifo_read=mode==1;
                            bus.abort_fifo_read=mode==4;
                            bus.fifo_read_times.clear();
                        };
                        prepare(fast);
                        prepare(reference);
                        if(mode==2)fast.cpu.on_instruction=[](u16){};
                        const auto blocks=fast.cpu.compiled_blocks_run();
                        const auto old_fast=fast.cpu.compiled_instructions();
                        const auto old_reference=reference.cpu.compiled_instructions();
                        require(fast.cpu.run(budget)==reference.cpu.run(budget),"dispatch exact cycles");
                        fast.cpu.on_instruction=nullptr;
                        const bool fused=fast.cpu.compiled_blocks_run()!=blocks;
                        if(fused)require((mode==0||mode==3||mode==5)&&budget>=6,"dispatch fusion requires a complete eligible prefix");
                        entered+=fused;
                        require(fast.cpu.compiled_instructions()-old_fast==
                                reference.cpu.compiled_instructions()-old_reference,"dispatch exact instruction count");
                        std::vector<uint8_t> a,b;
                        m2::StateWriter x(a),y(b);
                        fast.cpu.state(x);reference.cpu.state(y);
                        require(fast.cpu.m_sample_command==reference.cpu.m_sample_command,
                                "dispatch preserves command sampler tags");
                        require(a==b&&fast.fifo_read_times==reference.fifo_read_times&&
                                fast.low==reference.low&&fast.high==reference.high&&
                                fast.io==reference.io&&fast.rf==reference.rf,"dispatch exact CPU, RAM and callback timing");
                        ++cases;
                    }
                }
            }
        }
        require(enabled?entered>0:entered==0,"dispatch mode exercised");
        std::printf("Dispatch: %u fused of %u exact slice comparisons\n",entered,cases);
    }

    static void check_command32_regions(test_bus &fast)
    {
        test_bus reference;
        reference.program=fast.program;
        reference.revision=0;
        const bool enabled=std::getenv("VF2_TGP_COMMAND32_REGIONS") &&
            !std::getenv("VF2_NO_TGP_PRIVATE_REGIONS") &&
            !std::getenv("VF2_TGP_SLOW_RAM") && !std::getenv("VF2_TGP_FETCH_GUARDS");
        constexpr u16 starts[]={0x4b5,0x4bd,0x4c4,0x4c9,0x4cc};
        static constexpr u32 edge[]={
            0x00000000u,0x80000000u,0x00000001u,0x80000001u,0x7f800000u,0xff800000u,
            0x7f800123u,0x7fc00456u,0xffc00789u,0x7f7fffffu,0x00800000u
        };
        for(u16 pc:starts){
            unsigned entered=0;
            for(unsigned seed=0;seed<16;++seed)
                for(int cycles=1;cycles<=33;++cycles)
                    for(unsigned mode=0;mode<5;++mode){
                        const auto init=[seed](unsigned n){
                            const auto index=(n+7*seed)%std::size(edge);
                            return seed%2 ? edge[index] : 0x3e800000u+((n*773u+seed*65521u)%0x01000000u);
                        };
                        if(mode==3){fast.program.back()^=1;reference.program.back()^=1;++fast.revision;}
                        auto prepare=[&](test_bus &bus){
                            enter(bus.cpu,pc,0,mode==1?1:0);
                            bus.cpu.m_a=init(30);bus.cpu.m_b=init(32);
                            bus.cpu.m_d=init(35);bus.cpu.m_p=init(36);
                            for(unsigned i=0;i<bus.low.size();++i)bus.low[i]=init(i);
                            for(unsigned i=0;i<bus.high.size();++i)bus.high[i]=init(i+257);
                            bus.rf.fill(init(42));bus.io.fill(init(43));
                            bus.stall_fifo_read=mode==4;
                            bus.fifo_read_times.clear();
                        };
                        prepare(fast);
                        prepare(reference);
                        if(mode==2)fast.cpu.on_instruction=[](u16){};
                        const auto blocks=fast.cpu.compiled_blocks_run();
                        const auto before_fast=fast.cpu.compiled_instructions();
                        const auto before_reference=reference.cpu.compiled_instructions();
                        require(fast.cpu.run(cycles)==reference.cpu.run(cycles),"command 0x32 exact slice cycles");
                        fast.cpu.on_instruction=nullptr;
                        const bool grouped=fast.cpu.compiled_blocks_run()!=blocks;
                        if(grouped)require(mode!=2&&mode!=3,"command 0x32 hook and modified program fallback");
                        entered+=grouped;
                        require(fast.cpu.compiled_instructions()-before_fast==
                                reference.cpu.compiled_instructions()-before_reference,"command 0x32 instruction count");
                        std::vector<uint8_t> a,b;
                        m2::StateWriter x(a),y(b);
                        fast.cpu.state(x);
                        reference.cpu.state(y);
                        if(a!=b)std::fprintf(stderr,"span %03x seed %u mode %u budget %d\n",pc,seed,mode,cycles);
                        require(a==b&&fast.fifo_read_times==reference.fifo_read_times&&fast.rf==reference.rf&&
                                fast.io==reference.io&&fast.low==reference.low&&fast.high==reference.high,
                                "command 0x32 exact CPU, bus and RAM state");
                        if(mode==3){fast.program.back()^=1;reference.program.back()^=1;++fast.revision;}
                    }
            require(enabled?entered>0:entered==0,"command 0x32 grouping configuration exercised");
            std::printf("Command 0x32 region %03x: %u grouped slices in 2640 exact comparisons\n",pc,entered);
        }
    }

    static void check_regions(test_bus &fast)
    {
        test_bus reference;
        reference.program=fast.program;
        reference.revision=0; // Each opcode uses the guarded individual handler.
        const bool enabled=!std::getenv("VF2_NO_TGP_PRIVATE_REGIONS") &&
            !std::getenv("VF2_TGP_SLOW_RAM") && !std::getenv("VF2_TGP_FETCH_GUARDS");
        constexpr u16 starts[]={0x757,0x777,0xd83,0xd98,0xdb0,0xdca,0xdcb,0xdcc,0xdf9};
        for(u16 pc: starts){
            unsigned entered=0;
            for(unsigned n=0;n<240;++n){
                const unsigned mode=n%3;
                const u16 base=(n%2)?0x200:0;
                const auto initial=[n](unsigned i){return 0x3f000000u+((31u*i+32771u*n)%0x200000u);};
                for(unsigned i=0;i<fast.low.size();++i)fast.low[i]=initial(i);
                for(unsigned i=0;i<fast.high.size();++i)fast.high[i]=initial(i+1000);
                reference.low=fast.low;
                reference.high=fast.high;
                fast.io=reference.io={};
                fast.rf=reference.rf={};
                enter(fast.cpu,pc,base,mode);
                enter(reference.cpu,pc,base,mode);
                unsigned hooks=0;
                if(mode==2)fast.cpu.on_instruction=[&hooks](u16){++hooks;};
                const int budget=1+int(n%68);
                const auto blocks=fast.cpu.compiled_blocks_run();
                const auto old_fast=fast.cpu.compiled_instructions();
                const auto old_reference=reference.cpu.compiled_instructions();
                require(fast.cpu.run(budget)==reference.cpu.run(budget),"private region exact slice cycles");
                fast.cpu.on_instruction=nullptr;
                if(mode==2)require(hooks && fast.cpu.compiled_blocks_run()==blocks,"private region debugger fallback");
                entered+=fast.cpu.compiled_blocks_run()!=blocks;
                require(fast.cpu.compiled_instructions()-old_fast==
                        reference.cpu.compiled_instructions()-old_reference,"private region exact instruction count");
                std::vector<uint8_t> a,b;
                m2::StateWriter x(a),y(b);
                fast.cpu.state(x);reference.cpu.state(y);
                require(a==b && fast.low==reference.low && fast.high==reference.high &&
                        fast.io==reference.io && fast.rf==reference.rf,"private region exact CPU and bus state");
            }
            const bool resumable=pc==0xdcb||pc==0xdcc;
            if(enabled&&(!resumable||std::getenv("VF2_TGP_4A_RESUME")))
                require(entered>0,"each private region executes");
            std::printf("Private region %03x: %u fast slices in 240 exact comparisons\n",pc,entered);
        }
    }
};
int main(int argc,char **argv){
    const std::string_view mode=argc==3?argv[2]:"";
    if(argc!=2&&mode!="private"&&mode!="dispatch"&&mode!="ram-window"&&mode!="dispatch-suffix"&&mode!="command32")return 2;
    test_bus bus;FILE *in=std::fopen(argv[1],"rb");require(in,"microcode file");
    require(std::fread(bus.program.data(),1,sizeof(bus.program),in)==sizeof(bus.program),"microcode size");std::fclose(in);
    if(mode=="private"){mb86233_matrix_fixture::check_regions(bus);return 0;}
    if(mode=="command32"){mb86233_matrix_fixture::check_command32_regions(bus);return 0;}
    if(mode=="dispatch"){mb86233_matrix_fixture::check_dispatch(bus);return 0;}
    if(mode=="dispatch-suffix"){mb86233_matrix_fixture::check_dispatch_suffix(bus);return 0;}
    if(mode=="ram-window"){mb86233_matrix_fixture::check_ram_window(bus);return 0;}
    bool blocks=std::getenv("VF2_TGP_BLOCKS")!=nullptr&&std::getenv("VF2_TGP_SINGLE")==nullptr;
    const bool mapped=std::getenv("VF2_TGP_STABLE_PROGRAM")!=nullptr;
    bus.isolated_ram=true;
    const auto run=[&](int cycles){
        if(mapped)return bus.cpu.run_mapped(cycles,bus.program,bus.revision);
        return bus.cpu.run(cycles);
    };
    bus.cpu.power_on();run(10000);bus.report("boot");
    if(blocks)require(bus.cpu.compiled_blocks_run()>0,"native blocks execute");
    unsigned changed=bus.cpu.pc()&4095;u32 original=bus.program[changed];
    bus.program[changed]^=1;++bus.revision;auto before=bus.cpu.compiled_blocks_run();
    run(10000);bus.report("modified");require(bus.cpu.compiled_blocks_run()==before,"modified image disables blocks");
    bus.program[changed]=original;++bus.revision;
    run(10000);bus.report("restored");
    if(blocks)require(bus.cpu.compiled_blocks_run()>before,"restored image resumes blocks");
    unsigned hooks=0;bus.cpu.on_instruction=[&](u16){++hooks;};before=bus.cpu.compiled_blocks_run();
    run(1000);bus.cpu.on_instruction=nullptr;bus.report("debug");
    require(hooks&&bus.cpu.compiled_blocks_run()==before,"debugger uses per-instruction path");
    run(1000);bus.report("resume");
    if(blocks)require(bus.cpu.compiled_blocks_run()>before,"blocks resume after debugger");
    // Slice boundaries must interrupt repetitions at exactly the same point.
    for(unsigned i=0;i<10000;++i)run(1+(i*17)%37);
    bus.report("short-slices");
    std::puts("TGP mutation/revision/debug fallback checks passed");
}
