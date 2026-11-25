// Development checkpoints for repeated ARM builds. The sound board is held;
// numeric geometry state travels with the core state, without process pointers.
#pragma once
#include "../machine/model2a.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <zlib.h>
namespace acm_checkpoint {
struct header {
    char magic[8]={'V','F','2','A','C','M','1',0};
    uint32_t version=1,bits=sizeof(void*)*8;
    uint64_t rom=0;
    uint32_t core_size=0,geometry_size=0,crc=0,reserved=0;
};
static_assert(sizeof(header)==40);
inline uint32_t checksum(const std::vector<uint8_t>&a,const std::vector<uint8_t>&b){
    return crc32(crc32(0,a.data(),a.size()),b.data(),b.size());
}
// A checkpoint holds no sound-board state, so a load with the board running resumes the
// sound from power-on rather than in step with the game. VF2_AUDIO_EXPERIMENT lifts the
// guard for timing work only.
inline bool sound_guard(model2a &m,std::string &error){
    if(m.sound()&&!std::getenv("VF2_AUDIO_EXPERIMENT")){error="Checkpoints require --no-sound-board";return false;}
    return true;
}
inline bool save(model2a &m,const std::string &path,std::string &error){
    if(!sound_guard(m,error))return false;
    // The core is streamed straight to the file: building its 7 MB buffer
    // exhausts the Mini's RAM beside the render caches (the kernel kills the
    // process). The geometry part is small (a few hundred KB) and stays a vector.
    FILE *f=std::fopen(path.c_str(),"wb");if(!f){error="Cannot create checkpoint "+path;return false;}
    header h;
    bool ok=std::fwrite(&h,1,sizeof h,f)==sizeof h;
    size_t core_size=0;
    // The CRC is accumulated as the bytes are written, so nothing is read back.
    m2::FileStateWriter w(f);
    if(ok)ok=m.save_state_file(w,core_size);
    std::vector<uint8_t> geometry;
    if(ok){m.video().save_geometry(geometry);if(!geometry.empty())w.bytes(geometry.data(),geometry.size());}
    ok=ok&&!w.failed;
    if(ok)ok=std::fflush(f)==0;
    h.rom=m.rom_hash();h.core_size=core_size;h.geometry_size=geometry.size();h.crc=w.crc;
    if(ok)ok=std::fseek(f,0,SEEK_SET)==0&&std::fwrite(&h,1,sizeof h,f)==sizeof h;
    if(std::fclose(f))ok=false;
    if(!ok)error="Checkpoint write failed";return ok;
}
inline bool load(model2a &m,const std::string &path,std::string &error){
    if(!sound_guard(m,error))return false;
    FILE *f=std::fopen(path.c_str(),"rb");if(!f){error="Cannot open checkpoint "+path;return false;}
    header h,expected;
    bool ok=std::fread(&h,1,sizeof h,f)==sizeof h;
    if(!ok||std::memcmp(h.magic,expected.magic,8)||h.version!=1||h.bits!=sizeof(void*)*8||h.rom!=m.rom_hash()||h.core_size<m.fixed_state_size()||h.core_size>16*1024*1024||h.geometry_size>4*1024*1024){std::fclose(f);error="Incompatible checkpoint header";return false;}
    std::vector<uint8_t> core(h.core_size),geometry(h.geometry_size);
    ok=std::fread(core.data(),1,core.size(),f)==core.size()&&std::fread(geometry.data(),1,geometry.size(),f)==geometry.size()&&std::fgetc(f)==EOF;
    std::fclose(f);
    if(!ok||checksum(core,geometry)!=h.crc){error="Checkpoint is truncated or has a CRC mismatch";return false;}
    if(std::getenv("VF2_CHECK_HASH"))std::fprintf(stderr,"CHECK file core %016llx geometry %016llx\n",
        (unsigned long long)m2::state_hash(core.data()+model2a::STATE_HEADER,core.size()-model2a::STATE_HEADER),
        (unsigned long long)m2::state_hash(geometry.data(),geometry.size()));
    if(!m.load_state(core.data(),core.size())||!m.video().load_geometry(geometry.data(),geometry.size())){error="Checkpoint state validation failed";return false;}
    return true;
}
}
