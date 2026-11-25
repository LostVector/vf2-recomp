// Diagnostic only: completed i960 coprocessor transactions and virtual clocks.
#include "../../src/host/acm_checkpoint.h"
#include <cstdio>
struct fifo_trace_fixture {
    static void event(model2a &m,FILE *out,char kind,u32 address,u32 value){
        auto now=m.time();auto &dsp=m.m_sched[model2a::CPU_TGP];
        auto phase=dsp.local-dsp.period*dsp.cycles;
        std::fprintf(out,"%llu %c %08x %08x %08x %016llx:%016llx %llu %016llx:%016llx\n",
            (unsigned long long)m.frame_number(),kind,m.cpu().pip(),address,value,
            (unsigned long long)(now>>64),(unsigned long long)now,
            (unsigned long long)dsp.cycles,(unsigned long long)(phase>>64),(unsigned long long)phase);
    }
};
int main(int argc,char **argv){
    if(argc!=5)return 2;
    auto roms=romset::load(argv[1]);if(!roms.ok)return 2;
    model2a m(std::move(roms));m.release_unused_roms();m.set_byte_link(true);m.set_sound(false);m.reset();m.defer_3d(true);
    std::string error;if(!acm_checkpoint::load(m,argv[2],error)){std::fprintf(stderr,"%s\n",error.c_str());return 2;}
    FILE *out=std::fopen(argv[4],"wb");if(!out)return 2;
    m.on_copro=[&](char kind,u32 address,u32 value){fifo_trace_fixture::event(m,out,kind,address,value);};
    for(unsigned i=0;i<std::strtoul(argv[3],nullptr,10);++i){m.run_frame();m.m_in0=m.m_in1=m.m_in2=m.m_sw=255;}
    m.on_copro={};std::fclose(out);m.report_cooperative_fifo(stderr);
    std::vector<uint8_t> state;m.save_state(state);
    std::fprintf(stderr,"Core %016llx\n",(unsigned long long)m2::state_hash(state.data()+model2a::STATE_HEADER,state.size()-model2a::STATE_HEADER));
}
