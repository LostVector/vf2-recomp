// Full per-slice state and helper-observed PCs against MAME's interpreter.
#include "../../src/rec/i960_native.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
struct bus:mame_bus32 {
    std::array<u8,0x4000> code{};
    std::array<u8,4096> plain{};
    std::vector<std::array<u32,5>> observations;
    i960_device *cpu=nullptr;u32 value=0x8234fedc;bool stall=false,abort=false;
    bool fixed_port=false,sequence_reads=false;int stall_after=-1;
    uint64_t fixed_port_flag_reads=0;
    u32 read_sequence=0;
    bool observe(u32 a,bool write){
        if(a<0x10000)return false;
        observations.push_back({a,u32(write),cpu->pip(),cpu->ip(),u32(cpu->cycles_used())});
        if(abort){abort=false;cpu->abort_timeslice();}
        if(stall||stall_after==0){stall=false;stall_after=-1;cpu->i960_stall();return true;}
        if(stall_after>0)--stall_after;
        return false;
    }
    u16 flags(offs_t address)override{
        if(address==0x10000){
            ++fixed_port_flag_reads;
            if(fixed_port)return VF2_FIXED_STORE_PORT|VF2_FIXED_LOAD_PORT;
        }
        return ram(address)?i960_cpu_device::BURST:0;
    }
    bool ram(u32 a)const{return a>=0x8000&&a<0x9000;}
    u8 raw(u32 a)const{return a<code.size()?code[a]:ram(a)?plain[a-0x8000]:u8(value>>((a&3)*8));}
    u8 read_byte(offs_t a)override{if(observe(a,false))return 0;return raw(a);}
    u16 read_word(offs_t a)override{if(observe(a,false))return 0;return u16(raw(a))|(u16(raw(a+1))<<8);}
    u32 read_dword(offs_t a)override{
        if(observe(a,false))return 0;
        if(sequence_reads&&a==0x10000)return 0x9e3779b9u*++read_sequence;
        return u32(raw(a))|(u32(raw(a+1))<<8)|(u32(raw(a+2))<<16)|(u32(raw(a+3))<<24);
    }
    void write_byte(offs_t a,u8 v)override{if(observe(a,true))return;if(ram(a))plain[a-0x8000]=v;else value=v;}
    void write_word(offs_t a,u16 v)override{if(observe(a,true))return;if(ram(a)&&ram(a+1)){plain[a-0x8000]=v;plain[a+1-0x8000]=v>>8;}else value=v;}
    void write_dword(offs_t a,u32 v)override{if(observe(a,true))return;if(ram(a)&&ram(a+3)){for(unsigned k=0;k<4;++k)plain[a+k-0x8000]=v>>(k*8);}else value=v;}

};
static void require(bool ok,const char *what){if(!ok){std::fprintf(stderr,"PC check failed: %s\n",what);std::exit(1);}}
static u32 regop(u32 major,u32 sub,u32 dst,u32 src2,u32 src1,bool literal=false){return (major<<24)|(dst<<19)|(src2<<14)|(sub<<7)|src1|(literal?0x800:0);}
struct timer_bus final:mame_bus32 {
    static constexpr u32 head=0x4cb7c,mask=0xfffff;
    // Countdown poll followed by a self branch at the exit label.
    static constexpr std::array<u32,11> program={0x80703000,0x500000,0x3503a020,
        0x98703000,0xf00008,0x8c703000,mask,0x587b808f,0x327b8008,0x317b5fdc,0x08000000};
    i960_device *cpu=nullptr;
    u32 first=0,steps=0,timer=0,flag=0,native_cycles=0;
    bool running=false;
    u16 flags(offs_t)override{return 0;}
    u32 read_dword(offs_t a)override{
        if(a==0xf00008){
            if(running)timer=first-(steps+cpu->cycles_used());
            return timer;
        }
        if(a==0x500000)return flag;
        if(a>=head&&(a-head)/4<program.size())return program[(a-head)/4];
        return 0;
    }
    u16 read_word(offs_t a)override{return u16(read_dword(a&~3)>>(8*(a&3)));}
    u8 read_byte(offs_t a)override{return u8(read_dword(a&~3)>>(8*(a&3)));}
    void write_byte(offs_t,u8)override{}
    void write_word(offs_t,u16)override{}
    void write_dword(offs_t,u32)override{}
    static void collapse(void *context,u32 limit,u32 &counter,int &budget){
        auto &bus=*static_cast<timer_bus*>(context);
        if(!bus.running||bus.flag||counter!=(bus.timer&mask))return;
        const auto elapsed=i960rec::timer_poll_elapsed(counter,limit,budget);
        bus.timer-=elapsed;counter-=elapsed;budget-=int(elapsed);
        bus.native_cycles+=elapsed;
    }
};

static void check_timer_poll(){
    timer_bus a,b;
    i960_device reference(a),native(b);a.cpu=&reference;b.cpu=&native;
    i960rec::hints hints;hints.entries.insert(timer_bus::head);
    i960rec::runtime rt;rt.cpu=&native;rt.rpage.resize(rt.PAGES);rt.wpage.resize(rt.PAGES);
    rt.timer_poll_context=&b;rt.collapse_timer_poll=timer_bus::collapse;
    auto read=[](void *ctx,uint32_t addr){return static_cast<timer_bus*>(ctx)->read_dword(addr);};
    require(i960rec::build_native(rt,read,&b,timer_bus::head+44,hints,nullptr),"timer loop native build");
    native.set_rec(&rt);
    unsigned cases=0;
    for(u32 initial:{0u,17u,24u,75u,320u,1212u,0xfffffu,0x100020u,0xfff00100u})
    for(u32 limit:{0u,1u,39u,80u,400u,800u,0xfffffu})
    for(int budget:{1,2,5,9,12,13,23,24,25,42,46,47,49,64,89,128,511,1000})
    for(bool running:{false,true})
    for(u32 flag:{0u,1u})
    for(u32 entry_offset:{0u,8u,12u,28u,36u}){
        for(auto *c:{&reference,&native}){
            c->power_on();c->set_reg(13,limit);c->set_reg(14,timer_bus::mask);c->set_reg(15,initial&timer_bus::mask);
            c->set_reg(I960_IP,timer_bus::head+entry_offset);
        }
        a.first=b.first=initial;a.timer=b.timer=initial;
        a.steps=b.steps=0;a.flag=b.flag=flag;a.running=b.running=running;
        for(int slice:{budget,budget/2+1,budget+1}){
            int x=reference.run(slice),y=native.run(slice);
            std::vector<u32> left,right;reference.state_words(left);native.state_words(right);
            if(x!=y||left!=right||a.timer!=b.timer){
                std::fprintf(stderr,"timer poll mismatch: timer %x/%x initial %x limit %x budget %d entry %u running %d flag %u used %d/%d\n",
                    a.timer,b.timer,initial,limit,slice,entry_offset,running,flag,x,y);
                for(unsigned j=0;j<left.size();++j)if(left[j]!=right[j]){std::fprintf(stderr,"state %u %x %x\n",j,left[j],right[j]);break;}
                std::exit(1);
            }
            a.steps+=x;b.steps+=y;
        }
        ++cases;
    }
    require(b.native_cycles>0,"timer loop batches exercised");
    std::printf("%u three-slice timer polling cases match the interpreter, %u cycles batched\n",cases,b.native_cycles);
}


int main(){
    check_timer_poll();
    const bool pc_mode=std::getenv("VF2_REC_LAZY_PC"),cycle_mode=std::getenv("VF2_REC_REG_CYCLES");
    require(pc_mode||cycle_mode,"feature enabled");
    bus a,b;i960_device reference(a),native(b);a.cpu=&reference;b.cpu=&native;
    i960rec::hints map;std::vector<u32> entries;
    for(unsigned variant=0;variant<32;++variant){
        const u32 pc=0x100+variant*128;u32 at=pc;
        auto put=[&](u32 word){std::memcpy(b.code.data()+at,&word,4);at+=4;};
        entries.push_back(pc);map.entries.insert(pc);
        put(regop(0x5c,12,17,0,variant,true));
        put(regop(0x59,0,18,17,1,true));
        put(regop(0x58,6,19,18,variant,true));
        put(regop(0x59,13,20,19,2,true)); // helper fallback: rotate
        put(regop(0x5a,2,21,20,3,true)); // helper fallback: conditional compare
        put(regop(0x5b,0,22,20,variant,true)); // helper fallback: add with carry
        put(regop(0x5d+variant%3,12,24,0,16)); // movl/t/q
        put((0x8cu<<24)|(28<<19)|(5<<14)|0x3400);put(0x20); // 8-byte lda
        put((0x24u<<24)|(29<<19));
        const u32 load[]={0x80,0x88,0x90,0xc0,0xc8};
        const u32 store[]={0x82,0x8a,0x92,0xc2,0xca};
        // The same address via both base and base+index+displacement forms.
        const u32 mode=variant&1?0x3c06:0x1000;
        put((load[variant%5]<<24)|(30<<19)|(5<<14)|mode);
        if(variant&1)put(0xfffffffd);
        put((store[variant%5]<<24)|(30<<19)|(5<<14)|mode);
        if(variant&1)put(0xfffffffd);
        put(0x12000008); // conditional branch across an arithmetic instruction
        put(regop(0x59,2,20,20,1,true));
        put(0x08000000); // branch to itself
    }
    const u32 fault_entry=0x2000;map.entries.insert(fault_entry);
    u32 mov=regop(0x5c,12,17,0,7,true),bad=0xf0000000;
    std::memcpy(b.code.data()+fault_entry,&mov,4);std::memcpy(b.code.data()+fault_entry+4,&bad,4);
    const u32 spin_entry=0x2400;map.entries.insert(spin_entry);
    const u32 spin[]={ (0x90u<<24)|(17<<19)|0x3000,0x8000,regop(0x5a,0,0,17,0,true),0x15fffff4 };
    std::memcpy(b.code.data()+spin_entry,spin,sizeof spin);
    const std::array<u32,6> burst_ops={0x98,0x9a,0xa0,0xa2,0xb0,0xb2};
    std::vector<u32> burst_entries;
    for(unsigned i=0;i<burst_ops.size();++i){
        const u32 entry=0x2500+i*32;
        const u32 dst=28+(i%2);  // verify normalization of a misaligned register group
        const u32 op=(burst_ops[i]<<24)|(dst<<19)|(5<<14)|0x1000;
        const u32 store_or_load=((burst_ops[i]^2)<<24)|(dst<<19)|(5<<14)|0x3400;
        const u32 instructions[]={op,store_or_load,4,0x08000000};
        std::memcpy(b.code.data()+entry,instructions,sizeof instructions);
        burst_entries.push_back(entry);
        map.entries.insert(entry);
    }
    // Each branch is tested at a slice boundary and before the bus write
    // at either destination. The alternate target leaves native code.
    std::vector<u32> branch_entries;
    for(u32 op:{0x08u,0x10u,0x11u,0x12u,0x13u,0x14u,0x15u,0x16u,0x17u,
                0x30u,0x31u,0x32u,0x33u,0x34u,0x35u,0x36u,0x37u,
                0x39u,0x3au,0x3bu,0x3cu,0x3du,0x3eu}){
        for(bool literal:{false,true}){
            const u32 pc=0x2800+32*branch_entries.size();
            const u32 operands=op>=0x30 ? ((literal?1u:9u)<<19)|(7u<<14)|(literal?0x2000u:0u) : 0u;
            const u32 program[]={op<<24|operands|16u,0x92816000,0x08000000,0,
                                 0x92896000,0x08000000};
            std::memcpy(b.code.data()+pc,program,sizeof program);
            branch_entries.push_back(pc);
            map.entries.insert(pc);
        }
    }
    const u32 external_branch=0x2f00;
    const std::array<u32,2> outside={0x08000000u|(0x3ffcu-external_branch),0xf0000000};
    std::memcpy(b.code.data()+external_branch,&outside[0],4);
    std::memcpy(b.code.data()+0x3ffc,&outside[1],4);
    map.entries.insert(external_branch);
    a.code=b.code;
    auto read=[](void *ctx,u32 address){return static_cast<bus*>(ctx)->read_dword(address);};
    i960rec::runtime runtime;runtime.cpu=&native;runtime.rpage.resize(runtime.PAGES);runtime.wpage.resize(runtime.PAGES);
    runtime.rpage[8]=b.plain.data();runtime.wpage[8]=b.plain.data();
    i960rec::native_stats stats;require(i960rec::build_native(runtime,read,&b,b.code.size(),map,&stats),"native build");
    {
        i960rec::runtime eager;eager.cpu=&native;eager.rpage.resize(eager.PAGES);eager.wpage.resize(eager.PAGES);
        i960rec::native_stats baseline;unsetenv("VF2_REC_LAZY_PC");unsetenv("VF2_REC_REG_CYCLES");
        require(i960rec::build_native(eager,read,&b,b.code.size(),map,&baseline),"eager comparison build");
        require(stats.code_bytes!=baseline.code_bytes,"deferred PC paths emitted");if(pc_mode)setenv("VF2_REC_LAZY_PC","1",1);if(cycle_mode)setenv("VF2_REC_REG_CYCLES","1",1);
    }
    native.set_rec(&runtime);
    unsigned fixtures=0;
    auto run=[&](u32 entry,unsigned seed,int budget,bool stall,bool hooks,u32 address,bool abort=false,int delayed_stall=-1,bool sequence_reads=false){
        for(auto *c:{&reference,&native}){
            c->power_on();for(unsigned k=0;k<32;++k)c->set_reg(k,(seed*0x13579u)^(k*0x9e3779b9u));
            c->set_reg(5,address);c->set_reg(6,3);c->set_reg(I960_AC,seed&7);c->set_reg(I960_IP,entry);
            if(entry>=0x2800 && entry<external_branch){
                constexpr std::array<u32,8> values={0,1,2,3,0x7fffffff,0x80000000,0xffffffff,31};
                c->set_reg(7,values[seed%values.size()]);
                c->set_reg(9,values[(seed+seed/8)%values.size()]);
            }
        }
        for(unsigned k=0;k<a.plain.size();++k)a.plain[k]=b.plain[k]=u8(seed*17+k*11);
        a.observations.clear();b.observations.clear();a.value=b.value=0x8234fedc;a.stall=b.stall=stall;a.abort=b.abort=abort;
        b.fixed_port=true;a.stall_after=b.stall_after=delayed_stall;
        a.sequence_reads=b.sequence_reads=sequence_reads;a.read_sequence=b.read_sequence=0;
        std::vector<std::array<u32,3>> ha,hb;
        if(hooks){reference.on_instruction=[&](offs_t ip){ha.push_back({ip,reference.pip(),reference.ip()});};native.on_instruction=[&](offs_t ip){hb.push_back({ip,native.pip(),native.ip()});};}
        else reference.on_instruction=native.on_instruction=nullptr;
        for(unsigned slice=0;slice<4;++slice){
            const int ca=reference.run(budget),cb=native.run(budget);
            std::vector<u32> sa,sb;reference.state_words(sa);native.state_words(sb);
            if(ca!=cb||sa!=sb||a.observations!=b.observations||a.value!=b.value||a.read_sequence!=b.read_sequence||a.plain!=b.plain||ha!=hb||reference.stopped()!=native.stopped()){
                std::fprintf(stderr,"Mismatch entry%x seed%u budget%d stall%d hooks%d slice%u cycles%d/%d\n",entry,seed,budget,stall,hooks,slice,ca,cb);
                for(size_t k=0;k<sa.size();++k)if(sa[k]!=sb[k]){std::fprintf(stderr,"state word %zu: %08x/%08x\n",k,sa[k],sb[k]);break;}
                std::exit(1);
            }
        }
        if(hooks)require(!ha.empty(),"debug hook exercised");
        ++fixtures;
    };
    for(u32 entry:entries)for(u32 address:{0x8000u,0x8001u,0x8ffcu,0x8fffu,0x10000u,0x10001u,0x10000000u})for(unsigned seed=0;seed<8;++seed)for(int budget=1;budget<=40;++budget)for(bool stall:{false,true})run(entry,seed,budget,stall,false,address);
    for(u32 entry:entries)for(u32 address:{0x8000u,0x8001u,0x8ffcu,0x8fffu,0x10000u,0x10001u,0x10000000u})run(entry,3,17,true,true,address);
    for(int budget=1;budget<=40;++budget)run(spin_entry,3,budget,false,false,0x8000);
    for(u32 entry:entries)for(bool stall:{false,true})run(entry,3,20,stall,false,0x10000,true);
    for(u32 entry:burst_entries)
        for(u32 address:{0x8000u,0x8001u,0x8ff0u,0x8ff4u,0x8ff8u,0x8ffcu,0x8fffu,0x10000u,0x10001u,0x10000000u})
            for(int budget=1;budget<=16;++budget)
                for(bool stall:{false,true})run(entry,5,budget,stall,false,address);
    const unsigned before_branches=fixtures;
    for(u32 entry:branch_entries)
        for(unsigned seed=0;seed<16;++seed)
            for(int budget:{1,2,3,4,5,6,10})
                run(entry,seed,budget,false,false,0x10000);
    for(int budget:{1,2,8})run(external_branch,0,budget,false,false,0x10000);
    std::printf("%u additional direct branch fixtures match per-slice state and bus-visible PCs\n",
                fixtures-before_branches);
    run(fault_entry,0,20,false,false,0x10000);
    for(unsigned op:{1u,3u,5u})
        for(int word=0;word<4;++word)
            for(int budget:{1,4,9,20}) run(burst_entries[op],5,budget,false,false,0x10000,false,word);
    for(unsigned op:{0u,2u,4u})
        for(int word=-1;word<4;++word)
            for(int budget:{1,4,9,20}) run(burst_entries[op],5,budget,false,false,0x10000,false,word,true);
    require(b.fixed_port_flag_reads<a.fixed_port_flag_reads,"fixed port flag reuse exercised");
    std::printf("Fixed port flag reads: %llu reference, %llu optimized\n",
        (unsigned long long)a.fixed_port_flag_reads,(unsigned long long)b.fixed_port_flag_reads);
    std::printf("%u native-vs-MAME fixtures exact over four slices: arithmetic, helper fallbacks, 8-byte instructions, plain/signed/unaligned/page-edge loads, multiword bursts, MMIO-observed PCs, branches, stalls/resume, debug hooks, stable RAM polling/spin helper, helper-observed cycles, explicit timeslice aborts, bad-code exit and budgets 1..40\n",fixtures);
}
