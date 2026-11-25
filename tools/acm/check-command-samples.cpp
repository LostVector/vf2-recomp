#include "matrix-test-bus.h"
#include "../../src/core/tgp_sample_context.h"
static void require(bool ok,const char *message){if(!ok){std::fprintf(stderr,"Command sampling failed: %s\n",message);std::exit(1);}}
static unsigned tag(){return (vf2_tgp_sample::location.load()>>12)&255;}
struct sampled_bus:matrix_bus {
    mutable unsigned observed=0;unsigned expected=255;bool stall_input=false,dispatcher=false;
    uint64_t program_revision()const override{
        require((vf2_tgp_sample::location.load()&0xc0000000u)==0xc0000000u,"DSP-entry scope");
        require(tag()==expected,"resumed native command identity");++observed;return revision;
    }
    u32 rf_r(u32 a)override{
        if(dispatcher)require(tag()==255,"dispatcher resets command before input read");
        if(stall_input){cpu.stall();cpu.abort_timeslice();}return matrix_bus::rf_r(a);
    }
};
int main(int argc,char **argv){
    if(argc!=2||!std::getenv("VF2_TGP_COMMAND_SAMPLES"))return 2;
    sampled_bus a;FILE *f=std::fopen(argv[1],"rb");if(!f)return 2;
    bool ok=std::fread(a.program.data(),1,sizeof(a.program),f)==sizeof(a.program);std::fclose(f);if(!ok)return 2;
    // Hooks force individually visible steps. A table branch assigns its own
    // command before the hook on that command's first body instruction.
    unsigned table_cases=0;
    for(unsigned command=0;command<112;++command){
        mb86233_matrix_fixture::at(a.cpu,u16(0x3f+command));a.expected=255;
        unsigned visits=0;a.cpu.on_instruction=[&](u16){if(visits++)require(tag()==command,"command-table identity");};
        a.cpu.run(2);require(visits>=2,"both instruction hooks run");
        require(vf2_tgp_sample::location.load()==0,"no command tag after CPU return");++table_cases;
    }
    a.dispatcher=true;
    for(bool stall:{false,true}){
        mb86233_matrix_fixture::at(a.cpu,0x35,7);a.cpu.on_instruction={};a.expected=7;a.stall_input=stall;
        a.cpu.run(1);require(vf2_tgp_sample::location.load()==0,"dispatcher/stall scope exit");
    }
    a.stall_input=false;a.dispatcher=false;
    for(unsigned mode:{0u,1u,2u}){
        a.low.fill(f2u(.5f));a.high.fill(f2u(.25f));a.low[0]=0;a.low[2]=f2u(1.f);
        mb86233_matrix_fixture::seed(a.cpu,1,100,120,mode,true);
        // Set the command without disturbing this matrix's entry registers.
        // One harmless command-table step establishes its identity, then the
        // friend helper seeds entry state while retaining a chosen identity.
        const auto entry=mode==0?0xbc3:mode==1?0xbfb:0xbfc;
        mb86233_matrix_fixture::at(a.cpu,u16(entry),0x37);
        a.expected=0x37;a.cpu.on_instruction={};
        // Power-on leaves X0/X1 at zero; the arrays still provide valid,
        // nonaliasing native inputs for all three recognized entry points.
        unsigned suspended=0;const auto before=a.cpu.resumed_matrices();
        for(unsigned i=0;i<110;++i){
            a.cpu.run(1);require(vf2_tgp_sample::location.load()==0,"native early-return scope exit");
            if(mb86233_matrix_fixture::pending(a.cpu))++suspended;
            if(a.cpu.resumed_matrices()>before&&!mb86233_matrix_fixture::pending(a.cpu))break;
        }
        require(a.cpu.resumed_matrices()>before&&suspended>0,"sampling retains resumable kernels");
        a.cpu.materialize_native_matrix();
    }
    std::printf("Command sampling: %u table identities, dispatcher/stall exits, all three resumable native entries and %u observed DSP scopes pass\n",table_cases,a.observed);
}
