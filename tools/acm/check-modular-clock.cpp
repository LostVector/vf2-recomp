#include "../../src/host/acm_checkpoint.h"
#include <array>
#include <cstdio>
#include <string_view>
struct modular_clock_fixture {
    static void prepare_rollover(model2a &m){
        using T=model2a::atime;
        const T lead=model2a::AS_PER_SEC/200;
        const T limb=T(1)<<64;
        T remaining=limb-T(uint64_t(m.m_base));
        if(remaining<lead)remaining+=limb;
        const T shift=remaining-lead;
        m.m_tgp_cycle_base+=uint64_t(shift/m.m_sched[model2a::CPU_TGP].period);
        for(auto &cpu:m.m_sched){
            const auto cycles=uint64_t(shift/cpu.period);
            cpu.local+=T(cycles)*cpu.period;
            cpu.cycles+=cycles;
        }
        for(auto &timer:m.m_timer)if(timer.enabled){
            timer.start+=shift;
            timer.expire+=shift;
        }
        m.m_base+=shift;
        m.rebuild_timer_queue();
        m.m_timerrun[0]=m.m_timerrun[1]=true;
        m.timer_adjust(model2a::T_TIMER0,lead+100);
        m.timer_adjust(model2a::T_TIMER1,lead+100);
    }
    static uint64_t high_time(const model2a &m){return uint64_t(m.m_base>>64);}
    static void run(model2a &m){
        using T=model2a::atime;
        const T saved_base=m.m_base;
        std::array<model2a::timer,model2a::N_TIMERS> timers;
        std::array<model2a::cpu_sched,model2a::N_CPUS> cpus;
        std::copy_n(m.m_timer,timers.size(),timers.begin());std::copy_n(m.m_sched,cpus.size(),cpus.begin());
        const bool sound=m.m_sound_on,only=m.m_sound_only;
        unsigned checks=0;
        auto require=[&](bool expected,const char *label){
            if(m.modular_frame_ready()!=expected){std::fprintf(stderr,"Clock window guard mismatch: %s\n",label);std::exit(1);}++checks;
        };
        auto seed=[&](){
            std::copy(cpus.begin(),cpus.end(),m.m_sched);std::copy(timers.begin(),timers.end(),m.m_timer);
            m.m_base=(T(1)<<80)+T(UINT64_MAX)-500;
            for(auto &t:m.m_timer)t.enabled=false;
            for(auto &c:m.m_sched)c.local=m.m_base;
            m.m_timer[model2a::T_VBLANK_START].enabled=true;
            m.m_timer[model2a::T_VBLANK_START].expire=m.m_base+model2a::AS_PER_SEC/60;
            m.m_sound_on=false;m.m_sound_only=false;
        };
        seed();require(true,"crossing low-limb wrap at a large epoch");
        for(int id:{model2a::CPU_I960,model2a::CPU_TGP,model2a::CPU_Z80}){
            seed();m.m_sched[id].local-=model2a::AS_PER_SEC+1;require(false,"stale CPU");
            seed();m.m_sched[id].local+=model2a::AS_PER_SEC+1;require(false,"ahead CPU");
            seed();m.m_sched[id].period=0;require(false,"zero period");
            seed();m.m_sched[id].period=T(1)<<64;require(false,"wide period");
            seed();m.m_sched[id].period=model2a::AS_PER_SEC/1000000+1;require(false,"slow period");
        }
        seed();m.m_timer[model2a::T_VBLANK_START].enabled=false;require(false,"missing frame timer");
        seed();m.m_timer[model2a::T_VBLANK_START].expire=m.m_base-1;require(false,"past frame timer");
        seed();m.m_timer[model2a::T_VBLANK_START].expire=m.m_base+model2a::AS_PER_SEC+1;require(false,"distant frame timer");
        seed();m.m_timer[model2a::T_TIMER0].enabled=true;m.m_timer[model2a::T_TIMER0].expire=m.m_base-model2a::AS_PER_SEC-1;require(false,"old expired timer");
        seed();m.m_timer[model2a::T_TIMER0].enabled=true;m.m_timer[model2a::T_TIMER0].expire=m.m_base+model2a::AS_PER_SEC*100;require(true,"long future timer retains wide ordering");
        seed();m.m_sched[model2a::CPU_68K].local=0;require(true,"halted auxiliary clock is deferred");
        seed();m.m_sound_on=true;require(false,"active sound board");
        seed();m.m_sound_only=true;require(false,"sound-only board");
        seed();m.m_base=T(1)<<101;require(false,"out-of-domain absolute epoch");
        seed();m.on_instruction=[](u32){};require(false,"i960 tracing");m.on_instruction={};
        seed();m.on_68k_instruction=[](u32){};require(false,"68K tracing");m.on_68k_instruction={};
        seed();m.cpu().on_instruction=[](offs_t){};require(false,"latched i960 hook");m.cpu().on_instruction={};
        seed();m.tgp().on_instruction=[](u16){};require(false,"DSP tracing");m.tgp().on_instruction={};
        seed();m.on_copro=[](char,u32,u32){};require(false,"external copro callback");m.on_copro={};
        seed();m.on_milestone=[](const std::string&){};require(false,"external milestone callback");m.on_milestone={};
        std::copy(timers.begin(),timers.end(),m.m_timer);std::copy(cpus.begin(),cpus.end(),m.m_sched);
        m.m_base=saved_base;m.m_sound_on=sound;m.m_sound_only=only;
        std::printf("Modular scheduler: %u window, rollover, tracing, sound and fallback guards pass\n",checks);
    }
};
int main(int argc,char **argv){
    if(argc!=3&&argc!=4)return 2;
    const std::string_view mode=argc==4?argv[3]:"";
    if(!mode.empty()&&mode!="rollover"&&mode!="rollover-reference")return 2;
    const bool rollover=!mode.empty(), modular=mode!="rollover-reference";
    auto roms=romset::load(argv[1]);if(!roms.ok)return 2;
    model2a m(std::move(roms));m.release_unused_roms();m.set_byte_link(true);m.set_sound(false);m.reset();m.defer_3d(true);
    std::string error;if(!acm_checkpoint::load(m,argv[2],error)){std::fprintf(stderr,"%s\n",error.c_str());return 2;}
    if(!rollover)modular_clock_fixture::run(m);
    if(rollover)modular_clock_fixture::prepare_rollover(m);
    const uint64_t initial_high=modular_clock_fixture::high_time(m);
    for(unsigned i=0;i<20;++i){m.run_frame();m.m_in0=m.m_in1=m.m_in2=m.m_sw=255;}
    const auto &picture=m.render();std::vector<uint8_t> state;m.save_state(state);
    const auto core=m2::state_hash(state.data()+model2a::STATE_HEADER,state.size()-model2a::STATE_HEADER);
    const auto pixels=m2::state_hash(reinterpret_cast<const uint8_t*>(picture.data()),size_t(picture.width())*picture.height()*4);
    if(rollover){
        if(modular_clock_fixture::high_time(m)!=initial_high+1||m.modular_scheduler_frames()!=(modular?20:0))return 1;
        std::printf("Rollover with equal-time timers: core %016llx pixels %016llx\n",(unsigned long long)core,(unsigned long long)pixels);
        return 0;
    }
    if(core!=0xd46dd64a8dbc0653ull||pixels!=0x1e01b6bd873de77full||m.modular_scheduler_frames()!=20){std::fprintf(stderr,"Modular game mismatch: core %016llx pixels %016llx fast frames %llu\n",(unsigned long long)core,(unsigned long long)pixels,(unsigned long long)m.modular_scheduler_frames());return 1;}
    std::puts("All 20 game frames exercise modular scheduling and match the independent saved core/pixel golden");
}
