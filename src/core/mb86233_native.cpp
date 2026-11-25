// Experimental ARM translator for Model 2 TGP instructions and repetitions.
// Capability-gated bus callbacks keep datapath registers resident at I/O.
#include "mb86233.h"
#include "../../third_party/sljit/sljitLir.h"
#include <array>
#include <vector>
#include <cstdio>
#include <memory>
#if defined(__arm__)
struct mb86233_native_generator {
    using C=mb86233_device;
    sljit_compiler *c=sljit_create_compiler(nullptr);C &cpu;const bool native_io;
    std::array<sljit_label*,4096> labels{};
    std::array<sljit_uw,4096> entries{},repeat_entries{};
    std::array<sljit_label*,4096> repeat_labels{};
    struct edge{sljit_jump *jump;unsigned pc,next;};std::vector<edge> exits;
    struct link{sljit_jump *jump;unsigned target;};std::vector<link> links;
    std::vector<sljit_jump*> dynamic_dispatch;
    std::vector<std::pair<sljit_jump*,unsigned>> stalled_reads;
    std::array<bool,4096> eligible{};
    void *code=nullptr;using fn=unsigned(*)(C*,sljit_uw);fn enter=nullptr;
    const bool vfp_cache=std::getenv("VF2_TGP_VFP_CACHE")!=nullptr;
    static constexpr int A=100,B=101,D=102,P=103;
    const int ST=vfp_cache?SLJIT_S3:SLJIT_S7;
    static constexpr u32 MASK=C::F_ZRD|C::F_SGD|C::F_CPD|C::F_OVD|C::F_DVZD;
    bool cached(int r)const{return r>=A&&r<=P;}
    bool floating_cache(int r)const{return vfp_cache&&cached(r);}
    int integer(int r)const{return cached(r)?SLJIT_S3-(r-A):r;}
    int floating_register(int r)const{return SLJIT_FS0-(r-A);}
    sljit_sw off(const void *p){return reinterpret_cast<const char*>(p)-reinterpret_cast<const char*>(&cpu);}
    void mov(int dst,int src){
        if(floating_cache(dst)){
            if(floating_cache(src))sljit_emit_fop1(c,SLJIT_MOV_F32,floating_register(dst),0,floating_register(src),0);
            else sljit_emit_fcopy(c,SLJIT_COPY32_TO_F32,floating_register(dst),integer(src));
        }else if(floating_cache(src))sljit_emit_fcopy(c,SLJIT_COPY32_FROM_F32,floating_register(src),integer(dst));
        else sljit_emit_op1(c,SLJIT_MOV32,integer(dst),0,integer(src),0);
    }
    void imm(int dst,u32 value){if(floating_cache(dst)){sljit_emit_op1(c,SLJIT_MOV32,SLJIT_R0,0,SLJIT_IMM,sljit_sw(value));mov(dst,SLJIT_R0);}else sljit_emit_op1(c,SLJIT_MOV32,integer(dst),0,SLJIT_IMM,sljit_sw(value));}
    void load(int dst,const void *p,int op=SLJIT_MOV_U32){if(floating_cache(dst))sljit_emit_fop1(c,SLJIT_MOV_F32,floating_register(dst),0,SLJIT_MEM1(SLJIT_S0),off(p));else sljit_emit_op1(c,op,integer(dst),0,SLJIT_MEM1(SLJIT_S0),off(p));}
    void store(const void *p,int src,int op=SLJIT_MOV32){if(floating_cache(src))sljit_emit_fop1(c,SLJIT_MOV_F32,SLJIT_MEM1(SLJIT_S0),off(p),floating_register(src),0);else sljit_emit_op1(c,op,SLJIT_MEM1(SLJIT_S0),off(p),integer(src),0);}
    void constant(const void *p,u32 value,int op=SLJIT_MOV32){sljit_emit_op1(c,op,SLJIT_MEM1(SLJIT_S0),off(p),SLJIT_IMM,sljit_sw(value));}
    void binary(int op,int dst,int left,int right){
        const int target=floating_cache(dst)?SLJIT_R0:integer(dst);
        int lhs=integer(left),rhs=integer(right);
        if(floating_cache(left)){mov(target,left);lhs=target;}
        if(floating_cache(right)){mov(SLJIT_R2,right);rhs=SLJIT_R2;}
        sljit_emit_op2(c,op,target,0,lhs,0,rhs,0);
        if(floating_cache(dst))mov(dst,target);
    }
    void immediate(int op,int dst,int src,u32 value){
        const int target=floating_cache(dst)?SLJIT_R0:integer(dst);int source=integer(src);
        if(floating_cache(src)){mov(target,src);source=target;}
        sljit_emit_op2(c,op,target,0,source,0,SLJIT_IMM,sljit_sw(value));if(floating_cache(dst))mov(dst,target);
    }
    void local_store(int at,int src){if(floating_cache(src))sljit_emit_fop1(c,SLJIT_MOV_F32,SLJIT_MEM1(SLJIT_SP),at,floating_register(src),0);else sljit_emit_op1(c,SLJIT_MOV32,SLJIT_MEM1(SLJIT_SP),at,integer(src),0);}
    void local_load(int dst,int at){if(floating_cache(dst))sljit_emit_fop1(c,SLJIT_MOV_F32,floating_register(dst),0,SLJIT_MEM1(SLJIT_SP),at);else sljit_emit_op1(c,SLJIT_MOV_U32,integer(dst),0,SLJIT_MEM1(SLJIT_SP),at);}
    int cache(unsigned r){switch(r&63){case 0x10:return A;case 0x13:return B;case 0x19:return D;case 0x1c:return P;default:return 0;}}
    bool readable(unsigned r){r&=63;return r<=3||r==12||r==13||(r>=16&&r<=21)||(r>=25&&r<=31)||r==52;}
    bool writable(unsigned r){r&=63;return r<=3||r==5||r==6||r==8||r==10||r==12||r==13||r==15||(r>=16&&r<=21)||(r>=25&&r<=31)||r==52||r==60;}
    void readreg(unsigned r){
        r&=63;if(int host=cache(r)){mov(SLJIT_R0,host);return;}
        switch(r){
        case 0:load(SLJIT_R0,&cpu.m_b0,SLJIT_MOV_U16);return;case 1:load(SLJIT_R0,&cpu.m_b1,SLJIT_MOV_U16);return;
        case 2:load(SLJIT_R0,&cpu.m_x0,SLJIT_MOV_U16);return;case 3:load(SLJIT_R0,&cpu.m_x1,SLJIT_MOV_U16);return;
        case 12:load(SLJIT_R0,&cpu.m_c0,SLJIT_MOV_U8);return;case 13:load(SLJIT_R0,&cpu.m_c1,SLJIT_MOV_U8);return;
        case 31:load(SLJIT_R0,&cpu.m_sft,SLJIT_MOV_U8);return;case 52:load(SLJIT_R0,&cpu.m_rpc,SLJIT_MOV_U8);return;
        }
        int host=cache(r-((r==17||r==20||r==26||r==29)?1:2));
        if(r==17||r==20||r==26||r==29){immediate(SLJIT_LSHR32,SLJIT_R0,host,23);immediate(SLJIT_AND32,SLJIT_R0,SLJIT_R0,255);}
        else{mov(SLJIT_R0,host);auto *pos=sljit_emit_cmp(c,SLJIT_SIG_GREATER_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,0);immediate(SLJIT_OR32,SLJIT_R0,SLJIT_R0,0x7f800000);auto *done=sljit_emit_jump(c,SLJIT_JUMP);sljit_set_label(pos,sljit_emit_label(c));immediate(SLJIT_AND32,SLJIT_R0,SLJIT_R0,0x807fffff);sljit_set_label(done,sljit_emit_label(c));}
    }
    void writereg(unsigned r){
        r&=63;if(int host=cache(r)){mov(host,SLJIT_R0);return;}
        switch(r){
        case 0:store(&cpu.m_b0,SLJIT_R0,SLJIT_MOV_U16);return;case 1:store(&cpu.m_b1,SLJIT_R0,SLJIT_MOV_U16);return;
        case 2:store(&cpu.m_x0,SLJIT_R0,SLJIT_MOV_U16);return;case 3:store(&cpu.m_x1,SLJIT_R0,SLJIT_MOV_U16);return;
        case 5:store(&cpu.m_i0,SLJIT_R0,SLJIT_MOV_U16);return;case 6:store(&cpu.m_i1,SLJIT_R0,SLJIT_MOV_U16);return;
        case 8:store(&cpu.m_sp,SLJIT_R0,SLJIT_MOV_U16);return;case 15:return;
        case 10:immediate(SLJIT_AND32,SLJIT_R0,SLJIT_R0,7);store(&cpu.m_vsm,SLJIT_R0,SLJIT_MOV_U8);imm(SLJIT_R1,8);binary(SLJIT_SHL32,SLJIT_R1,SLJIT_R1,SLJIT_R0);immediate(SLJIT_SUB32,SLJIT_R1,SLJIT_R1,1);store(&cpu.m_vsmr,SLJIT_R1,SLJIT_MOV_U16);return;
        case 12:case 13:{
            store(r==12?&cpu.m_c0:&cpu.m_c1,SLJIT_R0,SLJIT_MOV_U8);immediate(SLJIT_AND32,SLJIT_R0,SLJIT_R0,255);
            const u32 flag=r==12?C::F_ZC0:C::F_ZC1;immediate(SLJIT_AND32,ST,ST,~flag);auto *done=sljit_emit_cmp(c,SLJIT_NOT_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,1);immediate(SLJIT_OR32,ST,ST,flag);sljit_set_label(done,sljit_emit_label(c));return;
        }
        case 31:store(&cpu.m_sft,SLJIT_R0,SLJIT_MOV_U8);return;case 52:store(&cpu.m_rpc,SLJIT_R0,SLJIT_MOV_U8);return;case 60:store(&cpu.m_mask,SLJIT_R0,SLJIT_MOV_U16);return;
        }
        int host=cache(r-((r==17||r==20||r==26||r==29)?1:2));
        if(r==17||r==20||r==26||r==29){immediate(SLJIT_AND32,SLJIT_R1,host,0x807fffff);immediate(SLJIT_AND32,SLJIT_R0,SLJIT_R0,255);immediate(SLJIT_SHL32,SLJIT_R0,SLJIT_R0,23);binary(SLJIT_OR32,host,SLJIT_R0,SLJIT_R1);}
        else{immediate(SLJIT_AND32,SLJIT_R1,host,0x7f800000);immediate(SLJIT_AND32,SLJIT_R2,SLJIT_R0,0x00800000);immediate(SLJIT_SHL32,SLJIT_R2,SLJIT_R2,8);immediate(SLJIT_AND32,SLJIT_R0,SLJIT_R0,0x007fffff);binary(SLJIT_OR32,SLJIT_R0,SLJIT_R0,SLJIT_R2);binary(SLJIT_OR32,host,SLJIT_R0,SLJIT_R1);}
    }
    void ea(unsigned r,bool bank){
        auto *x=bank?&cpu.m_x1:&cpu.m_x0;auto *b=bank?&cpu.m_b1:&cpu.m_b0;
        if(!(r&0x180)){imm(SLJIT_R0,r&127);return;}
        load(SLJIT_R0,x,SLJIT_MOV_U16);
        if((r&0x180)==0x180){if(r&0x40){load(SLJIT_R1,&cpu.m_vsmr,SLJIT_MOV_U16);binary(SLJIT_AND32,SLJIT_R0,SLJIT_R0,SLJIT_R1);}if(!(r&0x20)){load(SLJIT_R1,b,SLJIT_MOV_U16);binary(SLJIT_ADD32,SLJIT_R0,SLJIT_R0,SLJIT_R1);}}
        else{load(SLJIT_R1,b,SLJIT_MOV_U16);binary(SLJIT_ADD32,SLJIT_R0,SLJIT_R0,SLJIT_R1);immediate(SLJIT_ADD32,SLJIT_R0,SLJIT_R0,r&127);}
        immediate(SLJIT_AND32,SLJIT_R0,SLJIT_R0,65535);
    }
    void post(unsigned r,bool bank){
        if(!(r&0x100))return;auto *x=bank?&cpu.m_x1:&cpu.m_x0;load(SLJIT_R0,x,SLJIT_MOV_U16);
        if(r&0x80)immediate(SLJIT_ADD32,SLJIT_R0,SLJIT_R0,u32(util::sext(r,5)));
        else{load(SLJIT_R1,bank?&cpu.m_i1:&cpu.m_i0,SLJIT_MOV_U16);binary(SLJIT_ADD32,SLJIT_R0,SLJIT_R0,SLJIT_R1);}
        store(x,SLJIT_R0,SLJIT_MOV_U16);
    }
    void memory_load(int dst,int address,sljit_sw offset){
        if(floating_cache(dst))sljit_emit_fop1(c,SLJIT_MOV_F32,floating_register(dst),0,address,offset);
        else sljit_emit_op1(c,SLJIT_MOV_U32,integer(dst),0,address,offset);
    }
    void memory_store(int address,sljit_sw offset,int src){
        if(floating_cache(src))sljit_emit_fop1(c,SLJIT_MOV_F32,address,offset,floating_register(src),0);
        else sljit_emit_op1(c,SLJIT_MOV32,address,offset,integer(src),0);
    }
    bool integer_alu(unsigned a){return (a>=1&&a<=4)||a==14||(a>=22&&a<=27);}
    void readmem(unsigned r,bool bank,unsigned add,int target=SLJIT_R2){
        if(!(r&0x180)){
            load(SLJIT_R1,add?&cpu.m_fast_ram.high:&cpu.m_fast_ram.low,SLJIT_MOV);
            memory_load(target,SLJIT_MEM1(SLJIT_R1),4*(r&127));return;
        }
        ea(r,bank);if(add)immediate(SLJIT_ADD32,SLJIT_R0,SLJIT_R0,add);immediate(SLJIT_AND32,SLJIT_R0,SLJIT_R0,65535);
        auto *high=sljit_emit_cmp(c,SLJIT_GREATER_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,256);
        load(SLJIT_R1,&cpu.m_fast_ram.low,SLJIT_MOV);memory_load(target,SLJIT_MEM2(SLJIT_R1,SLJIT_R0),2);auto *done1=sljit_emit_jump(c,SLJIT_JUMP);
        sljit_set_label(high,sljit_emit_label(c));immediate(SLJIT_SUB32,SLJIT_R0,SLJIT_R0,512);auto *zero=sljit_emit_cmp(c,SLJIT_GREATER_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,512);
        load(SLJIT_R1,&cpu.m_fast_ram.high,SLJIT_MOV);memory_load(target,SLJIT_MEM2(SLJIT_R1,SLJIT_R0),2);auto *done2=sljit_emit_jump(c,SLJIT_JUMP);
        sljit_set_label(zero,sljit_emit_label(c));imm(target,0);auto *done=sljit_emit_label(c);sljit_set_label(done1,done);sljit_set_label(done2,done);
    }
    void writemem(unsigned r,unsigned add,int source=SLJIT_R2){
        if(!(r&0x180)){
            load(SLJIT_R1,add?&cpu.m_fast_ram.high:&cpu.m_fast_ram.low,SLJIT_MOV);
            memory_store(SLJIT_MEM1(SLJIT_R1),4*(r&127),source);return;
        }
        ea(r,true);if(add)immediate(SLJIT_ADD32,SLJIT_R0,SLJIT_R0,add);immediate(SLJIT_AND32,SLJIT_R0,SLJIT_R0,65535);
        auto *high=sljit_emit_cmp(c,SLJIT_GREATER_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,256);load(SLJIT_R1,&cpu.m_fast_ram.low,SLJIT_MOV);memory_store(SLJIT_MEM2(SLJIT_R1,SLJIT_R0),2,source);auto *done1=sljit_emit_jump(c,SLJIT_JUMP);
        sljit_set_label(high,sljit_emit_label(c));immediate(SLJIT_SUB32,SLJIT_R0,SLJIT_R0,512);auto *done2=sljit_emit_cmp(c,SLJIT_GREATER_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,512);load(SLJIT_R1,&cpu.m_fast_ram.high,SLJIT_MOV);memory_store(SLJIT_MEM2(SLJIT_R1,SLJIT_R0),2,source);auto *done=sljit_emit_label(c);sljit_set_label(done1,done);sljit_set_label(done2,done);post(r,true);
    }
    bool alu_ok(unsigned a){return a<=14||a==16||a==17||a==19||a==20||(a>=22&&a<=27);}
    bool floating(unsigned a){return (a>=5&&a<=13)||a==16||a==17||a==19||a==20;}
    void fp(int op,int left,int right,int output){
        if(vfp_cache)sljit_emit_fop2(c,op,SLJIT_FR0,0,floating_register(left),0,floating_register(right),0);
        else{sljit_emit_fcopy(c,SLJIT_COPY32_TO_F32,SLJIT_FR0,integer(left));sljit_emit_fcopy(c,SLJIT_COPY32_TO_F32,SLJIT_FR1,integer(right));sljit_emit_fop2(c,op,SLJIT_FR0,0,SLJIT_FR0,0,SLJIT_FR1,0);}
        sljit_emit_fcopy(c,SLJIT_COPY32_FROM_F32,SLJIT_FR0,output);
    }
    void alu_pre(unsigned a){
        if(!a)return;constant(&cpu.m_alu_stmask,a==8?0:MASK);
        if(a<=4){if(a==4)immediate(SLJIT_XOR32,SLJIT_R3,D,~0u);else binary(a==1?SLJIT_AND32:a==2?SLJIT_OR32:SLJIT_XOR32,SLJIT_R3,D,A);}
        else if(a==5||a==6||a==7)fp(a==6?SLJIT_ADD_F32:SLJIT_SUB_F32,D,A,SLJIT_R3);
        else if(a==8)fp(SLJIT_MUL_F32,A,B,SLJIT_R3);
        else if(a==9||a==10||a==13){
            if(a==13)mov(SLJIT_R3,P);else fp(a==9?SLJIT_ADD_F32:SLJIT_SUB_F32,D,P,SLJIT_R3);
            store(&cpu.m_alu_r1,SLJIT_R3);fp(SLJIT_MUL_F32,A,B,SLJIT_R2);store(&cpu.m_alu_r2,SLJIT_R2);
        }
        else if(a==11)immediate(SLJIT_AND32,SLJIT_R3,D,0x7fffffff);
        else if(a==12)fp(SLJIT_ADD_F32,D,P,SLJIT_R3);
        else if(a==14){mov(SLJIT_R0,D);sljit_emit_fop1(c,SLJIT_CONV_F32_FROM_S32,SLJIT_FR0,0,SLJIT_R0,0);sljit_emit_fcopy(c,SLJIT_COPY32_FROM_F32,SLJIT_FR0,SLJIT_R3);}
        else if(a==16)fp(SLJIT_DIV_F32,D,A,SLJIT_R3);
        else if(a==17){mov(SLJIT_R3,D);auto *zero=sljit_emit_cmp(c,SLJIT_EQUAL|SLJIT_32,SLJIT_R3,0,SLJIT_IMM,0);immediate(SLJIT_XOR32,SLJIT_R3,SLJIT_R3,0x80000000);sljit_set_label(zero,sljit_emit_label(c));}
        else if(a==19||a==20)fp(a==19?SLJIT_ADD_F32:SLJIT_SUB_F32,B,A,SLJIT_R3);
        else if(a>=22&&a<=25){load(SLJIT_R0,&cpu.m_sft,SLJIT_MOV_U8);immediate(SLJIT_AND32,SLJIT_R0,SLJIT_R0,31);binary(a==22?SLJIT_LSHR32:a==24?SLJIT_ASHR32:SLJIT_SHL32,SLJIT_R3,D,SLJIT_R0);}
        else binary(a==26?SLJIT_ADD32:SLJIT_SUB32,SLJIT_R3,D,A);
        if(a!=5)store(&cpu.m_alu_r1,SLJIT_R3);
        if(a==8){constant(&cpu.m_alu_stset,0);return;}
        imm(SLJIT_R1,0);immediate(SLJIT_AND32,SLJIT_R0,SLJIT_R3,floating(a)?0x7fffffff:~0u);
        auto *zero=sljit_emit_cmp(c,SLJIT_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,0);auto *positive=sljit_emit_cmp(c,SLJIT_SIG_GREATER_EQUAL|SLJIT_32,SLJIT_R3,0,SLJIT_IMM,0);imm(SLJIT_R1,C::F_SGD);sljit_set_label(positive,sljit_emit_label(c));auto *done=sljit_emit_jump(c,SLJIT_JUMP);sljit_set_label(zero,sljit_emit_label(c));imm(SLJIT_R1,C::F_ZRD);sljit_set_label(done,sljit_emit_label(c));store(&cpu.m_alu_stset,SLJIT_R1);
    }
    void update_st(){load(SLJIT_R0,&cpu.m_alu_stset);immediate(SLJIT_AND32,ST,ST,~MASK);binary(SLJIT_OR32,ST,ST,SLJIT_R0);}
    void post1(unsigned a){if((a>=1&&a<=4)||a==14||(a>=22&&a<=27)){mov(D,SLJIT_R3);update_st();}}
    void post2(unsigned a){if(!floating(a))return;if(a==8){mov(P,SLJIT_R3);return;}if(a!=5)mov(D,SLJIT_R3);if(a==9||a==10||a==13)load(P,&cpu.m_alu_r2);update_st();}
    void guard_pair(unsigned pc,int left,int right){
        // GCC's choice of commutative operand order can change NaN payload
        // priority. Preserve the canonical handler for two nonfinite inputs.
        if(vfp_cache){
            auto *ordered=sljit_emit_fcmp(c,SLJIT_ORDERED|SLJIT_32,floating_register(left),0,floating_register(left),0);
            exits.push_back({sljit_emit_fcmp(c,SLJIT_UNORDERED|SLJIT_32,floating_register(right),0,floating_register(right),0),~0u,pc});
            sljit_set_label(ordered,sljit_emit_label(c));return;
        }
        immediate(SLJIT_AND32,SLJIT_R0,left,0x7f800000);
        auto *finite=sljit_emit_cmp(c,SLJIT_NOT_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,0x7f800000);
        immediate(SLJIT_AND32,SLJIT_R0,right,0x7f800000);
        exits.push_back({sljit_emit_cmp(c,SLJIT_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,0x7f800000),~0u,pc});
        sljit_set_label(finite,sljit_emit_label(c));
    }
    void guard(unsigned pc,u32 op){
        if(op>>26!=0&&op>>26!=7)return;
        unsigned a=op>>21&31;
        if(a==5||a==6||a==7||a==16)guard_pair(pc,D,A);
        if(a==8||a==9||a==10||a==13)guard_pair(pc,A,B);
        if(a==9||a==10||a==12)guard_pair(pc,D,P);
        if(a==19||a==20)guard_pair(pc,B,A);
    }
    bool rf(unsigned r){return (r&63)>=32&&(r&63)<48;}
    bool io_instruction(u32 op){
        if(!native_io)return false;
        const unsigned family=op>>26,r1=op&511,r2=op>>9&511,sub=op>>18&7;
        if(family>=16&&family<=31)return rf(op>>24);
        if(!alu_ok(op>>21&31))return false;
        if(family==0)return sub==0||sub==1;
        if(family!=7)return false;
        if(sub<=2)return true;
        if(sub!=7)return false;
        switch(r2>>6){
        case 0:return rf(r2);
        case 1:return readable(r2)||rf(r2);
        case 2:case 3:return rf(r2);
        case 4:return writable(r2)||rf(r2);
        case 6:return (rf(r1)&&(writable(r2)||rf(r2)))||(rf(r2)&&readable(r1));
        default:return false;
        }
    }
    static u32 SLJIT_FUNC io_read(C *state,u32 address){return state->m_bus->io_r(address);}
    static u32 SLJIT_FUNC rf_read(C *state,u32 address){return state->m_bus->rf_r(address);}
    static void SLJIT_FUNC io_write(C *state,u32 address,u32 value){state->m_bus->io_w(address,value);}
    static void SLJIT_FUNC rf_write(C *state,u32 address,u32 value){state->m_bus->rf_w(address,value);}
    void bus_context(unsigned pc){constant(&cpu.m_ppc,pc,SLJIT_MOV_U16);constant(&cpu.m_pc,pc+1,SLJIT_MOV_U16);store(&cpu.m_icount,SLJIT_S1);}
    void restore_alu(unsigned a){if(a&&a!=5)load(SLJIT_R3,&cpu.m_alu_r1);}
    // Address in R0; reads return in R0, writes consume R2. Cached CPU
    // datapath values are preserved by the ARM ABI and the bus capability.
    void bus_read(unsigned pc,bool reg,unsigned a){
        mov(SLJIT_R1,SLJIT_R0);bus_context(pc);mov(SLJIT_R0,SLJIT_S0);
        sljit_emit_icall(c,SLJIT_CALL,SLJIT_ARGS2(32,P,32),SLJIT_IMM,reg?SLJIT_FUNC_ADDR(rf_read):SLJIT_FUNC_ADDR(io_read));
        load(SLJIT_S1,&cpu.m_icount,SLJIT_MOV_S32);load(SLJIT_R1,&cpu.m_stall,SLJIT_MOV_U8);
        stalled_reads.push_back({sljit_emit_cmp(c,SLJIT_NOT_EQUAL|SLJIT_32,SLJIT_R1,0,SLJIT_IMM,0),pc});restore_alu(a);
    }
    void bus_write(unsigned pc,bool reg,unsigned a){
        mov(SLJIT_R1,SLJIT_R0);bus_context(pc);mov(SLJIT_R0,SLJIT_S0);
        sljit_emit_icall(c,SLJIT_CALL,SLJIT_ARGS3V(P,32,32),SLJIT_IMM,reg?SLJIT_FUNC_ADDR(rf_write):SLJIT_FUNC_ADDR(io_write));
        load(SLJIT_S1,&cpu.m_icount,SLJIT_MOV_S32);restore_alu(a);
    }
    void io_operation(unsigned pc,u32 op){
        unsigned family=op>>26,r1=op&511,r2=op>>9&511,sub=op>>18&7,a=op>>21&31;
        if(family>=16&&family<=31){imm(SLJIT_R2,u32(util::sext(op,24)));imm(SLJIT_R0,(op>>24)&31);bus_write(pc,true,0);return;}
        alu_pre(a);
        if(family==0){
            readmem(r1,false,0);local_store(0,SLJIT_R2);ea(r2,true);bus_read(pc,false,a);local_store(4,SLJIT_R0);
            post(r1,false);post(r2,true);local_load(A,0);local_load(B,4);post1(a);post2(a);return;
        }
        if(sub<=1){readmem(r1,false,0);post(r1,false);post1(a);ea(r2,true);bus_write(pc,false,a);post(r2,true);post2(a);return;}
        if(sub==2){ea(r1,false);bus_read(pc,false,a);mov(SLJIT_R2,SLJIT_R0);post(r1,false);post1(a);writemem(r2,0);post2(a);return;}
        switch(r2>>6){
        case 0:imm(SLJIT_R0,r2&31);bus_read(pc,true,a);mov(SLJIT_R2,SLJIT_R0);post1(a);writemem(r1,0);break;
        case 1:
            if(rf(r2)){imm(SLJIT_R0,r2&31);bus_read(pc,true,a);}else readreg(r2);
            mov(SLJIT_R2,SLJIT_R0);post1(a);ea(r1,true);bus_write(pc,false,a);post(r1,true);break;
        case 2:case 3:readmem(r1,true,(r2>>6)==2?512:0);post(r1,true);post1(a);imm(SLJIT_R0,r2&31);bus_write(pc,true,a);break;
        case 4:
            ea(r1,true);bus_read(pc,false,a);mov(SLJIT_R2,SLJIT_R0);post(r1,true);post1(a);
            if(rf(r2)){imm(SLJIT_R0,r2&31);bus_write(pc,true,a);}else{mov(SLJIT_R0,SLJIT_R2);writereg(r2);}break;
        case 6:
            if(rf(r1)){imm(SLJIT_R0,r1&31);bus_read(pc,true,a);}else readreg(r1);
            mov(SLJIT_R2,SLJIT_R0);post1(a);
            if(rf(r2)){imm(SLJIT_R0,r2&31);bus_write(pc,true,a);}else{mov(SLJIT_R0,SLJIT_R2);writereg(r2);}break;
        }
        post2(a);
    }
    bool accepts(u32 op){
        const auto family=op>>26,r1=op&511,r2=(op>>9)&511,sub=(op>>18)&7,alu=(op>>21)&31;
        if(io_instruction(op))return true;
        if(family==0)return (sub==3||sub==4)&&alu_ok(alu);
        if(family==7){if(!alu_ok(alu))return false;if(sub==3||sub==4)return true;if(sub!=7)return false;switch(r2>>6){case 0:return readable(r2);case 2:case 3:return writable(r2);case 6:return readable(r1)&&writable(r2);default:return false;}}
        if(family==47||family==63){
            const auto cond=op>>20&31,subtype=op>>17&7;
            if(!(cond<=2||cond==10||cond==11||cond==12||cond==16||cond==17||cond==18||cond==22))return false;
            if(subtype==0||subtype==2||subtype==5)return true;
            if(subtype==1||subtype==3)return !(op&0x4000)||readable(op);
            if(subtype==6)return writable(op>>9);
            return false;
        }
        if(family==14)return true;
        if(family>=16&&family<=31)return writable(op>>24);
        return false;
    }
    void operation(u32 op){
        unsigned family=op>>26,r1=op&511,r2=(op>>9)&511,sub=(op>>18)&7,a=(op>>21)&31;
        if(family==14){unsigned which=(op>>24)&3;if(!which){immediate(SLJIT_AND32,P,P,0xff000000);immediate(SLJIT_OR32,P,P,op&0xffffff);}else imm(which==1?A:which==2?B:D,u32(util::sext(op,24)));return;}
        if(family>=16&&family<=31){imm(SLJIT_R0,u32(util::sext(op,24)));writereg(op>>24);return;}
        alu_pre(a);
        if(vfp_cache&&family==0){readmem(r1,false,sub==4?512:0,A);readmem(r2,true,sub==3?512:0,B);post(r1,false);post(r2,true);post1(a);post2(a);return;}
        if(family==0){readmem(r1,false,sub==4?512:0);local_store(0,SLJIT_R2);readmem(r2,true,sub==3?512:0);post(r1,false);post(r2,true);local_load(A,0);mov(B,SLJIT_R2);post1(a);post2(a);return;}
        if(sub==3||sub==4){readmem(r1,false,sub==4?512:0);post(r1,false);post1(a);writemem(r2,sub==3?512:0);post2(a);return;}
        switch(r2>>6){
        case 0:if(vfp_cache&&cache(r2)&&!(cache(r2)==D&&integer_alu(a))){post1(a);writemem(r1,0,cache(r2));break;}readreg(r2);mov(SLJIT_R2,SLJIT_R0);post1(a);writemem(r1,0);break;
        case 2:case 3:if(vfp_cache&&cache(r2)&&!(cache(r2)==D&&integer_alu(a))){readmem(r1,true,(r2>>6)==2?512:0,cache(r2));post(r1,true);post1(a);break;}readmem(r1,true,(r2>>6)==2?512:0);post(r1,true);post1(a);mov(SLJIT_R0,SLJIT_R2);writereg(r2);break;
        case 6:readreg(r1);if((a>=1&&a<=4)||a==14||(a>=22&&a<=27)){mov(SLJIT_R2,SLJIT_R0);post1(a);mov(SLJIT_R0,SLJIT_R2);}writereg(r2);break;
        }
        post2(a);
    }
    void count(unsigned cost=1){immediate(SLJIT_ADD32,SLJIT_S2,SLJIT_S2,1);immediate(SLJIT_SUB32,SLJIT_S1,SLJIT_S1,cost);}
    void direct(unsigned pc,unsigned target){
        count();
        if(target<4096){exits.push_back({sljit_emit_cmp(c,SLJIT_SIG_LESS_EQUAL|SLJIT_32,SLJIT_S1,0,SLJIT_IMM,0),pc,target});links.push_back({sljit_emit_jump(c,SLJIT_JUMP),target});}
        else exits.push_back({sljit_emit_jump(c,SLJIT_JUMP),pc,target});
    }
    void condition(unsigned op){
        unsigned cond=op>>20&31;u32 mask=0;bool zero=false;
        switch(cond){case 0:mask=C::F_ZRD;break;case 1:mask=C::F_SGD;zero=true;break;case 2:mask=C::F_ZRD|C::F_SGD;break;case 16:mask=C::F_ZC0;zero=true;break;case 17:mask=C::F_ZC1;zero=true;break;}
        if(mask){sljit_emit_op2u(c,SLJIT_AND32|SLJIT_SET_Z,ST,0,SLJIT_IMM,sljit_sw(mask));sljit_emit_op_flags(c,SLJIT_MOV32,SLJIT_R3,0,zero?SLJIT_ZERO:SLJIT_NOT_ZERO);}
        else if(cond==22)imm(SLJIT_R3,1);
        else load(SLJIT_R3,cond==10?&cpu.m_gpio0:cond==11?&cpu.m_gpio1:cond==12?&cpu.m_gpio2:&cpu.m_gpio3,SLJIT_MOV_U8);
        if(op&0x40000000)immediate(SLJIT_XOR32,SLJIT_R3,SLJIT_R3,1);
        if((op>>17&7)<2&&(cond==16||cond==17)){
            auto *counter=cond==16?&cpu.m_c0:&cpu.m_c1;load(SLJIT_R0,counter,SLJIT_MOV_U8);
            auto *one=sljit_emit_cmp(c,SLJIT_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,1);
            immediate(SLJIT_SUB32,SLJIT_R0,SLJIT_R0,1);immediate(SLJIT_AND32,SLJIT_R0,SLJIT_R0,255);store(counter,SLJIT_R0,SLJIT_MOV_U8);
            auto *notone=sljit_emit_cmp(c,SLJIT_NOT_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,1);
            immediate(SLJIT_OR32,ST,ST,cond==16?C::F_ZC0:C::F_ZC1);auto *end=sljit_emit_label(c);sljit_set_label(one,end);sljit_set_label(notone,end);
        }
    }
    void push(unsigned pc){for(int i=3;i>0;--i){load(SLJIT_R1,&cpu.m_pcs[i-1],SLJIT_MOV_U16);store(&cpu.m_pcs[i],SLJIT_R1,SLJIT_MOV_U16);}constant(&cpu.m_pcs[0],pc+1,SLJIT_MOV_U16);}
    void branch(unsigned pc,u32 op){
        if((op>>20&31)==22&&(op&0x40000000)){direct(pc,pc+1);return;}
        sljit_jump *untaken=nullptr;
        if((op>>20&31)!=22){condition(op);untaken=sljit_emit_cmp(c,SLJIT_EQUAL|SLJIT_32,SLJIT_R3,0,SLJIT_IMM,0);}
        unsigned type=op>>17&7;
        if(type==0||type==2){if(type==2)push(pc);direct(pc,op&65535);}
        else if(type==6){readmem(op&511,false,0);post(op&511,false);mov(SLJIT_R0,SLJIT_R2);writereg(op>>9);direct(pc,pc+1);}
        else{
            if(type==5){load(SLJIT_R0,&cpu.m_pcs[0],SLJIT_MOV_U16);for(int i=0;i<3;++i){load(SLJIT_R1,&cpu.m_pcs[i+1],SLJIT_MOV_U16);store(&cpu.m_pcs[i],SLJIT_R1,SLJIT_MOV_U16);}}
            else{if(op&0x4000)readreg(op);else{readmem(op&511,false,0);post(op&511,false);mov(SLJIT_R0,SLJIT_R2);}if(type==3)push(pc);}
            store(&cpu.m_pc,SLJIT_R0,SLJIT_MOV_U16);constant(&cpu.m_ppc,pc,SLJIT_MOV_U16);count();dynamic_dispatch.push_back(sljit_emit_jump(c,SLJIT_JUMP));
        }
        if(untaken){sljit_set_label(untaken,sljit_emit_label(c));direct(pc,pc+1);}
    }
    static void SLJIT_FUNC helper(C *state){
        state->m_ppc=state->m_pc++;
        ++state->m_native_helper_slice;
        C::compiled_program()[state->m_ppc&4095].run(state);
    }
    void spill(){store(&cpu.m_icount,SLJIT_S1);store(&cpu.m_a,A);store(&cpu.m_b,B);store(&cpu.m_d,D);store(&cpu.m_p,P);store(&cpu.m_st,ST);}
    void reload(){load(SLJIT_S1,&cpu.m_icount,SLJIT_MOV_S32);load(A,&cpu.m_a);load(B,&cpu.m_b);load(D,&cpu.m_d);load(P,&cpu.m_p);load(ST,&cpu.m_st);}
    explicit mb86233_native_generator(C &state,bool io):cpu(state),native_io(io){
        if(!c)return;const u32 *words=cpu.m_bus->program_data();for(unsigned i=0;i<4096;++i)eligible[i]=accepts(words[i])&&i!=0xbc3&&i!=0xbfb&&i!=0xbfc;
        sljit_emit_enter(c,0,SLJIT_ARGS2(32,P,W),4|SLJIT_ENTER_FLOAT(2),(vfp_cache?4|SLJIT_ENTER_FLOAT(4):8),8);
        mov(SLJIT_R0,SLJIT_S1);reload();imm(SLJIT_S2,0);sljit_emit_ijump(c,SLJIT_JUMP,SLJIT_R0,0);
        std::vector<sljit_jump*> fallback;
        for(unsigned i=0;i<4096;++i){
            labels[i]=sljit_emit_label(c);
            if(!eligible[i]){constant(&cpu.m_pc,i,SLJIT_MOV_U16);fallback.push_back(sljit_emit_jump(c,SLJIT_JUMP));continue;}
            if(words[i]>>26==47||words[i]>>26==63){branch(i,words[i]);continue;}
            guard(i,words[i]);if(io_instruction(words[i]))io_operation(i,words[i]);else operation(words[i]);
            unsigned a=words[i]>>21&31;unsigned cost=(words[i]>>26==0||words[i]>>26==7)&&floating(a)?2:1;
            immediate(SLJIT_ADD32,SLJIT_S2,SLJIT_S2,1);immediate(SLJIT_SUB32,SLJIT_S1,SLJIT_S1,cost);
            if(i+1<4096)exits.push_back({sljit_emit_cmp(c,SLJIT_SIG_LESS_EQUAL|SLJIT_32,SLJIT_S1,0,SLJIT_IMM,0),i,i+1});
            else exits.push_back({sljit_emit_jump(c,SLJIT_JUMP),i,i+1});
        }
        // Repeat bodies preserve the normal path's cost: no repeat-state
        // load on ordinary instructions. The final repetition rejoins it.
        for(unsigned i=0;i<4096;++i)if(eligible[i]&&words[i]>>26!=47&&words[i]>>26!=63){
            repeat_labels[i]=sljit_emit_label(c);guard(i,words[i]);
            if(io_instruction(words[i]))io_operation(i,words[i]);else operation(words[i]);
            unsigned a=words[i]>>21&31;unsigned cost=(words[i]>>26==0||words[i]>>26==7)&&floating(a)?2:1;
            load(SLJIT_R0,&cpu.m_r,SLJIT_MOV_U8);
            auto *last=sljit_emit_cmp(c,SLJIT_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,1);
            immediate(SLJIT_SUB32,SLJIT_R0,SLJIT_R0,1);store(&cpu.m_r,SLJIT_R0,SLJIT_MOV_U8);count(cost);
            exits.push_back({sljit_emit_cmp(c,SLJIT_SIG_LESS_EQUAL|SLJIT_32,SLJIT_S1,0,SLJIT_IMM,0),i,i});
            sljit_set_label(sljit_emit_jump(c,SLJIT_JUMP),repeat_labels[i]);
            sljit_set_label(last,sljit_emit_label(c));count(cost);
            if(i+1<4096){exits.push_back({sljit_emit_cmp(c,SLJIT_SIG_LESS_EQUAL|SLJIT_32,SLJIT_S1,0,SLJIT_IMM,0),i,i+1});links.push_back({sljit_emit_jump(c,SLJIT_JUMP),i+1});}
            else exits.push_back({sljit_emit_jump(c,SLJIT_JUMP),i,i+1});
        }
        std::vector<sljit_jump*> finish;
        for(auto [jump,pc]:stalled_reads){
            sljit_set_label(jump,sljit_emit_label(c));constant(&cpu.m_stall,0,SLJIT_MOV_U8);
            constant(&cpu.m_ppc,pc,SLJIT_MOV_U16);constant(&cpu.m_pc,pc,SLJIT_MOV_U16);count(1);
            finish.push_back(sljit_emit_jump(c,SLJIT_JUMP));
        }
        for(auto &e:exits){sljit_set_label(e.jump,sljit_emit_label(c));if(e.pc!=~0u)constant(&cpu.m_ppc,e.pc,SLJIT_MOV_U16);constant(&cpu.m_pc,e.next,SLJIT_MOV_U16);if(e.pc==~0u)fallback.push_back(sljit_emit_jump(c,SLJIT_JUMP));else finish.push_back(sljit_emit_jump(c,SLJIT_JUMP));}
        auto *slow=sljit_emit_label(c);for(auto *j:fallback)sljit_set_label(j,slow);
        spill();immediate(SLJIT_ADD32,SLJIT_S2,SLJIT_S2,1);mov(SLJIT_R0,SLJIT_S0);
        sljit_emit_icall(c,SLJIT_CALL,SLJIT_ARGS1V(P),SLJIT_IMM,SLJIT_FUNC_ADDR(helper));reload();
        auto *dispatch=sljit_emit_label(c);for(auto *j:dynamic_dispatch)sljit_set_label(j,dispatch);
        finish.push_back(sljit_emit_cmp(c,SLJIT_SIG_LESS_EQUAL|SLJIT_32,SLJIT_S1,0,SLJIT_IMM,0));
        load(SLJIT_R0,&cpu.m_stall,SLJIT_MOV_U8);sljit_set_label(sljit_emit_cmp(c,SLJIT_NOT_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,0),slow);
        load(SLJIT_R0,&cpu.m_pc,SLJIT_MOV_U16);finish.push_back(sljit_emit_cmp(c,SLJIT_GREATER_EQUAL|SLJIT_32,SLJIT_R0,0,SLJIT_IMM,4096));
        load(SLJIT_R1,&cpu.m_r,SLJIT_MOV_U8);
        auto *normal=sljit_emit_cmp(c,SLJIT_EQUAL|SLJIT_32,SLJIT_R1,0,SLJIT_IMM,1);
        sljit_emit_op1(c,SLJIT_MOV,SLJIT_R1,0,SLJIT_IMM,reinterpret_cast<sljit_sw>(repeat_entries.data()));
        sljit_emit_op1(c,SLJIT_MOV,SLJIT_R2,0,SLJIT_MEM2(SLJIT_R1,SLJIT_R0),SLJIT_WORD_SHIFT);
        sljit_set_label(sljit_emit_cmp(c,SLJIT_EQUAL,SLJIT_R2,0,SLJIT_IMM,0),slow);
        sljit_emit_ijump(c,SLJIT_JUMP,SLJIT_R2,0);sljit_set_label(normal,sljit_emit_label(c));
        sljit_emit_op1(c,SLJIT_MOV,SLJIT_R1,0,SLJIT_IMM,reinterpret_cast<sljit_sw>(entries.data()));
        sljit_emit_op1(c,SLJIT_MOV,SLJIT_R0,0,SLJIT_MEM2(SLJIT_R1,SLJIT_R0),SLJIT_WORD_SHIFT);sljit_emit_ijump(c,SLJIT_JUMP,SLJIT_R0,0);
        auto *end=sljit_emit_label(c);for(auto *j:finish)sljit_set_label(j,end);spill();sljit_emit_return(c,SLJIT_MOV32,SLJIT_S2,0);
        for(auto &link:links)sljit_set_label(link.jump,labels[link.target]);
        code=sljit_generate_code(c,0,nullptr);
        if(code){enter=reinterpret_cast<fn>(code);for(unsigned i=0;i<4096;++i){entries[i]=sljit_get_label_addr(labels[i]);if(repeat_labels[i])repeat_entries[i]=sljit_get_label_addr(repeat_labels[i]);}std::fprintf(stderr,"Native DSP translator: %zu bytes, %s register cache, %s I/O\n",size_t(sljit_get_generated_code_size(c)),vfp_cache?"VFP":"integer",native_io?"native":"handler");}
        sljit_free_compiler(c);c=nullptr;
    }
    ~mb86233_native_generator(){if(c)sljit_free_compiler(c);if(code)sljit_free_code(code,nullptr);}
};
#endif
unsigned vf2_tgp_native(mb86233_device &cpu){
#if defined(__arm__)
    if(cpu.m_pc>=4096||cpu.m_stall||!cpu.m_fast_ram.low||!cpu.m_fast_ram.high)return 0;
    static const bool requested_io=std::getenv("VF2_TGP_NATIVE_IO")!=nullptr;
    const bool io=requested_io&&cpu.m_bus->native_control_callbacks();
    static std::unique_ptr<mb86233_native_generator> variants[2];
    auto &variant=variants[io?1:0];if(!variant)variant=std::make_unique<mb86233_native_generator>(cpu,io);
    auto &generator=*variant;
    const auto entry=cpu.m_r==1?generator.entries[cpu.m_pc]:generator.repeat_entries[cpu.m_pc];if(!entry||!generator.enter)return 0;
    cpu.m_native_helper_slice=0;
    const unsigned done=generator.enter(&cpu,entry),helpers=cpu.m_native_helper_slice;
    cpu.m_compiled_instructions+=done;cpu.m_native_instructions+=done-helpers;
    cpu.m_native_helpers+=helpers;cpu.m_native_entries+=1;return 1;
#else
    (void)cpu;return 0;
#endif
}
