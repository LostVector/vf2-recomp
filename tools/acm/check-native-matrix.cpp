#include "matrix-test-bus.h"
int main(int argc,char **argv){
    if(argc!=2||!std::getenv("VF2_TGP_NATIVE_MATRIX"))return 2;
    matrix_bus a,b;FILE *in=std::fopen(argv[1],"rb");if(!in)return 2;
    bool ok=std::fread(a.program.data(),1,sizeof(a.program),in)==sizeof(a.program);std::fclose(in);if(!ok)return 2;
    b.program=a.program;b.cpu.on_instruction=[](u16){};
    const unsigned starts[]={0,1,16,32,33,44,55,56,120,244,245,500,501};
    const uint32_t edge[]={0,0x80000000,0x00000001,0x007fffff,0x00800000,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc12345,0x7fa23456};
    unsigned fixtures=0;
    uint64_t mode_calls[3]{};
    for(unsigned mode:{0u,1u,2u})for(unsigned seed=1;seed<=2500;++seed)for(unsigned budget:{1u,94u,95u,96u,99u,100u,101u,137u}){
        const bool reverse=mode!=0;
        const unsigned total=mode==0?95:mode==1?100:99;
        uint32_t random=seed*1013904223u;
        auto next=[&](){random=random*1664525u+1013904223u;return random;};
        auto number=[&](){uint32_t r=next();return seed%4==0?r:f2u(float(int(r%32769)-16384)/1024.f);};
        for(auto &v:a.low)v=number();for(auto &v:a.high)v=number();for(auto &v:a.io)v=next();for(auto &v:a.rf)v=next();
        const unsigned x0=seed%502,x1=starts[seed%13];
        a.low[0]=seed%9==0?next():0;
        if(seed%5==0)a.high[x0]=edge[(seed/5)%11];
        b.low=a.low;b.high=a.high;b.io=a.io;b.rf=a.rf;
        mb86233_matrix_fixture::seed(a.cpu,seed,x0,x1,mode);mb86233_matrix_fixture::seed(b.cpu,seed,x0,x1,mode);
        const auto before=a.cpu.native_matrices();
        const int ca=a.cpu.run(budget),cb=b.cpu.run(budget);auto sa=a.state(),sb=b.state();
        if(ca!=cb||sa!=sb||a.cpu.compiled_instructions()!=b.cpu.compiled_instructions()){
            size_t offset=0;while(offset<sa.size()&&offset<sb.size()&&sa[offset]==sb[offset])++offset;
            std::fprintf(stderr,"Mismatch mode %d seed %u budget %u cycles %d/%d state offset %zu native %llu\n",mode,seed,budget,ca,cb,offset,(unsigned long long)(a.cpu.native_matrices()-before));return 1;
        }
        if(budget<total&&a.cpu.native_matrices()!=before)return 1;
        mode_calls[mode]+=a.cpu.native_matrices()-before;
        ++fixtures;
    }
    if(!mode_calls[0]||!mode_calls[1]||!mode_calls[2]){std::fprintf(stderr,"A native matrix variant never ran\n");return 1;}
    const uint32_t original=a.program[0xbc5];
    for(bool restored:{false,true})for(unsigned mode:{0u,1u,2u}){
        a.program[0xbc5]=b.program[0xbc5]=restored?original:original^1u;++a.revision;++b.revision;
        a.low.fill(f2u(.5f));a.high.fill(f2u(.25f));a.low[0]=0;a.low[2]=f2u(1.f);
        b.low=a.low;b.high=a.high;b.io=a.io;b.rf=a.rf;
        mb86233_matrix_fixture::seed(a.cpu,1,100,120,mode,true);mb86233_matrix_fixture::seed(b.cpu,1,100,120,mode,true);
        const auto before=a.cpu.native_matrices();
        const int ca=a.cpu.run(mode==0?95:mode==1?100:99),cb=b.cpu.run(mode==0?95:mode==1?100:99);
        if(ca!=cb||a.state()!=b.state()||a.cpu.native_matrices()-before!=unsigned(restored)){
            std::fprintf(stderr,"Revision guard mismatch restored %d mode %d\n",restored,mode);return 1;
        }
    }
    std::printf("%u matrix fixtures match single handlers, native calls %llu/%llu/%llu: short budgets, alias fallback, arbitrary bits, subnormals, infinities and NaNs\n",fixtures,(unsigned long long)mode_calls[0],(unsigned long long)mode_calls[1],(unsigned long long)mode_calls[2]);
    std::puts("Microcode mutation disables all native matrix entries; restoration re-enables all");
}
