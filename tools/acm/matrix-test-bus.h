#pragma once
// Differential checks for native matrix arithmetic, complete DSP state and RAM.
#include "../../src/core/mb86233.h"
#include "../../src/machine/state_io.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

struct mb86233_matrix_fixture {
    static void at(mb86233_device &c,u16 pc,u8 command=255){c.power_on();c.m_pc=pc;c.m_sample_command=command;}
    static void reconnect(mb86233_device &c,mb86233_bus &bus,u32 *low,u32 *high){c.m_bus=&bus;c.m_fast_ram={low,high};c.m_fast_ram_checked=true;}
    static void rotation(mb86233_device &c,u16 pc,unsigned x0,unsigned x1,bool private_body,uint32_t seed){
        mb86233_matrix_fixture::seed(c,seed,x0,x1,0,true);
        c.m_pc=pc;
        if(private_body){c.m_b0=0;c.m_b1=0x200;}
        if(seed%13==0)c.m_r=2;
    }
    static bool pending(const mb86233_device &c){return c.m_pending_matrix.active;}

    static void seed(mb86233_device &c,uint32_t seed,unsigned x0,unsigned x1,unsigned mode,bool clean=false){
        auto next=[&](){seed=seed*1664525u+1013904223u;return seed;};
        c.power_on();c.m_pc=mode==0?0xbc3:mode==1?0xbfb:0xbfc;c.m_ppc=0;
        c.m_a=next();c.m_b=next();c.m_d=next();c.m_p=next();
        c.m_alu_stmask=next();c.m_alu_stset=next();c.m_alu_r1=next();c.m_alu_r2=next();
        c.m_st=next();c.m_b0=next();c.m_b1=next();c.m_x0=x0;c.m_x1=x1;
        c.m_i0=next();c.m_i1=next();c.m_c0=next();c.m_c1=next();
        c.m_rpc=next();c.m_sft=next();c.m_m=next();c.m_vsmr=next();
        c.m_r=seed%7==0?3:1;c.m_stall=seed%11==0;
        if(clean){c.m_r=1;c.m_stall=false;}
    }
};
struct matrix_bus:mb86233_bus {
    std::array<u32,4096> program{},alternate_program{};bool alternate=false;
    std::array<u32,256> low{};
    std::array<u32,512> high{};
    std::array<u32,64> io{};
    std::array<u32,16> rf{};
    uint64_t revision=1;bool isolated=true;
    mb86233_device cpu{*this};
    const u32 *program_data()const override{return alternate?alternate_program.data():program.data();}
    uint64_t program_revision()const override{return revision;}
    internal_ram direct_internal_ram()override{return {low.data(),high.data()};}
    bool isolated_internal_ram()const override{return isolated;}
    u32 prog_r(u32 a)override{return program_data()[a&4095];}
    u32 data_r(u32 a)override{a&=65535;return a<256?low[a]:a-512<512?high[a-512]:0;}
    void data_w(u32 a,u32 v)override{a&=65535;if(a<256)low[a]=v;else if(a-512<512)high[a-512]=v;}
    u32 io_r(u32 a)override{return io[a&63];}
    void io_w(u32 a,u32 v)override{io[a&63]=v;}
    u32 rf_r(u32 a)override{return rf[a&15];}
    void rf_w(u32 a,u32 v)override{rf[a&15]=v;}
    std::vector<uint8_t> view(){matrix_bus shadow=*this;mb86233_matrix_fixture::reconnect(shadow.cpu,shadow,shadow.low.data(),shadow.high.data());return shadow.state();}
    bool load(const std::vector<uint8_t> &bytes){m2::StateReader rd(bytes.data(),bytes.size());cpu.state(rd);rd.pod(low);rd.pod(high);rd.pod(io);rd.pod(rf);return rd.done();}
    std::vector<uint8_t> state(){std::vector<uint8_t> out;m2::StateWriter w(out);cpu.state(w);w.pod(low);w.pod(high);w.pod(io);w.pod(rf);return out;}
};
