// Signed texture batches for known scene and fighter selections.
#pragma once

#include "../gpu/gles2_atlas_decode.h"
#include <charconv>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

class gpu3d;
struct gpu3d_rams;
bool gles2_prewarm_atlas(gpu3d &,const gpu3d_rams &,const uint64_t *,size_t);

using vf2_batch::atlas_source_signature;
bool gles2_check_atlas_sources(const gpu3d_rams &,const atlas_source_signature *,size_t);

inline bool read_hex_word(std::istream &input,uint64_t &value){
    std::string token;
    if(!(input>>token)||token.size()!=16)return false;
    const auto end=token.data()+token.size();
    const auto parsed=std::from_chars(token.data(),end,value,16);
    return parsed.ec==std::errc{}&&parsed.ptr==end;
}

inline bool valid_atlas_key(uint64_t key){
    return !(key>>50)&&((key>>21)&4095)&&((key>>33)&4095);
}

struct atlas_state_recipe {
    std::array<unsigned,5> identity{};
    unsigned lock_mask=0,stable_ticks=0;
    atlas_source_signature sentinel;
    std::vector<std::vector<atlas_source_signature>> batches;

    bool read(std::istream &input,bool legacy,unsigned &total){
        unsigned count=0;
        for(auto &value:identity)if(!(input>>value)||value>0xffff)return false;
        if(legacy)lock_mask=identity[2]?3:1;
        else if(!(input>>lock_mask)||lock_mask>3)return false;
        if(!(input>>stable_ticks>>count)||!stable_ticks||stable_ticks>120||!count||count>1024)return false;
        const auto read_signature=[&](atlas_source_signature &s){
            return read_hex_word(input,s.key)&&valid_atlas_key(s.key)&&read_hex_word(input,s.hash);
        };
        if(!read_signature(sentinel))return false;
        for(unsigned i=0;i<count;++i){
            unsigned tiles=0;
            if(!(input>>tiles)||!tiles||tiles>65536-total)return false;
            total+=tiles;
            auto &batch=batches.emplace_back(tiles);
            for(auto &s:batch)if(!read_signature(s))return false;
        }
        return true;
    }
};

inline bool read_atlas_state_plan(const char *path,std::vector<atlas_state_recipe> &recipes){
    std::ifstream input(path);
    std::string magic;
    if(!(input>>magic))return false;
    const bool legacy=magic=="VF2_ATLAS_STATE_V1";
    unsigned count=1,total=0;
    if(!legacy&&(magic!="VF2_ATLAS_STATE_V2"||!(input>>count)||!count||count>32))return false;
    recipes.resize(count);
    for(auto &recipe:recipes)
        if(!recipe.read(input,legacy,total))return false;
    return !(input>>magic)&&input.eof();
}

struct atlas_preload_progress {
    size_t batch=0;
    std::array<uint32_t,2> revisions{};
    uint64_t changed=0;
    bool wait_for_revision=false;
};

inline unsigned atlas_fighter_id(const std::vector<uint8_t> &work,unsigned side){
    uint32_t base=0;
    std::memcpy(&base,work.data()+0x804+4*side,sizeof(base));
    if(base<0x500000||base-0x500000+0x1b4>=work.size())return 0xffff;
    const size_t at=base-0x500000;
    return unsigned(work[at+0x1b0])|(unsigned(work[at+0x1b4])<<8);
}

inline bool atlas_selection_locked(const std::vector<uint8_t> &work,unsigned side){
    uint32_t object=0;
    std::memcpy(&object,work.data()+0x83c+4*side,sizeof(object));
    if(object<0x500000||object-0x500000+4>=work.size())return false;
    const size_t at=object-0x500000;
    uint32_t flags=0;
    std::memcpy(&flags,work.data()+at,sizeof(flags));
    // A live selection object has bits 31 and 3. Bit 1 clears on confirmation.
    return work[at+4]==side&&(flags&0x8000000a)==0x80000008;
}
