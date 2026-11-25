// Immutable Model 2 ROM mesh topology, independent of camera and lighting.
#pragma once
#include <array>
#include <algorithm>
#include <cstddef>
#include <utility>
#include <bit>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>
namespace vf2_mesh {
using point=std::array<float,3>;
struct polygon {uint32_t attributes=0;uint8_t n=0;point normal{};std::array<point,4> position{};};
struct geometry {std::vector<polygon> polygons;size_t source_words=0;bool terminated=false,generated_normals=false;size_t bytes()const{return polygons.capacity()*sizeof(polygon);}};
inline bool decode(std::span<const uint32_t> words,uint32_t count,unsigned mode,geometry &result,size_t byte_limit=4*1024*1024){
    result={};if(words.size()<6)return false;result.generated_normals=(mode&2)!=0;
    size_t at=0;auto read=[&](){point p;for(auto &v:p)v=std::bit_cast<float>(words[at++]);return p;};
    point p0=read(),p1=read();if(!count)count=0xfffff;
    for(uint32_t i=0;i<count;++i){
        if(at==words.size()){result={};return false;}
        const uint32_t attr=words[at++];
        if(!(attr&3)){result.source_words=at;result.terminated=true;return true;}
        if(words.size()-at<9){result={};return false;}
        polygon p;p.attributes=attr;p.n=(attr&1)?4:3;p.normal=read();const point p2=read();point p3=read();if(p.n==3)p3=p2;
        p.position={p1,p0,p2,p3};
        const size_t max_polygons=byte_limit/sizeof(polygon);
        if(result.polygons.size()==max_polygons){result={};return false;}
        if(result.polygons.size()==result.polygons.capacity()){
            size_t capacity=result.polygons.capacity();capacity=capacity?capacity*2:std::min<size_t>(count,16);
            result.polygons.reserve(std::min(capacity,max_polygons));
        }
        result.polygons.push_back(p);
        switch((attr>>8)&3){case 0:case 2:p0=p2;p1=p3;break;case 1:p1=p2;break;case 3:p0=p3;break;}
    }
    result.source_words=at;return true;
}
struct key {uint32_t address,count,mode;bool operator==(const key&)const=default;};
struct key_hash {size_t operator()(const key &k)const{return (k.address*0x9e3779b1u)^(k.count*0x85ebca6bu)^(k.mode*0xc2b2ae35u);}};
class catalog {
    std::unordered_map<key,geometry,key_hash> entries;
    size_t used=0,limit;
public:
    uint64_t hits=0,misses=0,rejected=0;
    explicit catalog(size_t bytes=4*1024*1024):limit(bytes){}
    // The caller scopes one catalog to one immutable ROM image. RAM uses the
    // existing dynamic path; a new ROM image requires a new catalog.
    const geometry *get(key id,std::span<const uint32_t> source){
        auto found=entries.find(id);if(found!=entries.end()){++hits;return &found->second;}
        ++misses;geometry mesh;if(!decode(source,id.count,id.mode,mesh,limit-used)||mesh.bytes()>limit-used){++rejected;return nullptr;}
        used+=mesh.bytes();return &entries.emplace(id,std::move(mesh)).first->second;
    }
    size_t bytes()const{return used;}size_t size()const{return entries.size();}
};
}
