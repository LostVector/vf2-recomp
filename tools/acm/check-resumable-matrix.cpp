#include "matrix-test-bus.h"
static uint64_t compared=0,live_prefixes=0;
static void compare(matrix_bus &a,matrix_bus &b,unsigned mode,unsigned seed,unsigned action,unsigned step){
    const bool pending=mb86233_matrix_fixture::pending(a.cpu);
    auto sa=a.view(),sb=b.state();
    if(sa!=sb||a.cpu.compiled_instructions()!=b.cpu.compiled_instructions()||a.cpu.interpreted_instructions()!=b.cpu.interpreted_instructions()){
        size_t at=0;while(at<sa.size()&&sa[at]==sb[at])++at;
        std::fprintf(stderr,"Resumable mismatch mode %u seed %u action %u step %u offset %zu counts %llu/%llu pending %d\n",mode,seed,action,step,at,(unsigned long long)a.cpu.compiled_instructions(),(unsigned long long)b.cpu.compiled_instructions(),pending);std::exit(1);
    }
    if(pending&&!mb86233_matrix_fixture::pending(a.cpu)){std::fputs("Clone inspection cancelled live pending math\n",stderr);std::exit(1);}
    ++compared;live_prefixes+=pending;
}
int main(int argc,char **argv){
    if(argc!=2||!std::getenv("VF2_TGP_RESUMABLE_MATRIX")||!std::getenv("VF2_TGP_EXTENDED_MATRIX"))return 2;
    matrix_bus a,b;FILE *in=std::fopen(argv[1],"rb");if(!in)return 2;
    bool ok=std::fread(a.program.data(),1,sizeof(a.program),in)==sizeof(a.program);std::fclose(in);if(!ok)return 2;
    const auto original=a.program;b.program=original;b.cpu.on_instruction=[](u16){};
    const unsigned starts[]={0,1,16,32,33,44,55,56,120,244,245,500,501};
    auto prepare=[&](unsigned mode,unsigned seed,bool clean){
        a.cpu.materialize_native_matrix();b.cpu.materialize_native_matrix();
        a.alternate=b.alternate=false;a.program=b.program=original;++a.revision;++b.revision;a.isolated=b.isolated=true;
        uint32_t random=seed*1013904223u;auto next=[&](){random=random*1664525u+1013904223u;return random;};
        auto number=[&](){u32 r=next();return !clean&&seed%4==0?r:f2u(float(int(r%32769)-16384)/1024.f);};
        for(auto &v:a.low)v=number();for(auto &v:a.high)v=number();for(auto &v:a.io)v=next();for(auto &v:a.rf)v=next();
        a.low[0]=!clean&&seed%9==0?next():0;
        const unsigned x0=clean?100:starts[seed%13],x1=clean?120:starts[(seed/13)%13];
        b.low=a.low;b.high=a.high;b.io=a.io;b.rf=a.rf;
        a.cpu.on_instruction={};b.cpu.on_instruction=[](u16){};
        mb86233_matrix_fixture::seed(a.cpu,seed,x0,x1,mode,clean);mb86233_matrix_fixture::seed(b.cpu,seed,x0,x1,mode,clean);
    };
    // Every initial cycle cut, followed by single-cycle budgets, checks exact
    // two-cycle overshoot and persistent cached execution across many runs.
    for(unsigned mode:{0u,1u,2u})for(unsigned first=1;first<=101;++first){
        prepare(mode,1,true);
        for(unsigned step=0;step<103;++step){
            const int budget=step?1:first;
            int ca=a.cpu.run(budget),cb=b.cpu.run(budget);
            if(ca!=cb){std::fprintf(stderr,"Cycle-cut mismatch mode %u first %u step %u %d/%d\n",mode,first,step,ca,cb);return 1;}
            compare(a,b,mode,first,0,step);
        }
    }
    uint64_t fixtures=0;
    for(unsigned mode:{0u,1u,2u})for(unsigned seed=1;seed<=350;++seed)for(unsigned action=0;action<10;++action){
        prepare(mode,seed,seed%3==0);
        if(action==6)a.isolated=b.isolated=false;
        const auto resumed=a.cpu.resumed_matrices();
        unsigned hooks=0;
        for(unsigned step=0;step<14;++step){
            if(step==2){
                if(action==1){auto bytes=a.view();if(!a.load(bytes))return 1;}
                if(action==2)a.cpu.on_instruction=[&](u16){++hooks;};
                if(action==3){a.cpu.reset();b.cpu.reset();}
                if(action==4){a.program[mode==0?0xbc3:mode==1?0xbfb:0xbfc]^=1;b.program=a.program;++a.revision;++b.revision;}
                if(action==5){a.cpu.gpio0_w(1);b.cpu.gpio0_w(1);a.cpu.gpio1_w(0);b.cpu.gpio1_w(0);a.cpu.gpio2_w(1);b.cpu.gpio2_w(1);a.cpu.gpio3_w(1);b.cpu.gpio3_w(1);}
                if(action==7&&a.cpu.pc()!=b.cpu.pc())return 1;
                if(action==8){unsigned pc=b.cpu.pc()&4095;a.program[pc]^=1;b.program=a.program;++a.revision;++b.revision;}
                if(action==9){a.alternate_program=a.program;b.alternate_program=b.program;a.alternate=b.alternate=true;}
                compare(a,b,mode,seed,action,step);
            }
            if(step==5&&action==2)a.cpu.on_instruction={};
            unsigned budget=1+(seed*17+step*11)%41;
            int ca=a.cpu.run(budget),cb=b.cpu.run(budget);
            if(ca!=cb){std::fprintf(stderr,"Slice-cycle mismatch mode %u seed %u action %u step %u %d/%d\n",mode,seed,action,step,ca,cb);return 1;}
            compare(a,b,mode,seed,action,step);
        }
        if(action==2&&!hooks)return 1;
        if(action==6&&a.cpu.resumed_matrices()!=resumed){std::fputs("Nonisolated bus entered deferred math\n",stderr);return 1;}
        ++fixtures;
    }
    if(!live_prefixes||!a.cpu.resumed_matrices())return 1;
    std::printf("Resumable matrices: %llu fixtures, %llu exact state/cycle comparisons, %llu live prefixes observed without cancelling execution; every cycle cut, save/load, debugger, reset, past/future mutation, program-pointer change, GPIO, isolation and PC observation pass\n",(unsigned long long)fixtures,(unsigned long long)compared,(unsigned long long)live_prefixes);
}
