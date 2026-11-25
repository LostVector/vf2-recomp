// Rendering may read this snapshot while the board advances on another thread.
#pragma once
#include "../gpu/gpu3d.h"
#include <array>
#include <vector>
#include <cstring>
#include <cstdlib>

class gpu3d_snapshot {
    std::vector<uint32_t> textures[2];
    std::array<uint8_t,0x8000> luma;
    std::array<uint16_t,0x6000> xlat;
    std::array<uint8_t,256> gamma;
    gpu3d_rams saved{};
    const void *source[4]={};
    const void *page_source[2]={};
    std::array<uint32_t,gpu3d_rams::texture_pages> page_versions[2]{};
    uint64_t texture_bytes_copied=0;
    bool valid=false;
public:
    void capture(const gpu3d_rams &r){
        for(unsigned i=0;i<2;i++){
            static const bool full_copy=std::getenv("VF2_FULL_TEXTURE_SNAPSHOT")!=nullptr;
            const bool full=!valid||source[i]!=r.tex[i]||page_source[i]!=r.tex_page_gen[i];
            if(full||saved.gen[i]!=r.gen[i]){
                if(full||full_copy||!r.tex_page_gen[i]){
                    textures[i].assign(r.tex[i],r.tex[i]+0x80000);texture_bytes_copied+=0x200000;
                }else for(size_t page=0;page<gpu3d_rams::texture_pages;++page){
                    if(page_versions[i][page]==r.tex_page_gen[i][page])continue;
                    const size_t offset=page*gpu3d_rams::texture_page_words;
                    std::memcpy(textures[i].data()+offset,r.tex[i]+offset,gpu3d_rams::texture_page_words*4);
                    texture_bytes_copied+=gpu3d_rams::texture_page_words*4;
                }
                if(r.tex_page_gen[i])std::memcpy(page_versions[i].data(),r.tex_page_gen[i],sizeof(page_versions[i]));
            }
            source[i]=r.tex[i];
            page_source[i]=r.tex_page_gen[i];
        }
        if(!valid||saved.gen[2]!=r.gen[2]||source[2]!=r.luma)std::memcpy(luma.data(),r.luma,luma.size());
        if(!valid||saved.gen[3]!=r.gen[3]||source[3]!=r.xlat)std::memcpy(xlat.data(),r.xlat,xlat.size()*sizeof(uint16_t));
        source[2]=r.luma;source[3]=r.xlat;std::memcpy(gamma.data(),r.gamma,gamma.size());
        saved=r;saved.tex[0]=textures[0].data();saved.tex[1]=textures[1].data();saved.luma=luma.data();saved.xlat=xlat.data();saved.gamma=gamma.data();valid=true;
        for(unsigned i=0;i<2;++i)saved.tex_page_gen[i]=r.tex_page_gen[i]?page_versions[i].data():nullptr;
    }
    const gpu3d_rams &rams()const{return saved;}
    uint64_t copied_texture_bytes()const{return texture_bytes_copied;}
};
