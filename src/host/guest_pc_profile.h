#pragma once
#include "../machine/model2a.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

// A bounded diagnostic run, never the timing baseline. i960 counters live in
// emitted native code so enabling them preserves its idle-loop skipping.
class guest_pc_profile {
    std::string path;
    std::array<uint64_t,4096> tgp{};
    std::array<uint64_t,4096> consecutive{};
    static constexpr unsigned command_count=112;
    std::array<uint64_t,command_count+1> command_calls{},command_attempts{},command_repeats{};
    std::array<uint16_t,command_count> command_targets{};
    unsigned current_command=command_count;
    bool command_table=false;
    uint16_t previous=0;bool have_previous=false;
public:
    void start(model2a &m) {
        const char *p=std::getenv("VF2_GUEST_COUNTS");if(!p)return;
        path=p;
        auto *r=m.cpu().rec();
        std::fill(r->native_pc.begin(),r->native_pc.end(),0);
        std::fill(r->interp_pc.begin(),r->interp_pc.end(),0);
        command_table=true;
        for(unsigned i=0;i<command_count;++i){
            const uint32_t word=m.tgp().program_word(uint16_t(0x3f+i));
            if((word&0xffff0000u)!=0xbf600000u)command_table=false;
            command_targets[i]=uint16_t(word);
        }
        m.tgp().on_instruction=[this](uint16_t pc){
            const bool repeated=have_previous&&previous==pc;
            ++tgp[pc&4095];if(repeated)++consecutive[pc&4095];
            if(command_table){
                if(pc==0x35)current_command=command_count;
                else if(pc>=0x3f&&pc<0x3f+command_count){current_command=pc-0x3f;++command_calls[current_command];}
                ++command_attempts[current_command];if(repeated)++command_repeats[current_command];
            }
            previous=pc;have_previous=true;
        };
    }
    bool finish(model2a &m) {
        if(path.empty())return true;
        m.tgp().on_instruction={};
        auto *r=m.cpu().rec();
        FILE *f=std::fopen((path+".i960").c_str(),"w");if(!f)return false;
        std::fprintf(f,"# pc native interpreter inline opcode function\n");
        for(size_t i=0;i<r->native_pc.size();++i){
            const auto fallback=i<r->interp_pc.size()?r->interp_pc[i]:0;
            if(!r->native_pc[i]&&!fallback)continue;
            const uint32_t pc=uint32_t(i*4);int owner=r->native_program->owner_of(pc);
            std::fprintf(f,"%08x %u %u %u %08x %08x\n",pc,r->native_pc[i],fallback,r->native_inline[i],m.peek32(pc),owner<0?0:r->native_program->functions[owner].entry);
        }
        bool ok=std::fclose(f)==0;
        f=std::fopen((path+".tgp").c_str(),"w");if(!f)return false;
        std::fprintf(f,"# pc attempts opcode consecutive_same_pc\n");
        for(size_t i=0;i<tgp.size();++i)if(tgp[i])std::fprintf(f,"%04x %llu %08x %llu\n",unsigned(i),(unsigned long long)tgp[i],m.tgp().program_word(uint16_t(i)),(unsigned long long)consecutive[i]);
        ok=std::fclose(f)==0&&ok;
        f=std::fopen((path+".commands").c_str(),"w");if(!f)return false;
        std::fprintf(f,"# command entry calls attempts consecutive_same_pc (ffff: dispatch or initial partial command; table_valid %d)\n",command_table);
        for(unsigned i=0;i<=command_count;++i)if(command_attempts[i])std::fprintf(f,"%04x %04x %llu %llu %llu\n",i==command_count?0xffff:i,i==command_count?0:command_targets[i],(unsigned long long)command_calls[i],(unsigned long long)command_attempts[i],(unsigned long long)command_repeats[i]);
        return std::fclose(f)==0&&ok;
    }
};
