// Snapshot ownership and sparse-copy correctness, without a graphics context.
#include "../../src/host/gpu3d_snapshot.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

static void require(bool ok,const char *message){if(!ok){std::fprintf(stderr,"Snapshot failed: %s\n",message);std::exit(1);}}
int main(){
    std::vector<uint32_t> tex[2];
    std::array<uint32_t,gpu3d_rams::texture_pages> pages[2];
    std::array<uint8_t,0x8000> luma{};std::array<uint16_t,0x6000> xlat{};std::array<uint8_t,256> gamma{};
    gpu3d_rams r;gpu3d_snapshot snapshot;
    for(unsigned bank=0;bank<2;++bank){
        tex[bank].resize(0x80000);pages[bank].fill(1);
        for(size_t i=0;i<tex[bank].size();++i)tex[bank][i]=uint32_t(i*257+bank);
        r.tex[bank]=tex[bank].data();r.tex_page_gen[bank]=pages[bank].data();r.gen[bank]=1;
    }
    r.luma=luma.data();r.xlat=xlat.data();r.gamma=gamma.data();
    auto check=[&](){
        snapshot.capture(r);const auto &saved=snapshot.rams();
        for(unsigned bank=0;bank<2;++bank){
            require(std::equal(r.tex[bank],r.tex[bank]+0x80000,saved.tex[bank]),"texture bytes");
            require(saved.tex[bank]!=r.tex[bank],"texture ownership");
            if(r.tex_page_gen[bank]){
                require(saved.tex_page_gen[bank]!=r.tex_page_gen[bank],"page metadata ownership");
                require(std::equal(pages[bank].begin(),pages[bank].end(),saved.tex_page_gen[bank]),"page revisions");
            }
        }
        require(!std::memcmp(saved.luma,luma.data(),luma.size()),"luma bytes");
        require(!std::memcmp(saved.xlat,xlat.data(),xlat.size()*2),"translation bytes");
        require(!std::memcmp(saved.gamma,gamma.data(),gamma.size()),"gamma bytes");
    };
    check();require(snapshot.copied_texture_bytes()==0x400000,"initial full copy");
    check();require(snapshot.copied_texture_bytes()==0x400000,"unchanged capture copies nothing");
    for(unsigned offset:{1023u,1024u}){tex[0][offset]^=0xff;++pages[0][offset/1024];++r.gen[0];}
    tex[1].back()^=0xff;++pages[1].back();++r.gen[1];check();
    if(!std::getenv("VF2_FULL_TEXTURE_SNAPSHOT"))require(snapshot.copied_texture_bytes()==0x400000+3*4096,"only three dirty pages copied");
    const auto saved_word=snapshot.rams().tex[0][0],saved_page=snapshot.rams().tex_page_gen[0][0];
    ++tex[0][0];++pages[0][0];++r.gen[0];
    require(snapshot.rams().tex[0][0]==saved_word&&snapshot.rams().tex_page_gen[0][0]==saved_page,"later board changes cannot mutate snapshot");
    check();
    std::fill(tex[0].begin(),tex[0].end(),12345);for(auto &p:pages[0])++p;++r.gen[0];
    luma[19]=9;++r.gen[2];xlat[37]=71;++r.gen[3];gamma[255]=31;check();
    auto replacement=tex[0];replacement[99]^=123;r.tex[0]=replacement.data();check();
    r.tex_page_gen[0]=nullptr;replacement[1701]^=17;++r.gen[0];check();
    r.tex_page_gen[0]=pages[0].data();check();
    std::puts("Snapshot passed: full/sparse copies, page boundaries, source replacement, invalidation, legacy input and ownership");
}
