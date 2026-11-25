// Owned geometry input, independent of the machine's writable display RAM.
#pragma once
#include <bit>
#include <cfenv>
#include <cstring>
#include <cstddef>
#include <cstdint>
#include <vector>

enum class geometry_execution { synchronous, deferred, worker };
struct gpu3d_geometry_job {
    static constexpr std::size_t words=0x20000/sizeof(uint32_t);
    std::vector<uint32_t> commands;
    uint32_t start=0;
    uint64_t frame=0;
    std::fenv_t environment{};
    static constexpr std::size_t page_bytes=4096;
    static constexpr std::size_t page_words=page_bytes/sizeof(uint32_t);
    static_assert(words==32*page_words);
    bool capture(const uint32_t *source,uint32_t address,uint64_t number,uint32_t pages=~uint32_t(0)){
        if(std::fegetenv(&environment))return false;
        if(commands.size()!=words||pages==~uint32_t(0)){
            commands.assign(source,source+words);
        }else{
            while(pages){
                const std::size_t offset=std::countr_zero(pages)*page_words;
                std::memcpy(commands.data()+offset,source+offset,page_bytes);
                pages&=pages-1;
            }
        }
        start=address;frame=number;return true;
    }
};
